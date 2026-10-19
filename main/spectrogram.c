#include "atomspectra.h"
#include "spectrogram.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_littlefs.h"
#include "sd_card.h"   // #SD-5: сегменты водопада — только на SD-карте
#include "esp_timer.h"   // #FW-21: монотонные часы для elapsed_sec (не зависят от NTP)
#include "nvs.h"   // #FW-15: настройки интервала/persist переживают ребут и clr_wf
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <inttypes.h>
#include <sys/stat.h>
#include <dirent.h>
#include "hang_watch.h"   // #HANG-1

static const char *TAG = "wf";

// #SD-5 (решение пользователя 2026-09-29): сегменты — ТОЛЬКО на SD-карте. Запись
// строки во внутренний флеш выключала кэш, и USB-приём терял свипы прибора
// («histogram sweep dropped»: 10 за 20 с при интервале 2 с, без записи — 1).
// Нет карты — строки остаются только в кольце ОЗУ, status.storage_err = true.
// Старые сегменты во флеше (/storage/wf) не трогаются и больше не видны.
#define WF_SEG_DIR        SD_MOUNT_POINT "/wf"        // каталог сегментов seg_NNNNN.aswf
#define WF_DATA           STORAGE_PATH "/wf_data.bin" // legacy (#REC-6) — только очистка
#define WF_META           STORAGE_PATH "/wf_meta.json"// legacy (#REC-6) — только очистка
#define WF_STATE          STORAGE_PATH "/wf_state.bin"
#define WF_FLASH_RESERVE  (1024 * 1024)
#define WF_SEG_PREP_ROW   (WF_SEG_MAX_ROWS / 2)  // #FW-8: фаза заблаговременного освобождения места кольцом
#define WF_STATE_MAGIC    0x53465731u   /* 'WFS1' — persist-состояние записи (#REC-6) */
// #FW-23: порог «время точно синхронизировано» — 2023-11-14. usb_host_cdc_init()
// (autostart водопада на первом USB-коннекте) стартует РАНЬШЕ init_sntp() в
// main.c, поэтому started_at при boot-автозапуске может зафиксироваться
// near-epoch-0 (1970), если ESP32 ещё не получил первый SNTP-ответ.
#define WF_SANE_EPOCH     1700000000

// #REC-11-A1/#FW-14: поля saved_rows/saved_at в шапке .aswf идут ПЕРВЫМИ,
// фикс. шириной WF_F_W (формат сохранён для совместимости читателей), но с
// #FW-14 всегда пишутся нулями и НЕ патчатся: 0 = «строк — из размера файла».
// JSON допускает пробелы перед значением → число выравнивается справа ("%*u").
#define WF_F_PRE        "{\"saved_rows\":"
#define WF_F_MID        ",\"saved_at\":"
#define WF_F_W          10

static uint16_t        *s_ring;
static uint32_t         s_capacity;
static uint32_t         s_head;
static uint32_t         s_count;
static uint16_t        *s_row;
static uint32_t        *s_prev;
static uint32_t         s_prev_total;
static uint16_t        *s_dur;       // #FW-5: кольцо реальных длительностей строк (сек), параллельно s_ring
static float           *s_temp;      // #FW-41: кольцо t1 детектора (°C), параллельно s_ring (NaN пока -inf не прочитан)
static uint32_t         s_prev_time; // #FW-5: предыдущее device total_time_sec (дельта = живое время среза)
static spectrum_data_t *s_snap;     // start()/seg_header_build() (serial/calib для шапки)
static spectrum_data_t *s_wf_snap;  // приватный буфер периодического wf_task (P3-4)
static uint32_t        *s_baseline;     // PSRAM: 8192×uint32 — снимок накопительного спектра при start()
static float            s_dose_k;       // µSv/h per cps из NVS (0.0 → NaN в dose_rate строк)
static float           *s_dose_lut;     // PSRAM: 8192×float LUT кривой МД (NULL → scalar k)
static int              s_dose_curve_n; // точек загружено (0 → scalar k)

static wf_status_t       s_status;
static wf_row_cb_t       s_row_cb;
// #FW-21: монотонная база старта ТЕКУЩЕЙ сессии записи (esp_timer, мкс от boot).
// Заводится в start() и restore() (обе точки total_rows=0). Из неё get_status
// считает elapsed_sec — честное «Время записи» без зависимости от NTP/wall-time.
// 0 = запись не идёт. Под s_lock (как и s_status).
static int64_t           s_started_uptime_us;
static SemaphoreHandle_t s_lock;     // защищает s_status / кольцо
static SemaphoreHandle_t s_fs_lock;  // защищает файловые операции с сегментами

// Состояние текущего открытого сегмента (под s_fs_lock).
static FILE     *s_seg_fp;
static uint32_t  s_seg_cur = 0xFFFFFFFFu;  // индекс открытого сегмента (0xFFFFFFFF = нет)
static uint32_t  s_seg_next;               // следующий индекс для нового сегмента
static uint32_t  s_seg_rows;               // строк записано в текущий открытый сегмент
static long      s_seg_opened_at;          // время открытия текущего сегмента (epoch с)
static uint32_t  s_seg_pinned = 0xFFFFFFFFu; // #REC-11-A2: сегмент в процессе выгрузки (claim) — кольцо его не трогает
static uint32_t  s_seg_seq;                // #DATA-1b: глоб. монотонный номер сегмента (NVS-персист, переживает clear/ребут)
static uint32_t  s_seg_total_at_open;      // #DATA-1c: device cumulative total на момент открытия текущего сегмента
static char      s_hdr[WF_HDR_RESERVE];    // буфер сборки шапки (только под s_fs_lock)

// #FW-6: расцепление acquisition↔flash. wf_task (producer) только набирает строки
// в кольцо и будит wf_fs_task (consumer), который пишет сегменты в своём темпе —
// латентность стирания флеша (1МБ unlink на границе ~31с) больше НЕ тормозит такт.
static SemaphoreHandle_t s_fs_sig;     // будит consumer на новую строку
// #FW-13 фикс №2: коммит свипа спектра (конец USB-burst) будит producer — снапшот
// и flash-запись строки уходят в тихое окно, а не в случайную фазу 1-с тика.
static SemaphoreHandle_t s_commit_sig;
static uint32_t          s_fs_flushed; // глоб. индекс следующей строки к записи на флеш
static uint8_t          *s_fs_buf;     // PSRAM-буфер строки для consumer (16384 Б)
static uint16_t          s_fs_dur;     // длительность копируемой строки (только в consumer)
static float             s_fs_temp;    // #FW-41: t1 копируемой строки (только в consumer)

#define LOCK()     do { if (s_lock)    xSemaphoreTake(s_lock,    portMAX_DELAY); } while (0)
#define UNLOCK()   do { if (s_lock)    xSemaphoreGive(s_lock);    } while (0)
// ВАЖНО про порядок захвата: разрешено брать LOCK ВНУТРИ FSLOCK (FS→status),
// и НИКОГДА наоборот. spectrogram_get_status берёт только LOCK — взаимоблокировки нет.
#define FSLOCK()   do { if (s_fs_lock) xSemaphoreTake(s_fs_lock, portMAX_DELAY); } while (0)
#define FSUNLOCK() do { if (s_fs_lock) xSemaphoreGive(s_fs_lock); } while (0)

static void wf_task(void *arg);
// #WF-4: после возобновления записи (restore) опора первой строки — первый СВЕЖИЙ
// свип этой загрузки, а не спектр из автосохранения (до 60 с до перезагрузки):
// иначе первая строка охватывала всё время простоя (замер: 74 с, 46 тыс. отсчётов
// в одной строке — яркая полоса). Импульсы простоя остаются в накопленном спектре.
static volatile bool s_rebase;
static void wf_fs_task(void *arg);                            // #FW-6 consumer
static void seg_write_row(const uint8_t *row, uint16_t dur, float temp);  // #FW-6/#FW-41 (под s_fs_lock сам)

// #REC-6: переживает ребут/сбой питания. Пишется на start/stop/clear; читается
// на boot в spectrogram_restore(). Решает, возобновлять ли запись.
typedef struct {
    uint32_t magic;
    uint32_t interval_sec;
    int64_t  started_at;     // time_t — непрерывная шкала времени N42
    uint8_t  active;         // запись была активна на момент сохранения
    uint8_t  persist;        // писалось во Flash
    uint8_t  _pad[6];
} wf_state_t;

static void write_state(bool active)
{
    wf_state_t st = {
        .magic        = WF_STATE_MAGIC,
        .interval_sec = s_status.interval_sec,
        .started_at   = (int64_t)s_status.started_at,
        .active       = active ? 1 : 0,
        .persist      = s_status.persist ? 1 : 0,
    };
    FILE *f = fopen(WF_STATE, "wb");
    if (!f) { ESP_LOGW(TAG, "cannot write state file"); return; }
    fwrite(&st, 1, sizeof(st), f);
    fclose(f);
}

// #FW-15: interval/persist — пользовательские НАСТРОЙКИ, а не state записи.
// WF_STATE стирается clr_wf=1 (FW-3) на буте, и автостарт (FW-2) поднимал бы
// compile-time дефолт (5 с). NVS переживает clear/ребут — настройки живут здесь.
#define WF_SETTINGS_NS "wf"

static void settings_load(void)
{
    nvs_handle_t h;
    if (nvs_open(WF_SETTINGS_NS, NVS_READONLY, &h) != ESP_OK) return;  // нет записи — дефолты
    uint32_t iv = 0; uint8_t ps = 0;
    if (nvs_get_u32(h, "interval", &iv) == ESP_OK &&
        iv >= WF_INTERVAL_MIN && iv <= WF_INTERVAL_MAX)
        s_status.interval_sec = iv;
    if (nvs_get_u8(h, "persist", &ps) == ESP_OK)
        s_status.persist = (ps != 0);
    uint32_t dose_bits = 0;
    if (nvs_get_u32(h, "dose_k_bits", &dose_bits) == ESP_OK)
        memcpy(&s_dose_k, &dose_bits, 4);
    uint32_t seq = 0;                       // #DATA-1b: глоб. номер сегмента переживает clear/ребут
    if (nvs_get_u32(h, "seg_seq", &seq) == ESP_OK)
        s_seg_seq = seq;
    nvs_close(h);
}

#define DOSE_CURVE_PATH       "/storage/dose_curve.csv"
#define DOSE_CURVE_MAX_POINTS 512

static void build_dose_lut(const float *ch_arr, const float *k_arr, int n)
{
    if (!s_dose_lut || n <= 0) return;
    int pi = 0;
    for (int ci = 0; ci < WF_CHANNELS; ci++) {
        float ch = (float)ci;
        if (n == 1) { s_dose_lut[ci] = k_arr[0]; continue; }
        if (ch <= ch_arr[0])   { s_dose_lut[ci] = k_arr[0];   continue; }
        if (ch >= ch_arr[n-1]) { s_dose_lut[ci] = k_arr[n-1]; continue; }
        while (pi + 1 < n - 1 && ch_arr[pi + 1] <= ch) pi++;
        float t = (ch - ch_arr[pi]) / (ch_arr[pi+1] - ch_arr[pi]);
        s_dose_lut[ci] = k_arr[pi] + t * (k_arr[pi+1] - k_arr[pi]);
    }
}

void spectrogram_load_dose_curve(void)
{
    if (!s_dose_lut) { s_dose_curve_n = 0; return; }
    static float ch_buf[DOSE_CURVE_MAX_POINTS];
    static float k_buf[DOSE_CURVE_MAX_POINTS];
    int n = 0;
    FILE *f = fopen(DOSE_CURVE_PATH, "r");
    if (!f) { s_dose_curve_n = 0; ESP_LOGI(TAG, "dose_curve: no file"); return; }
    char line[80];
    while (fgets(line, sizeof(line), f) && n < DOSE_CURVE_MAX_POINTS) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\r' || *p == '\0') continue;
        if (*p != '.' && (*p < '0' || *p > '9')) continue; // заголовок
        float ch, k;
        if (sscanf(p, "%f,%f", &ch, &k) == 2 && ch >= 0.0f && ch < WF_CHANNELS && k >= 0.0f)
            ch_buf[n] = ch, k_buf[n] = k, n++;
    }
    fclose(f);
    if (n < 1) { s_dose_curve_n = 0; ESP_LOGW(TAG, "dose_curve: 0 valid points"); return; }
    // insertion sort по channel
    for (int i = 1; i < n; i++) {
        float tc = ch_buf[i], tk = k_buf[i]; int j = i - 1;
        while (j >= 0 && ch_buf[j] > tc) { ch_buf[j+1] = ch_buf[j]; k_buf[j+1] = k_buf[j]; j--; }
        ch_buf[j+1] = tc; k_buf[j+1] = tk;
    }
    build_dose_lut(ch_buf, k_buf, n);
    s_dose_curve_n = n;
    ESP_LOGI(TAG, "dose_curve: %d points loaded", n);
}

int spectrogram_get_dose_curve_n(void) { return s_dose_curve_n; }

float spectrogram_compute_dose_rate(const uint16_t *bins, uint32_t dur)
{
    uint32_t nan_bits = 0x7FC00000u;
    float result;
    memcpy(&result, &nan_bits, 4);
    if (dur == 0) return result;
    if (s_dose_lut && s_dose_curve_n > 0) {
        double wsum = 0.0;
        for (int i = 0; i < WF_CHANNELS; i++) wsum += (double)bins[i] * s_dose_lut[i];
        result = (float)(wsum / (double)dur);
    } else if (s_dose_k > 0.0f) {
        uint64_t total = 0;
        for (int i = 0; i < WF_CHANNELS; i++) total += bins[i];
        result = ((float)total / (float)dur) * s_dose_k;
    }
    return result;
}

static void settings_save(uint32_t interval_sec, bool persist)
{
    nvs_handle_t h;
    if (nvs_open(WF_SETTINGS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "cannot save wf settings");
        return;
    }
    nvs_set_u32(h, "interval", interval_sec);
    nvs_set_u8(h, "persist", persist ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
}

// #SD-5: место под сегменты — на SD-карте (имя историческое: кольцо keep-last).
static uint32_t flash_free_bytes(void)
{
    uint64_t total = 0, free_b = 0;
    if (!sd_card_space(&total, &free_b)) return 0;
    return free_b > UINT32_MAX ? UINT32_MAX : (uint32_t)free_b;
}

// ----------------------------------------------------------------------------
//  Сегменты .aswf
// ----------------------------------------------------------------------------

static void seg_path(char *out, size_t cap, uint32_t idx)
{
    snprintf(out, cap, WF_SEG_DIR "/seg_%05" PRIu32 ".aswf", idx);
}

// #DATA-1a: стандартный CRC32 (init 0xFFFFFFFF, poly 0xEDB88320 рефлексия, финальный
// XOR 0xFFFFFFFF) — zlib-совместим, тот же алгоритм, что #CMD-1 (spectrum.c). Аккумулятивный:
// вызывающий стартует с 0xFFFFFFFF, применяет по чанкам, финализирует ^0xFFFFFFFF.
static uint32_t crc32_upd(uint32_t crc, const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int j = 0; j < 8; j++)
            crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320u : (crc >> 1);
    }
    return crc;
}

// Разбор имени seg_NNNNN.aswf → индекс. false, если имя не подходит.
static bool seg_name_index(const char *name, uint32_t *idx)
{
    if (strncmp(name, "seg_", 4) != 0) return false;
    const char *p = name + 4;
    char *end = NULL;
    unsigned long v = strtoul(p, &end, 10);
    if (end == p) return false;                  // ни одной цифры
    if (strcmp(end, ".aswf") != 0) return false; // неверный суффикс
    *idx = (uint32_t)v;
    return true;
}

// Собрать JSON-шапку сегмента v5 в s_hdr (добита пробелами до WF_HDR_RESERVE).
// saved_rows/saved_at — первыми, фикс. ширины (#FW-14: всегда 0). started_at —
// время открытия ЭТОГО сегмента. row_fields — полное самоописание строки v5
// (#FW-41: +temperature float32 перед crc32).
static void seg_header_build(uint32_t rows, long saved_at, long started_at)
{
    int cap = (int)sizeof(s_hdr);
    int n = snprintf(s_hdr, sizeof(s_hdr),
        WF_F_PRE "%*" PRIu32 WF_F_MID "%*ld"
        ",\"format\":\"atomspectra-waterfall\",\"version\":5"
        ",\"channels\":%d,\"dtype\":\"uint16\",\"byte_order\":\"little\""
        ",\"row_stride\":%d"
        ",\"row_fields\":["
        "{\"name\":\"spectrum\",\"dtype\":\"uint16\",\"channels\":%d,\"offset\":0},"
        "{\"name\":\"duration\",\"dtype\":\"uint16\",\"unit\":\"sec\",\"offset\":%d},"
        "{\"name\":\"timestamp\",\"dtype\":\"uint32\",\"unit\":\"unix_sec\",\"offset\":%d},"
        "{\"name\":\"latitude\",\"dtype\":\"float32\",\"unit\":\"deg\",\"offset\":%d},"
        "{\"name\":\"longitude\",\"dtype\":\"float32\",\"unit\":\"deg\",\"offset\":%d},"
        "{\"name\":\"dose_rate\",\"dtype\":\"float32\",\"unit\":\"usv_h\",\"offset\":%d},"
        "{\"name\":\"temperature\",\"dtype\":\"float32\",\"unit\":\"celsius\",\"offset\":%d},"
        "{\"name\":\"crc32\",\"dtype\":\"uint32\",\"algo\":\"crc32\",\"covers\":%d,\"offset\":%d}"
        "]"
        ",\"baseline\":{\"dtype\":\"uint32\",\"channels\":%d,\"byte_order\":\"little\"}"
        ",\"compressed\":false"
        ",\"seg_seq\":%" PRIu32 ",\"total_at_open\":%" PRIu32
        ",\"interval_sec\":%" PRIu32 ",\"started_at\":%ld",
        WF_F_W, rows, WF_F_W, saved_at,
        WF_CHANNELS, WF_ROW_STRIDE,
        WF_CHANNELS,                                              // spectrum.channels
        WF_ROW_BYTES,                                             // duration.offset=16384
        WF_ROW_BYTES + WF_DUR_BYTES,                             // timestamp.offset=16386
        WF_ROW_BYTES + WF_DUR_BYTES + WF_TS_BYTES,              // latitude.offset=16390
        WF_ROW_BYTES + WF_DUR_BYTES + WF_TS_BYTES + 4,         // longitude.offset=16394
        WF_ROW_BYTES + WF_DUR_BYTES + WF_TS_BYTES + 8,         // dose_rate.offset=16398
        WF_ROW_BYTES + WF_DUR_BYTES + WF_TS_BYTES + 12,        // temperature.offset=16402
        WF_ROW_PRECRC,                                            // crc32.covers=16406
        WF_ROW_PRECRC,                                            // crc32.offset=16406
        WF_CHANNELS,                                              // baseline.channels
        s_seg_seq, s_seg_total_at_open,                          // #DATA-1b/1c
        s_status.interval_sec, started_at);
    if (n > 0 && n < cap && s_snap && s_snap->serial_number[0])
        n += snprintf(s_hdr + n, cap - n, ",\"serial\":\"%s\"", s_snap->serial_number);
    if (n > 0 && n < cap && s_snap && s_snap->calib_valid) {
        n += snprintf(s_hdr + n, cap - n, ",\"calibration\":[");
        for (int i = 0; i <= s_snap->calib_order && n > 0 && n < cap; i++)
            n += snprintf(s_hdr + n, cap - n, "%s%.15g", i ? "," : "", s_snap->calibration[i]);
        if (n > 0 && n < cap) n += snprintf(s_hdr + n, cap - n, "]");
    }
    if (n > 0 && n < cap) n += snprintf(s_hdr + n, cap - n, "}");
    if (n < 0)   n = 0;
    if (n > cap) n = cap;
    memset(s_hdr + n, ' ', cap - n);
}

// Записать префикс "ASWF"+u32(reserve) и s_hdr с начала файла.
static bool seg_write_full_header(FILE *f)
{
    uint8_t pre[8];
    uint32_t reserve = WF_HDR_RESERVE;
    pre[0] = 'A'; pre[1] = 'S'; pre[2] = 'W'; pre[3] = 'F';
    pre[4] = (uint8_t)(reserve);       pre[5] = (uint8_t)(reserve >> 8);
    pre[6] = (uint8_t)(reserve >> 16); pre[7] = (uint8_t)(reserve >> 24);
    if (fseek(f, 0, SEEK_SET) != 0) return false;
    if (fwrite(pre, 1, 8, f) != 8) return false;
    if (fwrite(s_hdr, 1, WF_HDR_RESERVE, f) != (size_t)WF_HDR_RESERVE) return false;
    return true;
}

// #FW-14: функции патча шапки (seg_read_rows/seg_patch_rows/seg_patch_counts)
// удалены — saved_rows/saved_at всегда 0 («выводить из размера файла»), патч
// offset 14/34 был LittleFS COW-заморозкой всего хвоста файла (#FW-8).

// Закрыть текущий открытый сегмент. rows>0 → валидный .aswf, seg_count++.
// rows==0 → удалить пустой огрызок. (под s_fs_lock)
static void seg_finalize(void)
{
    if (!s_seg_fp) return;
    if (s_seg_rows > 0) {
        // #FW-8/#FW-14: шапку НЕ патчить НИКОГДА. LittleFS хранит файл
        // обратно-связанным CTZ-списком: запись в offset 14/34 = copy-on-write
        // всего хвоста (1 МБ ≈ 25-45 c заморозки flash-кэша, трасса
        // fw8_rollover_trace.py 2026-07-02; даже 10-строчный огрызок ≈ 4-7 c).
        // Конвенция: saved_rows=0 в шапке = «строк — из размера файла».
        // Все потребители и так выводят rows из payload/stride: вьюер
        // (waterfall_viewer.html:162), /api/waterfall/segments, boot-reconcile.
        // Финализация = только fsync+fclose — дёшево, возрастной ролловер
        // раз в 10 мин (#FW-14) не замораживает USB-приём.
        fflush(s_seg_fp);
        fsync(fileno(s_seg_fp));
        fclose(s_seg_fp);
        s_seg_fp = NULL;
        LOCK(); s_status.seg_count++; UNLOCK();
        ESP_LOGI(TAG, "seg_%05" PRIu32 ".aswf finalized (%" PRIu32 " rows)",
                 s_seg_cur, s_seg_rows);
    } else {
        fclose(s_seg_fp);
        s_seg_fp = NULL;
        char p[64]; seg_path(p, sizeof(p), s_seg_cur);
        unlink(p);
    }
    s_seg_cur  = 0xFFFFFFFFu;
    s_seg_rows = 0;
}

// Открыть новый сегмент (rows=0). mkdir + 1 повтор при отсутствии каталога.
// (под s_fs_lock)
static bool seg_open_new(void)
{
    char p[64];
    seg_path(p, sizeof(p), s_seg_next);
    // #FW-14: "wb" — шапка после открытия не читается и не патчится
    // (конвенция saved_rows=0, см. seg_finalize).
    FILE *f = fopen(p, "wb");
    if (!f) {
        mkdir(WF_SEG_DIR, 0777);
        f = fopen(p, "wb");
        if (!f) { ESP_LOGE(TAG, "cannot open %s", p); return false; }
    }
    long now = (long)time(NULL);
    // #DATA-1b/1c: снимок метаданных сегмента ДО сборки шапки (оба идут в JSON).
    // seg_seq — глоб. монотонный, переживает clear/ребут (NVS); total_at_open —
    // накопительный total прибора сейчас (reconciliation на PC). Персист seq в NVS
    // сразу: если ребут случится с открытым сегментом, следующий seq не повторится.
    s_seg_seq++;
    s_seg_total_at_open = spectrum_get_current()->total_counts;
    {
        nvs_handle_t nh;
        if (nvs_open(WF_SETTINGS_NS, NVS_READWRITE, &nh) == ESP_OK) {
            nvs_set_u32(nh, "seg_seq", s_seg_seq);
            nvs_commit(nh);
            nvs_close(nh);
        }
    }
    // #FW-14: saved_rows=0 / saved_at=0 = «выводить из размера файла» — шапка
    // никогда не патчится (патч offset 14/34 = LittleFS COW всего хвоста с
    // заморозкой flash-кэша, #FW-8). Время строк несут поля dur (v2) +
    // started_at; saved_at потребители шапки не используют.
    seg_header_build(0, 0, now);
    if (!seg_write_full_header(f)) {
        ESP_LOGE(TAG, "header write failed %s", p);
        fclose(f); unlink(p); return false;
    }
    // v3: baseline секция (WF_CHANNELS×uint32 LE) между header и payload
    if (s_baseline) {
        if (fwrite(s_baseline, 4, WF_CHANNELS, f) != (size_t)WF_CHANNELS) {
            ESP_LOGE(TAG, "baseline write failed %s", p);
            fclose(f); unlink(p); return false;
        }
    } else {
        // s_baseline не выделен (OOM) — пишем нули (32 КБ по 128 Б за раз)
        static const uint8_t zeroes[128] = {0};
        for (int c = 0; c < WF_BASELINE_BYTES / (int)sizeof(zeroes); c++) {
            if (fwrite(zeroes, 1, sizeof(zeroes), f) != sizeof(zeroes)) {
                ESP_LOGE(TAG, "baseline zeros write failed %s", p);
                fclose(f); unlink(p); return false;
            }
        }
    }
    fflush(f); fsync(fileno(f));
    s_seg_fp        = f;
    s_seg_cur       = s_seg_next;
    s_seg_rows      = 0;
    s_seg_opened_at = now;
    s_seg_next++;
    ESP_LOGI(TAG, "seg_%05" PRIu32 ".aswf opened", s_seg_cur);
    return true;
}

// Минимальный индекс среди ЗАВЕРШЁННЫХ сегментов (≠ текущего открытого).
// 0xFFFFFFFF — нечего удалять. (под s_fs_lock)
static uint32_t seg_oldest_completed(void)
{
    DIR *d = opendir(WF_SEG_DIR);
    if (!d) return 0xFFFFFFFFu;
    uint32_t best = 0xFFFFFFFFu;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        uint32_t idx;
        if (!seg_name_index(e->d_name, &idx)) continue;
        if (s_seg_fp && idx == s_seg_cur) continue;  // открытый — пропустить
        if (idx == s_seg_pinned) continue;           // #REC-11-A2: выгружается прямо сейчас — не удалять
        if (idx < best) best = idx;
    }
    closedir(d);
    return best;
}

// Кольцо keep-last: пока на Flash меньше need байт — удалять старейший
// ЗАВЕРШЁННЫЙ сегмент. Текущий открытый не трогаем. (под s_fs_lock)
static void make_room(uint32_t need)
{
    while (flash_free_bytes() < need) {
        uint32_t oldest = seg_oldest_completed();
        if (oldest == 0xFFFFFFFFu) break;        // только открытый/пусто — выйти
        char p[64];
        seg_path(p, sizeof(p), oldest);
        if (unlink(p) != 0) { ESP_LOGW(TAG, "ring: unlink %s failed", p); break; }
        LOCK();
        s_status.seg_dropped++;
        if (s_status.seg_count) s_status.seg_count--;
        s_status.flash_full = true;              // индикатор: кольцо хоть раз сработало
        UNLOCK();
        ESP_LOGW(TAG, "ring: dropped seg_%05" PRIu32 ".aswf", oldest);
    }
}

// Удалить все сегменты каталога. (под s_fs_lock)
static void seg_delete_all(void)
{
    DIR *d = opendir(WF_SEG_DIR);
    if (!d) return;
    struct dirent *e;
    char p[80];
    while ((e = readdir(d)) != NULL) {
        uint32_t idx;
        if (!seg_name_index(e->d_name, &idx)) continue;
        snprintf(p, sizeof(p), WF_SEG_DIR "/%.32s", e->d_name);  // имя ≤24 (seg_name_index), %.32s = доказуемая граница для -Wformat-truncation
        unlink(p);
    }
    closedir(d);
}

// Определить stride и payload-offset существующего сегмента по JSON-шапке.
// v3: stride=16402, payload_offset=4104+32768; v2: stride=16386, offset=4104;
// v1 (нет row_stride): stride=16384, offset=4104. Позиция файла → 0 после вызова.
static uint32_t seg_detect_stride(FILE *f)
{
    char hdr[320];
    if (fseek(f, 8, SEEK_SET) != 0) { fseek(f, 0, SEEK_SET); return WF_ROW_BYTES; }
    size_t rd = fread(hdr, 1, sizeof(hdr) - 1, f);
    fseek(f, 0, SEEK_SET);
    hdr[rd] = '\0';
    const char *p = strstr(hdr, "\"row_stride\":");
    if (p) {
        unsigned long v = strtoul(p + 13, NULL, 10);   // 13 = strlen("\"row_stride\":")
        if (v >= WF_ROW_BYTES && v <= (unsigned long)WF_ROW_BYTES + 128) return (uint32_t)v;
    }
    return WF_ROW_BYTES;   // legacy v1 — без длительностей
}

static int seg_detect_version(FILE *f)
{
    char hdr[320];
    if (fseek(f, 8, SEEK_SET) != 0) { fseek(f, 0, SEEK_SET); return 2; }
    size_t rd = fread(hdr, 1, sizeof(hdr) - 1, f);
    fseek(f, 0, SEEK_SET);
    hdr[rd] = '\0';
    const char *p = strstr(hdr, "\"version\":");
    if (p) {
        int v = (int)strtol(p + 10, NULL, 10);   // 10 = strlen("\"version\":")
        if (v >= 1 && v <= 9) return v;
    }
    return 2;   // default
}

static long seg_payload_offset(FILE *f)
{
    return seg_detect_version(f) >= 3
        ? (long)(8 + WF_HDR_RESERVE + WF_BASELINE_BYTES)
        : (long)(8 + WF_HDR_RESERVE);
}

// boot-реконсиляция: пройти каталог, для каждого сегмента rows = из размера файла.
// rows==0 → удалить огрызок. Шапки НЕ патчатся (#FW-14: saved_rows=0 = derive-from-
// size, патч = COW-заморозка #FW-8). Восстановить s_seg_next/seg_count. (под s_fs_lock)
static void seg_reconcile(void)
{
    if (!sd_card_present()) return;               // #SD-5: сегменты только на карте
    mkdir(WF_SEG_DIR, 0777);                      // гарантировать каталог
    DIR *d = opendir(WF_SEG_DIR);
    uint32_t maxidx = 0, completed = 0;
    bool any = false;
    if (d) {
        struct dirent *e;
        char p[80];
        while ((e = readdir(d)) != NULL) {
            uint32_t idx;
            if (!seg_name_index(e->d_name, &idx)) continue;
            snprintf(p, sizeof(p), WF_SEG_DIR "/%.32s", e->d_name);  // имя ≤24 (seg_name_index), %.32s = доказуемая граница для -Wformat-truncation
            struct stat sb;
            if (stat(p, &sb) != 0) continue;
            FILE *f = fopen(p, "rb");
            if (!f) continue;
            uint32_t stride = seg_detect_stride(f);   // v3=16402, v2=16386, v1=16384
            long poff = seg_payload_offset(f);         // v3=36872, v1/v2=4104
            long payload = (long)sb.st_size - poff;
            uint32_t rows = payload > 0 ? (uint32_t)(payload / stride) : 0;
            if (rows == 0) { fclose(f); unlink(p); continue; }
            fclose(f);
            completed++;
            if (!any || idx > maxidx) { maxidx = idx; any = true; }
        }
        closedir(d);
    }
    s_seg_next = any ? maxidx + 1 : 0;
    s_seg_cur  = 0xFFFFFFFFu;
    s_seg_fp   = NULL;
    s_seg_rows = 0;
    LOCK(); s_status.seg_count = completed; s_status.seg_dropped = 0; UNLOCK();
    ESP_LOGI(TAG, "seg reconcile: %" PRIu32 " segments, next=%" PRIu32, completed, s_seg_next);
}

// ----------------------------------------------------------------------------

static void ring_push(const uint16_t *row, uint16_t dur, float temp)
{
    memcpy(s_ring + (size_t)s_head * WF_CHANNELS, row, WF_ROW_BYTES);
    s_dur[s_head]  = dur;                // #FW-5: реальная длительность среза (сек)
    s_temp[s_head] = temp;               // #FW-41: t1 детектора на момент закрытия строки (°C, NaN если нет -inf)
    s_head = (s_head + 1) % s_capacity;
    if (s_count < s_capacity) s_count++;
}

void spectrogram_init(void)
{
    s_lock    = xSemaphoreCreateMutex();
    s_fs_lock = xSemaphoreCreateMutex();
    memset(&s_status, 0, sizeof(s_status));
    s_status.interval_sec = WF_INTERVAL_DEFAULT;
    s_status.persist      = true;
    settings_load();   // #FW-15: поверх дефолтов — сохранённые настройки из NVS
    s_seg_cur             = 0xFFFFFFFFu;

    s_capacity = WF_RING_ROWS_DEFAULT;
    size_t ring_bytes = (size_t)s_capacity * WF_ROW_BYTES;
    s_ring = heap_caps_malloc(ring_bytes, MALLOC_CAP_SPIRAM);
    while (!s_ring && s_capacity > 16) {
        s_capacity /= 2;
        ring_bytes = (size_t)s_capacity * WF_ROW_BYTES;
        s_ring = heap_caps_malloc(ring_bytes, MALLOC_CAP_SPIRAM);
    }
    // #FW-5: параллельное кольцо длительностей (s_capacity уже финализирована выше).
    s_dur  = heap_caps_malloc((size_t)s_capacity * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    // #FW-41: параллельное кольцо температур t1 (float, °C). ~1 КБ на 256 строк.
    s_temp = heap_caps_malloc((size_t)s_capacity * sizeof(float), MALLOC_CAP_SPIRAM);
    s_prev = heap_caps_malloc(WF_CHANNELS * sizeof(uint32_t), MALLOC_CAP_SPIRAM);
    s_row  = heap_caps_malloc(WF_ROW_BYTES, MALLOC_CAP_SPIRAM);
    s_snap     = heap_caps_malloc(sizeof(spectrum_data_t), MALLOC_CAP_SPIRAM);
    s_wf_snap  = heap_caps_malloc(sizeof(spectrum_data_t), MALLOC_CAP_SPIRAM);
    s_fs_buf   = heap_caps_malloc(WF_ROW_BYTES, MALLOC_CAP_SPIRAM);   // #FW-6 consumer
    // v3: baseline (32 КБ) — не критично, пишем нули если нет PSRAM
    s_baseline = heap_caps_calloc(WF_CHANNELS, sizeof(uint32_t), MALLOC_CAP_SPIRAM);
    if (!s_baseline) ESP_LOGW(TAG, "baseline alloc failed — zeros on start");
    // v3: LUT кривой мощности дозы (32 КБ PSRAM, nullable)
    s_dose_lut = heap_caps_malloc(WF_CHANNELS * sizeof(float), MALLOC_CAP_SPIRAM);
    if (!s_dose_lut) ESP_LOGW(TAG, "dose_lut alloc failed — curve disabled");

    if (!s_ring || !s_dur || !s_temp || !s_prev || !s_row || !s_snap || !s_wf_snap || !s_fs_buf) {
        ESP_LOGE(TAG, "PSRAM alloc failed");
        s_status.ready = false;
        return;
    }
    s_status.ready         = true;
    s_status.ring_capacity = s_capacity;
    ESP_LOGI(TAG, "waterfall ready: ring=%" PRIu32 " rows (%u KB PSRAM)",
             s_capacity, (unsigned)(ring_bytes / 1024));

    // #FW-6: семафор-будильник consumer'а создаём ДО запуска producer'а.
    s_fs_sig = xSemaphoreCreateBinary();
    if (!s_fs_sig) { ESP_LOGE(TAG, "fs sig create failed"); s_status.ready = false; return; }
    // #FW-13 фикс №2: подписка producer'а на коммиты свипов (NULL — останется 1-с тик).
    s_commit_sig = xSemaphoreCreateBinary();
    if (s_commit_sig) spectrum_add_commit_listener(s_commit_sig);
    // wf_fs_task (consumer, prio 2 < producer): весь flash-I/O (opendir/readdir +
    // snprintf шапки на стеке → 8192). Producer prio 3 всегда вытесняет — такт точнее.
    xTaskCreatePinnedToCore(wf_fs_task, "wf_fs", 8192, NULL, 2, NULL, 1);
    // stack 8192: producer теперь без FS, но оставляем запас (snapshot + row_cb).
    xTaskCreatePinnedToCore(wf_task, "wf", 8192, NULL, 3, NULL, 1);
    spectrogram_load_dose_curve(); // попытка; FS смонтирована в main до вызова init()
}

// #FW-6: запись одной строки в сегмент. Вызывается ТОЛЬКО из wf_fs_task без
// удержанных локов — берёт s_fs_lock сам. Логика идентична прежнему inline-блоку
// wf_task: финализация по лимиту строк/возраста, кольцо keep-last, ленивое
// открытие сегмента, запись 16384 Б спектра + 2 Б длительности (uint16 LE) + fsync.
static void seg_write_row(const uint8_t *row, uint16_t dur, float temp)
{
    if (!sd_card_present()) {   // #SD-5: без карты — никуда не пишем, только флаг ошибки
        LOCK();
        if (!s_status.storage_err) ESP_LOGE(TAG, "no SD card: waterfall rows are NOT saved (RAM ring only)");
        s_status.storage_err = true;
        UNLOCK();
        return;
    }
    FSLOCK();
    // #FW-14: здесь только ролловер по строкам; возрастной (10 мин,
    // WF_SEG_MAX_AGE_SEC) живёт в цикле wf_fs_task — при interval > возраста
    // проверка в write-пути ждала бы следующей строки часами, файл висел открытым.
    // Огрызок теперь дёшев (без патча шапки, saved_rows=0 = derive-from-size),
    // растягивать возраст до 64*interval (#FW-8) больше незачем.
    if (s_seg_fp && s_seg_rows >= WF_SEG_MAX_ROWS) seg_finalize();
    make_room((uint32_t)WF_ROW_STRIDE + WF_FLASH_RESERVE);   // страховка (обычно место уже есть — #FW-8)
    if (!s_seg_fp) seg_open_new();
    if (s_seg_fp) {
        uint8_t durle[WF_DUR_BYTES] = { (uint8_t)(dur & 0xFF), (uint8_t)(dur >> 8) };
        // #FW-41 v5 суффикс: timestamp + lat(NaN) + lon(NaN) + dose_rate + temperature.
        uint8_t v3tail[WF_TS_BYTES + WF_GPS_BYTES + WF_DOSE_BYTES + WF_TEMP_BYTES];
        {
            uint32_t ts = (uint32_t)time(NULL);
            uint32_t nan_bits = 0x7FC00000u;
            float lat_v, lon_v, dose_v;
            memcpy(&lat_v,  &nan_bits, 4);
            memcpy(&lon_v,  &nan_bits, 4);
            memcpy(&dose_v, &nan_bits, 4);
            if (s_dose_lut && s_dose_curve_n > 0 && dur > 0) {
                // кривая: взвешенная сумма bins[i]*lut[i]
                double wsum = 0.0;
                const uint16_t *sp = (const uint16_t *)row;
                for (int ci = 0; ci < WF_CHANNELS; ci++) wsum += (double)sp[ci] * s_dose_lut[ci];
                dose_v = (float)(wsum / (double)dur);
            } else if (s_dose_k > 0.0f && dur > 0) {
                uint64_t sum = 0;
                const uint16_t *sp = (const uint16_t *)row;
                for (int ci = 0; ci < WF_CHANNELS; ci++) sum += sp[ci];
                dose_v = ((float)sum / (float)dur) * s_dose_k;
            }
            memcpy(v3tail + 0,  &ts,    4);
            memcpy(v3tail + 4,  &lat_v, 4);
            memcpy(v3tail + 8,  &lon_v, 4);
            memcpy(v3tail + 12, &dose_v, 4);
            memcpy(v3tail + 16, &temp,  4);   // #FW-41: t1 детектора (NaN если нет -inf)
        }
        // #DATA-1a: CRC32 по всем 16406 предшествующим байтам строки (spectrum+dur+v3tail),
        // аккумулятивно, без доп. буфера. LE-запись 4 байтами следом. PC при pull сверяет.
        uint32_t crc = 0xFFFFFFFFu;
        crc = crc32_upd(crc, row,    WF_ROW_BYTES);
        crc = crc32_upd(crc, durle,  WF_DUR_BYTES);
        crc = crc32_upd(crc, v3tail, sizeof(v3tail));
        crc ^= 0xFFFFFFFFu;
        uint8_t crcle[WF_CRC_BYTES] = {
            (uint8_t)(crc), (uint8_t)(crc >> 8), (uint8_t)(crc >> 16), (uint8_t)(crc >> 24)
        };
        size_t wr  = fwrite(row,    1, WF_ROW_BYTES,    s_seg_fp);
        size_t wrd = (wr  == WF_ROW_BYTES)  ? fwrite(durle,  1, WF_DUR_BYTES,       s_seg_fp) : 0;
        size_t wrv = (wrd == WF_DUR_BYTES)  ? fwrite(v3tail, 1, sizeof(v3tail),     s_seg_fp) : 0;
        size_t wrc = (wrv == sizeof(v3tail))? fwrite(crcle,  1, WF_CRC_BYTES,       s_seg_fp) : 0;
        if (wr != WF_ROW_BYTES || wrd != WF_DUR_BYTES || wrv != sizeof(v3tail) ||
            wrc != WF_CRC_BYTES || fflush(s_seg_fp) != 0) {
            ESP_LOGE(TAG, "seg row write failed (wr=%zu) — drop segment", wr);
            fclose(s_seg_fp); s_seg_fp = NULL; s_seg_cur = 0xFFFFFFFFu; s_seg_rows = 0;
        } else {
            s_seg_rows++;
            LOCK(); s_status.flash_rows++; UNLOCK();
            if (s_seg_rows % WF_FSYNC_BATCH == 0)
                fsync(fileno(s_seg_fp));
            // #FW-8: место под хвост текущего сегмента + весь следующий освобождаем
            // ЗАРАНЕЕ, в середине сегмента. Иначе unlink 1МБ кольцом (десятки секунд
            // стираний Flash с заморозкой кэша) фазово совпадал с ролловером
            // finalize+create → USB-приём терял секундный пакет прибора → строки
            // границы сегмента получали dur 4/6 вместо 5 (тёмные полосы каждые
            // 64 строки, #VIEW-9). Разнос по фазе дробит burst; недобор среза в
            // середине, если и случится, честно ложится в поле dur (v2).
            if (s_seg_rows == WF_SEG_PREP_ROW) {
                uint32_t rows_ahead = (WF_SEG_MAX_ROWS - s_seg_rows) + WF_SEG_MAX_ROWS;
                make_room(rows_ahead * (uint32_t)WF_ROW_STRIDE + WF_FLASH_RESERVE);
            }
        }
    }
    FSUNLOCK();
}

static void wf_task(void *arg)
{
    for (;;) {
        // #FW-10: строка закрывается по живому времени ПРИБОРА (total_time_sec),
        // а не по локальному тику. vTaskDelay(iv) дрейфовал против секундной
        // сетки прибора: задержка коммита свипа (flash-запись/чтение) давала
        // пары dur 3+7 / 4+6 → мелкая полосатость яркости (счёты ∝ dur) и
        // «склейка» на границах сегментов. Опрос раз в 1 c атомарного скаляра
        // без лока; полный снимок — только при фактическом закрытии строки.
        // Прибор молчит (время не идёт) → строк нет, водопад честно стоит.
        // #FW-13 фикс №2: будимся коммитом свипа (= конец USB-burst), чтобы снапшот
        // и запись строки легли в тихое окно. Таймаут 1500 мс — fallback-тик при
        // молчащем/отключённом приборе (прежнее поведение).
        if (s_commit_sig) xSemaphoreTake(s_commit_sig, pdMS_TO_TICKS(1500));
        else vTaskDelay(pdMS_TO_TICKS(1000));
        hang_watch_beat(HB_WF);   // #HANG-1

        if (!s_status.recording) continue;
        if (s_rebase) {   // #WF-4
            uint32_t commits = 0;
            spectrum_get_hist_stats(&commits, NULL);
            if (commits == 0) continue;   // свежих свипов в эту загрузку ещё не было
            spectrum_get_snapshot(s_wf_snap);
            LOCK();
            memcpy(s_prev, s_wf_snap->bins, WF_CHANNELS * sizeof(uint32_t));
            s_prev_total = s_wf_snap->total_counts;
            s_prev_time  = s_wf_snap->total_time_sec;
            UNLOCK();
            if (s_baseline)
                for (int i = 0; i < WF_CHANNELS; i++) s_baseline[i] = s_wf_snap->bins[i];
            s_rebase = false;
            continue;
        }
        uint32_t iv = s_status.interval_sec;
        if (iv < WF_INTERVAL_MIN) iv = WF_INTERVAL_MIN;
        uint32_t now_time = spectrum_get_current()->total_time_sec;
        if (now_time >= s_prev_time && now_time - s_prev_time < iv) continue;

        spectrum_get_snapshot(s_wf_snap);

        bool reset = (s_wf_snap->total_counts < s_prev_total);
        for (int i = 0; i < WF_CHANNELS; i++) {
            int64_t d = (int64_t)s_wf_snap->bins[i] - (reset ? 0 : (int64_t)s_prev[i]);
            if (d < 0) d = 0; else if (d > 65535) d = 65535;
            s_row[i]  = (uint16_t)d;
            s_prev[i] = s_wf_snap->bins[i];
        }
        s_prev_total = s_wf_snap->total_counts;

        // #FW-5: реальная длительность среза = дельта живого времени прибора
        // (total_time_sec, целые секунды). reset (счётчики прибора обнулились) →
        // считать от 0. dur==0 (прибор не продвинул живое время за тик) хранится
        // как есть; потребитель (N42-экспорт) подставит номинал при делении.
        uint32_t cur_time = s_wf_snap->total_time_sec;
        uint32_t dt = reset ? cur_time
                            : (cur_time >= s_prev_time ? cur_time - s_prev_time : 0);
        s_prev_time = cur_time;
        uint16_t dur = dt > 65535 ? 65535 : (uint16_t)dt;

        // #FW-41: t1 детектора на момент закрытия строки. Источник — -inf (каждые
        // 30 мин, #FW-13), поле device_info_t.t1. Пока -inf не прочитан (valid=false)
        // пишем NaN, а не struct-нулевой 0.0 — иначе ложное «0 °C».
        float temp_v;
        {
            const device_info_t *di = spectrum_get_device_info();
            if (di && di->valid) {
                temp_v = di->t1;
            } else {
                uint32_t nan_bits = 0x7FC00000u;
                memcpy(&temp_v, &nan_bits, 4);
            }
        }

        LOCK();
        ring_push(s_row, dur, temp_v);
        s_status.total_rows++;
        s_status.ring_count = s_count;
        uint32_t idx = s_status.total_rows - 1;
        UNLOCK();

        // #FW-6: вся flash-запись вынесена в wf_fs_task. Producer лишь будит
        // consumer; решение persist и запись строки из кольца (по s_fs_flushed)
        // делает consumer. Латентность флеша больше НЕ влияет на такт набора.
        if (s_fs_sig) xSemaphoreGive(s_fs_sig);
        if (s_row_cb) s_row_cb(s_row, WF_ROW_BYTES, idx);
    }
}

// #FW-6 consumer: единственный писатель сегментов. Сливает все накопленные строки
// кольца по глоб. индексу; отставание при всплеске стирания (≈7 строк) << ёмкости.
static void wf_fs_task(void *arg)
{
    for (;;) {
        // #FW-14: таймаут вместо portMAX_DELAY — периодическая проверка возраста
        // открытого сегмента даже без новых строк (при interval > MAX_AGE строк
        // между ролловерами нет вовсе).
        xSemaphoreTake(s_fs_sig, pdMS_TO_TICKS(5000));
        hang_watch_beat(HB_WF_FS);   // #HANG-1
        for (;;) {
            LOCK();
            uint32_t g = s_fs_flushed, total = s_status.total_rows;
            bool have = (g < total), pst = s_status.persist;
            bool lost = have && (total - g > s_capacity);
            if (have && !lost) {
                uint32_t slot = g % s_capacity;
                memcpy(s_fs_buf, s_ring + (size_t)slot * WF_CHANNELS, WF_ROW_BYTES);
                s_fs_dur  = s_dur[slot];
                s_fs_temp = s_temp[slot];   // #FW-41
            }
            UNLOCK();
            if (!have) break;
            if (lost) { LOCK(); s_fs_flushed = total - s_capacity; UNLOCK(); continue; }
            if (pst) seg_write_row(s_fs_buf, s_fs_dur, s_fs_temp);
            LOCK(); s_fs_flushed++; UNLOCK();
        }
        // #FW-14: финализация по возрасту — 64 строки ИЛИ 10 мин (WATERFALL.md),
        // чтобы при больших интервалах файл не висел открытым часами и приёмник
        // мог его забрать. Без патча шапки это только fsync+fclose (дёшево).
        FSLOCK();
        if (s_seg_fp && (long)(time(NULL) - s_seg_opened_at) >= WF_SEG_MAX_AGE_SEC)
            seg_finalize();
        FSUNLOCK();
    }
}

void spectrogram_get_status(wf_status_t *out)
{
    if (!out) return;
    LOCK();
    *out = s_status;
    out->sd = sd_card_present();   // #SD-5
    // #FW-21: elapsed_sec из монотонных часов — честная длительность сессии,
    // не зависит от NTP/started_at. Клиент показывает это число напрямую (F5
    // не сбрасывает: значение живёт на плате, не в памяти вкладки).
    if (s_status.recording && s_started_uptime_us > 0) {
        int64_t d = (esp_timer_get_time() - s_started_uptime_us) / 1000000;
        out->elapsed_sec = (d > 0) ? (uint32_t)d : 0;
    } else {
        out->elapsed_sec = 0;
    }
    UNLOCK();
}

bool spectrogram_is_recording(void)
{
    return s_status.recording;
}

void spectrogram_restore(void)
{
    if (!s_status.ready) return;

    // Всегда сверить каталог сегментов (восстановить s_seg_next/seg_count,
    // пропатчить недописанные, удалить пустые).
    FSLOCK();
    seg_reconcile();
    FSUNLOCK();

    FILE *f = fopen(WF_STATE, "rb");
    if (!f) return;                          // нет persist-состояния — чистый старт
    wf_state_t st;
    size_t rd = fread(&st, 1, sizeof(st), f);
    fclose(f);
    if (rd != sizeof(st) || st.magic != WF_STATE_MAGIC) return;
    if (!st.active || !st.persist) return;   // запись была остановлена — не возобновляем

    // Возобновляем запись в НОВЫЙ сегмент (wf_task откроет лениво на первом тике).
    spectrum_get_snapshot(s_snap);
    // v3: baseline при resume — накопительный спектр на момент восстановления
    if (s_baseline) {
        for (int i = 0; i < WF_CHANNELS; i++) s_baseline[i] = s_snap->bins[i];
    }
    LOCK();
    memcpy(s_prev, s_snap->bins, WF_CHANNELS * sizeof(uint32_t));
    s_prev_total          = s_snap->total_counts;
    s_prev_time           = s_snap->total_time_sec;   // #FW-5: база для дельты длительности
    s_head = 0; s_count = 0;
    s_status.ring_count   = 0;
    s_status.total_rows   = 0;     // счётчик ТЕКУЩЕЙ сессии записи (с момента возобновления)
    s_fs_flushed          = 0;     // #FW-6: consumer стартует с нуля
    s_status.flash_rows   = 0;
    s_status.flash_full   = false;
    s_status.persist      = (st.persist != 0);
    s_status.interval_sec = st.interval_sec;
    s_status.started_at   = (time_t)st.started_at;
    s_started_uptime_us   = esp_timer_get_time();  // #FW-21: новая сессия по счёту строк
    s_status.recording    = true;
    s_rebase              = true;   // #WF-4: опора — первый свежий свип
    UNLOCK();
    ESP_LOGW(TAG, "restore: resumed recording in new segment, interval=%" PRIu32 "s",
             (uint32_t)st.interval_sec);
}

// #OTA-3: перед штатной перезагрузкой (команда, прошивка по WiFi) дописать строки
// кольца на карту и закрыть сегмент. Состояние «запись идёт» (wf_state.bin) не
// трогаем — после перезагрузки запись продолжится (spectrogram_restore).
void spectrogram_prepare_reboot(void)
{
    if (!s_status.ready) return;
    LOCK();
    bool was = s_status.recording;
    s_status.recording = false;
    UNLOCK();
    if (was) {
        vTaskDelay(pdMS_TO_TICKS(100));   // in-flight итерация wf_task
        if (s_fs_sig) xSemaphoreGive(s_fs_sig);
        for (int i = 0; i < 30; i++) {
            LOCK(); bool drained = (s_fs_flushed >= s_status.total_rows); UNLOCK();
            if (drained) break;
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    FSLOCK();
    if (s_seg_fp) seg_finalize();
    FSUNLOCK();
}

void spectrogram_time_synced(void)
{
    bool corrected = false;
    time_t new_started_at = 0;
    LOCK();
    if (s_status.recording && s_status.started_at < (time_t)WF_SANE_EPOCH) {
        int64_t elapsed_us = esp_timer_get_time() - s_started_uptime_us;
        new_started_at = time(NULL) - (time_t)(elapsed_us / 1000000);
        s_status.started_at = new_started_at;
        corrected = true;
    }
    UNLOCK();
    if (corrected) {
        ESP_LOGW(TAG, "FW-23: started_at corrected after SNTP sync -> %ld", (long)new_started_at);
        write_state(true);   // #REC-6: сохранить исправленный якорь на случай ребута
    }
}

int spectrogram_start(void)
{
    if (!s_status.ready) return -1;

    // #WF-1: остановить producer и дождаться дренажа consumer ДО сброса
    // счётчиков. Иначе consumer в окне между UNLOCK и FSLOCK внутри
    // seg_write_row допишет строку СТАРОЙ сессии в НОВЫЙ сегмент, а его
    // s_fs_flushed++ «съест» строку 0 новой. Ждать надо ДО FSLOCK: consumer
    // сам берёт FSLOCK в seg_write_row, ожидание под FSLOCK не сойдётся.
    // 100 мс — на завершение in-flight итерации producer (recording
    // проверяется без лока в начале итерации). Bounded, образец — stop().
    LOCK();
    s_status.recording = false;
    UNLOCK();
    vTaskDelay(pdMS_TO_TICKS(100));
    if (s_fs_sig) xSemaphoreGive(s_fs_sig);
    for (int i = 0; i < 60; i++) {
        LOCK(); bool drained = (s_fs_flushed >= s_status.total_rows); UNLOCK();
        if (drained) break;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    spectrum_get_snapshot(s_snap);
    // v3: снимок накопительного спектра → baseline секция каждого сегмента
    if (s_baseline) {
        for (int i = 0; i < WF_CHANNELS; i++) s_baseline[i] = s_snap->bins[i];
    }

    FSLOCK();
    if (s_seg_fp) seg_finalize();          // закрыть огрызок от прошлой записи
    LOCK();
    memcpy(s_prev, s_snap->bins, WF_CHANNELS * sizeof(uint32_t));
    s_prev_total        = s_snap->total_counts;
    s_prev_time         = s_snap->total_time_sec;   // #FW-5: база для дельты длительности
    s_head              = 0;
    s_count             = 0;
    s_status.ring_count = 0;
    s_status.total_rows = 0;
    s_fs_flushed        = 0;     // #FW-6: consumer стартует с нуля
    s_status.flash_full = false;
    s_status.flash_rows = 0;
    s_status.started_at = time(NULL);
    s_started_uptime_us = esp_timer_get_time();  // #FW-21: монотонная база elapsed_sec
    s_status.recording  = true;            // старые сегменты НЕ трогаем (монотонный индекс)
    UNLOCK();
    FSUNLOCK();

    write_state(true);   // #REC-6: пометить «запись активна» для возобновления после ребута
    ESP_LOGI(TAG, "recording started, interval=%" PRIu32 "s persist=%d",
             s_status.interval_sec, s_status.persist);
    return 0;
}

int spectrogram_stop(void)
{
    LOCK();
    s_status.recording = false;
    s_started_uptime_us = 0;   // #FW-21: сессия окончена — elapsed_sec → 0
    UNLOCK();

    // #FW-6: дождаться, пока consumer допишет хвост строк из кольца (он —
    // единственный писатель), затем финализировать. Bounded (≤60с): при
    // зависшем FS не блокируемся навсегда.
    if (s_fs_sig) xSemaphoreGive(s_fs_sig);
    for (int i = 0; i < 60; i++) {
        LOCK(); bool drained = (s_fs_flushed >= s_status.total_rows); UNLOCK();
        if (drained) break;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    FSLOCK();
    seg_finalize();      // закрыть текущий сегмент как валидный .aswf
    FSUNLOCK();

    write_state(false);  // #REC-6: запись остановлена — ребут НЕ должен возобновлять
    ESP_LOGI(TAG, "recording stopped, flash_rows=%" PRIu32, s_status.flash_rows);
    return 0;
}

int spectrogram_clear(void)
{
    FSLOCK();
    LOCK();
    if (s_status.recording) { UNLOCK(); FSUNLOCK(); return -1; }
    s_head              = 0;
    s_count             = 0;
    s_status.ring_count = 0;
    s_status.total_rows = 0;
    s_fs_flushed        = 0;     // #FW-6: consumer стартует с нуля
    s_status.flash_rows = 0;
    s_status.flash_full = false;
    s_status.seg_count  = 0;
    s_status.seg_dropped = 0;
    s_started_uptime_us = 0;     // #FW-21: очистка — сессии нет
    UNLOCK();

    if (s_seg_fp) { fclose(s_seg_fp); s_seg_fp = NULL; }
    s_seg_cur    = 0xFFFFFFFFu;
    s_seg_pinned = 0xFFFFFFFFu;   // #REC-11-A2: индексы сбрасываются (next=0) — снять устаревший пин
    s_seg_rows = 0;
    seg_delete_all();
    s_seg_next = 0;
    unlink(WF_DATA);     // legacy (#REC-6)
    unlink(WF_META);     // legacy (#REC-6)
    unlink(WF_STATE);    // #REC-6: сбросить persist-состояние записи
    FSUNLOCK();

    ESP_LOGI(TAG, "waterfall cleared");
    return 0;
}

void spectrogram_set_interval(uint32_t sec)
{
    if (sec < WF_INTERVAL_MIN) sec = WF_INTERVAL_MIN;
    if (sec > WF_INTERVAL_MAX) sec = WF_INTERVAL_MAX;
    LOCK(); s_status.interval_sec = sec; bool rec = s_status.recording; bool ps = s_status.persist; UNLOCK();
    settings_save(sec, ps);   // #FW-15: настройка переживает ребут и clr_wf (NVS)
    // #FW-15: смена интервала посреди записи обязана попасть и в WF_STATE —
    // иначе restore после ребута поднимет интервал, записанный на старте записи.
    if (rec) write_state(true);
}

void spectrogram_set_persist(bool on)
{
    LOCK(); s_status.persist = on; bool rec = s_status.recording; uint32_t iv = s_status.interval_sec; UNLOCK();
    settings_save(iv, on);        // #FW-15: настройка переживает ребут и clr_wf (NVS)
    if (rec) write_state(true);   // #FW-15: как и интервал — в persist-состояние
}

void spectrogram_set_row_cb(wf_row_cb_t cb)
{
    s_row_cb = cb;
}

size_t spectrogram_copy_window(uint16_t *dst, size_t max_rows, uint32_t *first_total_index)
{
    LOCK();
    size_t n = s_count;
    if (n > max_rows) n = max_rows;
    size_t start = ((size_t)s_head + s_capacity - n) % s_capacity;
    for (size_t i = 0; i < n; i++) {
        size_t ri = (start + i) % s_capacity;
        memcpy(dst + i * WF_CHANNELS, s_ring + ri * WF_CHANNELS, WF_ROW_BYTES);
    }
    if (first_total_index) *first_total_index = s_status.total_rows - (uint32_t)n;
    UNLOCK();
    return n;
}

// #FW-5: реальные длительности (сек) до max_rows новейших строк кольца,
// выровненные с spectrogram_copy_window/stream_window (старейшая из окна первой).
// Возвращает число записанных элементов. 0 в элементе = device-время не
// продвинулось (потребитель подставит номинал).
size_t spectrogram_copy_window_durations(uint16_t *dst, size_t max_rows)
{
    LOCK();
    size_t n = s_count;
    if (n > max_rows) n = max_rows;
    size_t start = ((size_t)s_head + s_capacity - n) % s_capacity;
    for (size_t i = 0; i < n; i++) {
        size_t ri = (start + i) % s_capacity;
        dst[i] = s_dur[ri];
    }
    UNLOCK();
    return n;
}

// #FW-41: температуры детектора (t1, °C) до max_rows новейших строк кольца,
// выровненные с spectrogram_copy_window/stream_window (старейшая из окна первой).
// NaN в элементе = на момент строки прибор ещё не прислал -inf (T неизвестна).
size_t spectrogram_copy_window_temps(float *dst, size_t max_rows)
{
    LOCK();
    size_t n = s_count;
    if (n > max_rows) n = max_rows;
    size_t start = ((size_t)s_head + s_capacity - n) % s_capacity;
    for (size_t i = 0; i < n; i++) {
        size_t ri = (start + i) % s_capacity;
        dst[i] = s_temp[ri];
    }
    UNLOCK();
    return n;
}

size_t spectrogram_stream_window(uint16_t *bounce, size_t max_rows,
                                 uint32_t *first_total_index,
                                 wf_emit_cb_t emit, void *ctx)
{
    // Снимок параметров окна под локом.
    LOCK();
    size_t n = s_count;
    if (max_rows && n > max_rows) n = max_rows;
    size_t start = ((size_t)s_head + s_capacity - n) % s_capacity;
    if (first_total_index) *first_total_index = s_status.total_rows - (uint32_t)n;
    UNLOCK();

    // Построчно: копия под локом в bounce, emit() — вне лока.
    // P3-5: окно — снимок best-effort. Если во время длинной выгрузки идёт
    // запись (wf_task пушит новые строки), кольцо может прокрутиться, и строки
    // у хвоста окна будут перезаписаны более свежими до того, как мы их скопируем.
    // Для мониторингового экспорта это приемлемо (а не строгий консистентный срез).
    for (size_t i = 0; i < n; i++) {
        LOCK();
        size_t ri = (start + i) % s_capacity;
        memcpy(bounce, s_ring + ri * WF_CHANNELS, WF_ROW_BYTES);
        UNLOCK();
        if (emit && !emit(ctx, bounce, WF_ROW_BYTES)) return i;
    }
    return n;
}

void spectrogram_lock_holders(void **ring, void **files)   // #HANG-1
{
    *ring  = s_lock    ? (void *)xSemaphoreGetMutexHolder(s_lock)    : NULL;
    *files = s_fs_lock ? (void *)xSemaphoreGetMutexHolder(s_fs_lock) : NULL;
}

const char *spectrogram_seg_dir(void)
{
    return WF_SEG_DIR;
}

uint32_t spectrogram_seg_open_index(void)
{
    uint32_t v;
    FSLOCK();
    v = s_seg_fp ? s_seg_cur : 0xFFFFFFFFu;
    FSUNLOCK();
    return v;
}

// ----------------------------------------------------------------------------
//  #REC-11-A2: координация выгрузки с кольцом keep-last (под s_fs_lock).
//  claim → pin старейшего завершённого сегмента (кольцо его не удалит);
//  done  → удалить с Flash после подтверждённого 2xx; release → снять пин (retry).
// ----------------------------------------------------------------------------

bool spectrogram_offload_claim(uint32_t *idx_out, char *name_out, size_t name_cap,
                               char *path_out, size_t path_cap, long *size_out)
{
    bool got = false;
    FSLOCK();
    if (s_seg_pinned == 0xFFFFFFFFu) {                 // не заняты другой выгрузкой
        uint32_t oldest = seg_oldest_completed();
        if (oldest != 0xFFFFFFFFu) {
            char p[80];
            seg_path(p, sizeof(p), oldest);
            struct stat sb;
            if (stat(p, &sb) == 0 && (long)sb.st_size > WF_SEG_HEADER) {
                s_seg_pinned = oldest;
                if (idx_out)  *idx_out  = oldest;
                if (name_out) snprintf(name_out, name_cap, "seg_%05" PRIu32 ".aswf", oldest);
                if (path_out) snprintf(path_out, path_cap, "%s", p);
                if (size_out) *size_out = (long)sb.st_size;
                got = true;
            }
        }
    }
    FSUNLOCK();
    return got;
}

void spectrogram_offload_done(uint32_t idx)
{
    FSLOCK();
    if (idx == s_seg_pinned) {
        char p[80];
        seg_path(p, sizeof(p), idx);
        if (unlink(p) != 0) ESP_LOGW(TAG, "offload_done: unlink %s failed", p);
        LOCK();
        if (s_status.seg_count) s_status.seg_count--;
        s_status.flash_full = false;   // #FW-27: сборщик освободил место → снять индикатор. Если кольцо реально переполнено, make_room поднимет флаг снова на следующей строке.
        UNLOCK();
        s_seg_pinned = 0xFFFFFFFFu;
        ESP_LOGI(TAG, "offload_done: seg_%05" PRIu32 " removed from flash", idx);
    }
    FSUNLOCK();
}

void spectrogram_offload_release(uint32_t idx)
{
    FSLOCK();
    if (idx == s_seg_pinned) s_seg_pinned = 0xFFFFFFFFu;  // файл оставляем для повтора
    FSUNLOCK();
}

// #REC-11 pull: удалить завершённый сегмент по индексу (PC-клиент подтвердил приём
// через POST /api/waterfall/segment/delete). В отличие от offload_done (снимает pin
// именно push-выгрузки), работает для любого завершённого сегмента, выбранного ПК.
// НИКОГДА не трогает СЕЙЧАС открытый сегмент и pin активной push-выгрузки. Так
// pull-модель освобождает Flash без входящих портов на ПК (ПК сам инициирует связь).
// Возвращает true, если файл удалён.
bool spectrogram_seg_delete(uint32_t idx)
{
    bool ok = false;
    FSLOCK();
    bool is_open   = (s_seg_fp && idx == s_seg_cur);
    bool is_pinned = (idx == s_seg_pinned);
    if (!is_open && !is_pinned) {
        char p[80];
        seg_path(p, sizeof(p), idx);
        struct stat sb;
        if (stat(p, &sb) == 0) {
            if (unlink(p) == 0) {
                LOCK();
                if (s_status.seg_count) s_status.seg_count--;
                s_status.flash_full = false;   // #FW-27: pull-ack освободил место → снять индикатор. make_room поднимет снова при реальном переполнении.
                UNLOCK();
                ok = true;
                ESP_LOGI(TAG, "seg_delete: seg_%05" PRIu32 ".aswf removed (pull-ack)", idx);
            } else {
                ESP_LOGW(TAG, "seg_delete: unlink seg_%05" PRIu32 " failed", idx);
            }
        }
    }
    FSUNLOCK();
    return ok;
}

// v3: дозовый коэффициент µSv/h per cps. Сохраняется в NVS как IEEE-754 bits.
void spectrogram_set_dose_k(float k)
{
    s_dose_k = k;
    nvs_handle_t h;
    if (nvs_open(WF_SETTINGS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    uint32_t bits; memcpy(&bits, &k, 4);
    nvs_set_u32(h, "dose_k_bits", bits);
    nvs_commit(h);
    nvs_close(h);
}

float spectrogram_get_dose_k(void)
{
    return s_dose_k;
}
