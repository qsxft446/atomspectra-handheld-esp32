#include "atomspectra.h"
#include "esp_log.h"
#include <inttypes.h>
#include "esp_littlefs.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <sys/stat.h>   // #FW-24: mkdir SPEC_DIR
#include <fcntl.h>      // #SD-8: open/write автосохранения
#include <unistd.h>
#include <dirent.h>
#include "sd_card.h"     // #SD-4: сохранённые спектры — на SD-карте, если она есть
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"     // #SB-1: окно после сброса

static const char *TAG = "spectrum";
#define AUTOSAVE_FILE STORAGE_PATH "/current.bin"
// #SD-8: при карте автосохранение живого спектра — на неё. Запись 33 КБ во флеш
// раз в минуту выключала кэш, приём USB терял свип, и строка водопада раз в 61 с
// выходила 3-секундной (в 1.5 раза ярче соседних — «артефакт»). Без карты — флеш.
#define AUTOSAVE_FILE_SD SD_MOUNT_POINT "/current.bin"
#define CALIB_FILE    STORAGE_PATH "/calib.bin"
#define AUTOSAVE_RESERVE (1024 * 1024)

typedef struct {
    char serial[64];
    double calibration[CALIB_COEFFS];
    int calib_order;
    uint8_t valid;
} calib_store_t;

static spectrum_data_t s_spectrum;
static device_info_t   s_device_info;

// #DEV-6: сырые тексты последних -inf / -tc_pot? ответов — источник для бэкапа
// настроек (см. spectrum_process_info_response/_tcpot_response).
// #FW-17: -inf с ПУСТЫМ PileUp[] ~404Б, но при реальном наборе таблица PileUp[]
// растёт до 99 элементов (значения-счётчики) → строка -inf ~1.2-1.5 КБ. Прежние
// 700Б молча резали её (store_raw_trimmed) на ~53-м элементе, теряя хвост PileUp[]
// и "PileUpThr N". 2048Б — с запасом на 99 крупных элементов. Tcpot-строка короче,
// ей 700Б хватает (20 пар термокомпенсации ~130Б).
static char     s_info_raw[2048];
static int      s_info_raw_len = 0;
static uint32_t s_info_raw_seq = 0;
static char     s_tcpot_raw[700];
static int      s_tcpot_raw_len = 0;
static uint32_t s_tcpot_raw_seq = 0;

// Защищает s_spectrum от гонки между CDC-таском (писатель) и httpd-таском (читатель).
static SemaphoreHandle_t s_spec_lock;
#define SPEC_LOCK()   do { if (s_spec_lock) xSemaphoreTake(s_spec_lock, portMAX_DELAY); } while (0)
#define SPEC_UNLOCK() do { if (s_spec_lock) xSemaphoreGive(s_spec_lock); } while (0)

// #WF-1: калибровка изменилась, требуется persist. Взводится под SPEC_LOCK
// (парсер -inf/-cal, set_calibration), гасится в spectrum_save_calibration
// (main loop) — flash-запись больше не выполняется под SPEC_LOCK в CDC/httpd.
static volatile bool s_calib_dirty;
// #CAL-1: номер разобранного дампа -cal (ждёт автосчитывание перед стартом набора)
static volatile uint32_t s_cal_dump_seq;
// #CAL-2: последний ПРИНЯТЫЙ дамп -cal (CRC сошёлся, калибровка ненулевая) — для
// проверки записи калибровки в прибор (usb_host_write_device_calibration).
static volatile uint32_t s_cal_dump_ok_seq;
static double            s_cal_dump_coef[CALIB_COEFFS];

// #FW-8: staging-сборка секундного свипа гистограммы. Прибор на 600000 бод шлёт
// ВЕСЬ спектр раз в секунду цепочкой chunk-ов: offset==0 — старт свипа, каждый
// следующий строго продолжает предыдущий, покрытие до 8192 каналов — свип полный
// (официальная спека AtomSpectra, пример приёма histogram). Во время flash erase
// (finalize+create сегмента водопада) кэш замораживает CDC-таск, и часть chunk-ов
// теряется: раньше они писались прямо в s_spectrum.bins, и живой спектр становился
// смесью старых и новых диапазонов каналов → дельта-строка водопада с рваными
// counts/dur («полосы на границах сегментов»). Теперь chunk-и собираются в staging
// (PSRAM) и публикуются в s_spectrum АТОМАРНО только полным свипом; рваный свип
// отбрасывается — живой спектр держит прошлый когерентный снимок. STAT-поля
// (total_time_sec и пр.) публикуются ВМЕСТЕ со свипом: время и counts замерзают/
// движутся синхронно, поэтому dur строк водопада остаётся честным.
// Staging трогает ТОЛЬКО CDC-таск (histogram и STAT приходят из одного feed_shproto)
// — лок на staging не нужен, SPEC_LOCK берётся только на публикацию.
static uint32_t *s_hist_staging;                  // [SPECTRUM_CHANNELS], PSRAM
static uint32_t  s_stage_next = UINT32_MAX;       // след. ожидаемый канал; UINT32_MAX = свип не активен
static bool      s_stage_ok = false;              // непрерывность с offset==0 не нарушена
static uint32_t  s_hist_commits = 0;              // опубликованных полных свипов
static uint32_t  s_hist_drops = 0;                // отброшенных рваных свипов
typedef struct {
    uint32_t total_time_sec;
    uint16_t cpu_load;
    uint32_t cps;
    uint32_t lost_impulses;
    uint32_t pulse_width;
    bool     fresh;                               // пришёл ли STAT после последнего commit
} stat_stage_t;
static stat_stage_t s_stat_stage;

// #SB-1 (перенос AWF-3 из апстрима, 1.2.24): прибор при обрыве питания обнуляет
// свою гистограмму и время. Раньше каждый свип целиком заменял спектр платы —
// после провала питания (батарея, #PWR-9) накопленное пропадало, а автосохранение
// затиралось первым же свипом. Теперь показываемый спектр = база + счёт прибора;
// перезапуск прибора (счёт меньше уже набранного, или при свежем STAT время
// упало больше чем вдвое) переносит показанный спектр в базу. Время переносится
// отдельно: прибор при перезагрузке командой обнуляет гистограмму, но НЕ своё время
// (замер 2026-10-06: время продолжилось с 7680) — база времени сдвигается, только
// когда время прибора пошло назад, чтобы показанное время шло без скачка. «Сброс»
// пользователя (кнопка, -rst из веба/по мосту) базу обнуляет и на
// SB_GUARD_US не даёт переносить: свипы, ушедшие до -rst, иначе воскресили бы
// старый спектр. База хранится в файле (SD или флеш) — переживает перезагрузку
// платы; пишет её main loop (spectrum_base_tick), не задача приёма USB.
#define SB_FILE_SD     SD_MOUNT_POINT "/base.bin"
#define SB_FILE_FLASH  STORAGE_PATH "/base.bin"
#define SB_MAGIC       0x32425353u          // 'SSB2' (SSB1 писала тестовая сборка с ошибкой времени)
#define SB_GUARD_US    (15LL * 1000000LL)
#define SB_TIME_TOL_S  5u
static uint32_t *s_base_bins;               // [SPECTRUM_CHANNELS], PSRAM
static uint32_t  s_base_time, s_base_counts;
static int64_t   s_base_guard_until_us;
static volatile bool s_base_dirty;          // базу записать (main loop)
static volatile uint32_t s_base_folds;      // переносов в базу (диагностика)
static volatile bool s_base_fold_log;       // перенос был — залогировать из main loop
static bool      s_session_first = true;    // первый коммит USB-сеанса ещё не опубликован
static uint8_t   s_session_defers;          // отложено свипов в начале сеанса (не больше 3)
static uint8_t   s_fold_quiet;              // после переноса по счёту — свипов без проверки по времени

static inline uint32_t sat_add(uint32_t a, uint32_t b)
{
    uint32_t r = a + b;
    return r < a ? UINT32_MAX : r;
}

// #FW-13 фикс №2: слушатели коммита свипа. Полный свип = конец USB-burst и начало
// тихого окна (~0.5 с до следующего свипа) — единственная фаза, где flash-запись
// (freeze кэша обоих ядер) не рвёт приём FTDI (FIFO 256 Б = 4.3 мс @600000 бод).
// Потребители (wf_task — строка водопада, main loop — autosave, monitor_task —
// серия CPS #MON-1) привязывают свои записи к этому окну через binary-семафоры.
// #MON-1: было 2 (wf_task + autosave, оба слота заняты) — монитору нужен третий;
// 4 = +1 запас (переполнение тихое: LOGW и потребитель живёт на fallback-тике).
#define COMMIT_LISTENERS_MAX 4
static SemaphoreHandle_t s_commit_listeners[COMMIT_LISTENERS_MAX];

void spectrum_add_commit_listener(void *sem)
{
    for (int i = 0; i < COMMIT_LISTENERS_MAX; i++)
        if (!s_commit_listeners[i]) { s_commit_listeners[i] = (SemaphoreHandle_t)sem; return; }
    ESP_LOGW(TAG, "commit listeners full");
}

// #OTA-2: служебные файлы внутреннего флеша — копия на SD-карте. Переход на
// таблицу разделов с OTA (#OTA-1) сдвигает раздел storage, и LittleFS
// переформатируется: калибровка, кривая дозы и состояние записи водопада
// пропали бы. Метка .mirror отличает свежий (только что отформатированный)
// флеш от обычного старта: на свежем файлы возвращаются с карты; на обычном
// флеш -> карта, а копии удалённых во флеше файлов с карты убираются (иначе
// удалённая кривая дозы воскресла бы после следующего форматирования).
#define MIRROR_DIR    SD_MOUNT_POINT "/flash_bak"
#define MIRROR_MARK   STORAGE_PATH "/.mirror"
static const char *const MIRROR_FILES[] = { "calib.bin", "dose_curve.csv", "wf_state.bin" };

static bool copy_file(const char *from, const char *to)
{
    char buf[512];
    int in = open(from, O_RDONLY);
    if (in < 0) return false;
    int out = open(to, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) { close(in); return false; }
    bool ok = true;
    ssize_t n;
    while ((n = read(in, buf, sizeof buf)) > 0)
        if (write(out, buf, n) != n) { ok = false; break; }
    if (n < 0) ok = false;
    close(in);
    if (close(out) != 0) ok = false;
    if (!ok) unlink(to);
    return ok;
}

static void storage_mirror(void)
{
    if (!sd_card_present()) return;
    mkdir(MIRROR_DIR, 0777);
    struct stat st;
    bool fresh = stat(MIRROR_MARK, &st) != 0;   // метки нет — флеш свежий (или первый старт с #OTA-2)
    char fp[64], sp[64];
    int copied = 0, restored = 0;
    for (size_t i = 0; i < sizeof MIRROR_FILES / sizeof MIRROR_FILES[0]; i++) {
        snprintf(fp, sizeof fp, "%s/%s", STORAGE_PATH, MIRROR_FILES[i]);
        snprintf(sp, sizeof sp, "%s/%s", MIRROR_DIR, MIRROR_FILES[i]);
        bool in_flash = stat(fp, &st) == 0;
        if (!in_flash && fresh && stat(sp, &st) == 0) {
            if (copy_file(sp, fp)) { restored++; ESP_LOGW(TAG, "#OTA-2: %s восстановлен с карты", MIRROR_FILES[i]); }
            continue;
        }
        if (in_flash && copy_file(fp, sp)) copied++;
        else if (!fresh) unlink(sp);
    }
    if (fresh) {
        int fd = open(MIRROR_MARK, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) close(fd);
    }
    ESP_LOGI(TAG, "#OTA-2: служебные файлы флеша: на карту %d, с карты %d%s", copied, restored,
             fresh ? " (флеш без метки — свежий)" : "");
}

void spectrum_init(void)
{
    s_spec_lock = xSemaphoreCreateMutex();
    memset(&s_spectrum, 0, sizeof(s_spectrum));
    memset(&s_device_info, 0, sizeof(s_device_info));
    // #FW-8: 32 КБ staging в PSRAM (пишется из CDC-таска ~130 chunk-ов/с — трафик
    // копеечный). Нет PSRAM → internal heap; нет и его → legacy-путь прямой записи.
    s_hist_staging = heap_caps_malloc(SPECTRUM_CHANNELS * sizeof(uint32_t),
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_base_bins = heap_caps_calloc(SPECTRUM_CHANNELS, sizeof(uint32_t),
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);   // #SB-1
    if (!s_base_bins) ESP_LOGE(TAG, "#SB-1: base alloc failed — device restart loses spectrum");
    if (!s_hist_staging)
        s_hist_staging = malloc(SPECTRUM_CHANNELS * sizeof(uint32_t));
    if (!s_hist_staging)
        ESP_LOGE(TAG, "hist staging alloc failed — fallback to direct bin writes");
    esp_vfs_littlefs_conf_t conf = {
        .base_path = STORAGE_PATH,
        .partition_label = "storage",
        .format_if_mount_failed = true,
    };
    esp_err_t ret = esp_vfs_littlefs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LittleFS mount failed: %s", esp_err_to_name(ret));
    } else {
        size_t total = 0, used = 0;
        esp_littlefs_info("storage", &total, &used);
        ESP_LOGI(TAG, "LittleFS: total=%zu used=%zu free=%zu", total, used, total - used);
        mkdir(SPEC_DIR, 0777);   // #FW-24: подкаталог сохранённых спектров (отделение от calib/current/wf_state в корне)
        storage_mirror();        // #OTA-2
    }
}

void spectrum_process_histogram_chunk(const uint8_t *data, size_t len)
{
    if (len < 6) return;
    uint16_t offset = data[0] | (data[1] << 8);
    size_t bin_count = (len - 2) / 4;

    if (!s_hist_staging) {
        // Legacy-путь (staging не выделился): прямая запись с инкрементальным total.
        SPEC_LOCK();
        int64_t delta = 0;
        for (size_t i = 0; i < bin_count && (offset + i) < SPECTRUM_CHANNELS; i++) {
            size_t idx = 2 + i * 4;
            uint32_t v = data[idx] | (data[idx+1] << 8) | (data[idx+2] << 16) | (data[idx+3] << 24);
            delta += (int64_t)v - (int64_t)s_spectrum.bins[offset + i];
            s_spectrum.bins[offset + i] = v;
        }
        s_spectrum.total_counts = (uint32_t)((int64_t)s_spectrum.total_counts + delta);
        s_spectrum.valid = true;
        SPEC_UNLOCK();
        return;
    }

    // #FW-8: сборка свипа в staging. offset==0 — старт нового свипа (официальная
    // спека); разрыв непрерывности = потерянный chunk (flash-freeze) → свип битый.
    if (offset == 0) {
        s_stage_next = 0;
        s_stage_ok = true;
    } else if ((uint32_t)offset != s_stage_next) {
        s_stage_ok = false;
    }
    for (size_t i = 0; i < bin_count && (offset + i) < SPECTRUM_CHANNELS; i++) {
        size_t idx = 2 + i * 4;
        s_hist_staging[offset + i] =
            data[idx] | (data[idx+1] << 8) | (data[idx+2] << 16) | (data[idx+3] << 24);
    }
    s_stage_next = (uint32_t)offset + (uint32_t)bin_count;

    if (s_stage_next >= SPECTRUM_CHANNELS) {
        if (s_stage_ok) {
            // Полный свип: сумма вне лока (staging приватен CDC-таску), публикация
            // атомарно — bins + STAT одним куском, время когерентно counts.
            uint64_t total = 0;
            for (size_t i = 0; i < SPECTRUM_CHANNELS; i++) total += s_hist_staging[i];
            uint32_t dev_total = total > UINT32_MAX ? UINT32_MAX : (uint32_t)total;
            SPEC_LOCK();
            // #SB-1: перезапуск прибора -> показанный спектр в базу.
            bool guard = esp_timer_get_time() < s_base_guard_until_us;
            if (s_base_bins && s_spectrum.valid && !guard) {
                uint32_t dev_expected = s_spectrum.total_counts - s_base_counts;
                bool reset = dev_total < dev_expected;
                // После переноса по счёту время прибора на следующих свипах идёт назад —
                // это не новый перезапуск (иначе счёт первых секунд ушёл бы в базу дважды):
                // время пересчитает сдвиг базы времени ниже. Замер: в свипе перезапуска
                // STAT ещё старый, обнулённое время приходит свипом позже.
                bool quiet = s_fold_quiet > 0;
                if (s_fold_quiet) s_fold_quiet--;
                if (reset) s_fold_quiet = 5;
                if (!reset && s_stat_stage.fresh && !quiet) {
                    uint32_t elapsed = s_spectrum.total_time_sec - s_base_time;
                    uint32_t t_dev = s_stat_stage.total_time_sec;
                    reset = t_dev + SB_TIME_TOL_S < elapsed && t_dev < elapsed / 2;
                }
                if (!reset && !s_stat_stage.fresh && s_session_first && s_session_defers < 3) {
                    // Первый свип USB-сеанса без STAT: перезапуск прибора с горячим
                    // источником (счёт успел перерасти прежний) по счёту не виден, а по
                    // времени без STAT решать нечем — ждём следующий свип.
                    s_session_defers++;
                    SPEC_UNLOCK();
                    s_stage_next = UINT32_MAX;
                    s_stage_ok = false;
                    return;
                }
                if (reset) {
                    // Время здесь НЕ переносится (см. выше) — его сдвигает блок времени ниже.
                    memcpy(s_base_bins, s_spectrum.bins, SPECTRUM_CHANNELS * sizeof(uint32_t));
                    s_base_counts = s_spectrum.total_counts;
                    s_base_dirty = true;
                    s_base_folds++;
                    s_base_fold_log = true;   // логировать нельзя: консоль блокирует приём USB (#FW-13)
                }
            }
            s_session_first = false;
            if (s_base_bins) {
                for (size_t i = 0; i < SPECTRUM_CHANNELS; i++)
                    s_spectrum.bins[i] = sat_add(s_base_bins[i], s_hist_staging[i]);
                s_spectrum.total_counts = sat_add(s_base_counts, dev_total);
            } else {
                memcpy(s_spectrum.bins, s_hist_staging, SPECTRUM_CHANNELS * sizeof(uint32_t));
                s_spectrum.total_counts = dev_total;
            }
            // Время ниже считается в шкале ПРИБОРА (dev), показывается base + dev.
            uint32_t base_t = s_base_bins ? s_base_time : 0;
            uint32_t dev_prev = s_spectrum.total_time_sec >= base_t ? s_spectrum.total_time_sec - base_t : 0;
            // #FW-12: время коммита не может опираться только на STAT — на
            // FIFO-burst протухший staged STAT неотличим от свежего (свип(t)
            // битый → его STAT остался staged; STAT(t+1) потерян → коммит
            // свипа(t+1) взял бы время t: bins на 1 c впереди → жирная строка
            // водопада). Опорная арифметика: каждый ПОЛНЫЙ свип = ровно 1 c
            // живого времени прибора, каждый ОТБРОШЕННЫЙ (drop) — ещё 1 c,
            // прожитый прибором между коммитами. Отсюда нижняя граница:
            //   expected = prev + 1 + drops_с_прошлого_коммита.
            // STAT принимаем не ниже expected (MAX): выше — легитимный резинк
            // (свип потерян ЦЕЛИКОМ, drop не увидел). Откат ≥5 c — рестарт
            // прибора, принимаем абсолют.
            {
                static uint32_t s_drops_at_commit = 0;
                uint32_t drops_delta = s_hist_drops - s_drops_at_commit;
                s_drops_at_commit = s_hist_drops;
                uint32_t expected = dev_prev + 1 + drops_delta;
                if (s_stat_stage.fresh) {
                    uint32_t t_new = s_stat_stage.total_time_sec;
                    if (s_spectrum.valid && s_base_bins && !guard && t_new + 5 < dev_prev) {   // после «Сброса» — время прибора как есть
                        // #SB-1: время прибора пошло назад (перезапуск с обнулением
                        // часов) — сдвинуть базу времени: показанное идёт дальше на 1 с.
                        uint32_t shown_prev = s_spectrum.total_time_sec;
                        base_t = shown_prev + 1 >= t_new ? shown_prev + 1 - t_new : 0;
                        s_base_time = base_t;
                        s_base_dirty = true;
                    } else if (s_spectrum.valid && t_new + 5 >= dev_prev &&
                        t_new < expected)
                        t_new = expected;      // протухший/отставший STAT
                    s_spectrum.total_time_sec = sat_add(base_t, t_new);
                    s_spectrum.cpu_load       = s_stat_stage.cpu_load;
                    s_spectrum.cps            = s_stat_stage.cps;
                    s_spectrum.lost_impulses  = s_stat_stage.lost_impulses;
                    s_spectrum.pulse_width    = s_stat_stage.pulse_width;
                    s_stat_stage.fresh = false;
                } else if (s_spectrum.valid) {
                    s_spectrum.total_time_sec = sat_add(base_t, expected);   // STAT потерян
                } else {
                    s_spectrum.total_time_sec = sat_add(base_t, dev_prev + 1);   // первый коммит без STAT
                }
            }
            s_spectrum.valid = true;
            SPEC_UNLOCK();
            s_hist_commits++;
            // #FW-13 фикс №2: сигнал «burst кончился, тихое окно открыто».
            for (int i = 0; i < COMMIT_LISTENERS_MAX; i++)
                if (s_commit_listeners[i]) xSemaphoreGive(s_commit_listeners[i]);
        } else {
            s_hist_drops++;
            ESP_LOGW(TAG, "histogram sweep dropped (gap in chunks), drops=%" PRIu32, s_hist_drops);
        }
        s_stage_next = UINT32_MAX;
        s_stage_ok = false;
    }
}

void spectrum_process_stat_packet(const uint8_t *data, size_t len)
{
    if (len < 10) return;
    // #FW-8: STAT — в staging, публикация вместе со свипом гистограммы (когерентность
    // время↔counts для dur строк водопада). Без staging — старый прямой путь.
    if (s_hist_staging) {
        s_stat_stage.total_time_sec = data[0] | (data[1]<<8) | (data[2]<<16) | (data[3]<<24);
        s_stat_stage.cpu_load = data[4] | (data[5] << 8);
        s_stat_stage.cps = data[6] | (data[7]<<8) | (data[8]<<16) | (data[9]<<24);
        if (len >= 14)
            s_stat_stage.lost_impulses = data[10] | (data[11]<<8) | (data[12]<<16) | (data[13]<<24);
        // #DT-4: суммарная ширина импульсов (отсчёты АЦП), STAT offset 14. Диагностика;
        // мёртвое время считается методом BecqMoni (RISE+FALL+1)/F, в расчёт не идёт.
        if (len >= 18)
            s_stat_stage.pulse_width = data[14] | (data[15]<<8) | (data[16]<<16) | (data[17]<<24);
        s_stat_stage.fresh = true;
        return;
    }
    SPEC_LOCK();
    s_spectrum.total_time_sec = data[0] | (data[1]<<8) | (data[2]<<16) | (data[3]<<24);
    s_spectrum.cpu_load = data[4] | (data[5] << 8);
    s_spectrum.cps = data[6] | (data[7]<<8) | (data[8]<<16) | (data[9]<<24);
    if (len >= 14)
        s_spectrum.lost_impulses = data[10] | (data[11]<<8) | (data[12]<<16) | (data[13]<<24);
    if (len >= 18)
        s_spectrum.pulse_width = data[14] | (data[15]<<8) | (data[16]<<16) | (data[17]<<24);
    SPEC_UNLOCK();
}

// #FW-8: диагностика сборки свипов для /api/spectrum JSON и верификации фикса.
void spectrum_get_hist_stats(uint32_t *commits, uint32_t *drops)
{
    if (commits) *commits = s_hist_commits;
    if (drops)   *drops   = s_hist_drops;
}
// Копирует text (без хвостовых \r\n\пробел) в raw-буфер фиксированного размера,
// бампает seq. Общий хелпер для -inf и -tc_pot? (вызывать ТОЛЬКО под SPEC_LOCK).
static void store_raw_trimmed(const char *text, char *buf, size_t bufsz, int *out_len, uint32_t *out_seq)
{
    size_t n = strlen(text);
    while (n > 0 && (text[n-1] == '\n' || text[n-1] == '\r' || text[n-1] == ' ')) n--;
    if (n >= bufsz) n = bufsz - 1;
    memcpy(buf, text, n);
    buf[n] = '\0';
    *out_len = (int)n;
    (*out_seq)++;
}

void spectrum_process_info_response(const char *text)
{
    SPEC_LOCK();
    // #BRIDGE-3: этот вход обслуживает ДВА разных ответа прибора — роутер
    // (usb_host_cdc.c) шлёт сюда и -inf (параметры+температура), и -cal (дамп
    // 40 hex-регистров: калибровка+CRC+серийник). Различаем по содержимому:
    // -inf несёт ключ "VERSION ", -cal ключей не содержит. РАНЬШЕ -cal шёл в ту
    // же ветку memset(device_info)+key-парсер → ключей нет → температура и версия
    // обнулялись (симптом «t=0»); серийник брался из lbuf[39] безусловно и при
    // сдвиге строк (склейка под нагрузкой bridge) вставал в FFFFFFFF — в дампе
    // 28 строк FFFFFFFF («пустые» слоты калибровки), любой сдвиг индекса туда попадал.
    bool is_inf = (strstr(text, "VERSION ") != NULL);
    // static: 3 КБ парс-буфера не на стеке CDC-таска (P1-1). Функция целиком
    // выполняется под SPEC_LOCK, единственный путь вызова — безопасно.
    static char lbuf[48][64];
    int lcount = 0;
    const char *lp = text;
    while (*lp && lcount < 48) {
        int li = 0;
        while (*lp && *lp != '\n' && *lp != '\r' && li < 63)
            lbuf[lcount][li++] = *lp++;
        lbuf[lcount][li] = '\0';
        while (*lp == '\n' || *lp == '\r') lp++;
        lcount++;
    }
    // #FW-13: LOGD, не LOGI — функция в CDC-таске, консоль UART0 115200 блокирующая
    // (FTDI FT232R 256 Б FIFO = 4.3 мс при 600000 бод → overflow, gap в chunk-ах).
    ESP_LOGD(TAG, "Info response: %d lines (%s)", lcount, is_inf ? "inf" : "cal");
    // Калибровка: только у -cal (CRC L[0..9]==L[10], ≥40 строк). Для -inf это
    // одна строка (lcount<11) → блок штатно пропускается, калибровку не трогает.
    if (lcount >= 11) {
        char hcat[256] = {0};
        for (int i = 0; i < 10; i++)
            strncat(hcat, lbuf[i], sizeof(hcat) - strlen(hcat) - 1);
        // #CMD-1: прибор считает СТАНДАРТНЫЙ CRC32 (init 0xFFFFFFFF, рефлексия,
        // финальный XOR 0xFFFFFFFF) по ASCII-конкатенации регистров L[0..9].
        // Подтверждено на реальном дампе -cal: CRC32("BFF9A132...00000000")=DF786A7E,
        // совпадает с регистром L[10]. Прежний init=0 без финального XOR давал mismatch.
        uint32_t cc = 0xFFFFFFFF;
        for (int i = 0; hcat[i]; i++) {
            cc ^= (uint8_t)hcat[i];
            for (int j = 0; j < 8; j++) {
                if (cc & 1) cc = (cc >> 1) ^ 0xEDB88320;
                else cc >>= 1;
            }
        }
        cc ^= 0xFFFFFFFF;
        uint32_t ce = (uint32_t)strtoul(lbuf[10], NULL, 16);
        if (cc == ce) {
            double cal[CALIB_COEFFS] = {0};
            bool nonzero = false;
            for (int c = 0; c < CALIB_COEFFS && (c*2+1) < 10; c++) {
                char pair[128];
                // #PERF-1: -O2 видит, что строки lbuf могут быть длиннее буфера; регистр —
                // 8 hex-символов, ограничение половин 63 символами их не касается.
                snprintf(pair, sizeof(pair), "%.63s%.63s", lbuf[c*2], lbuf[c*2+1]);
                uint64_t raw = strtoull(pair, NULL, 16);
                memcpy(&cal[c], &raw, sizeof(double));
                if (cal[c] != 0.0) nonzero = true;
            }
            if (nonzero) {
                memcpy(s_spectrum.calibration, cal, sizeof(cal));
                int order = CALIB_COEFFS - 1;
                while (order > 0 && s_spectrum.calibration[order] == 0.0) order--;
                s_spectrum.calib_order = order;
                s_spectrum.calib_valid = true;
                s_calib_dirty = true;   // #WF-1: запись сделает main loop вне SPEC_LOCK
                memcpy(s_cal_dump_coef, cal, sizeof(cal));   // #CAL-2
                s_cal_dump_ok_seq++;
                ESP_LOGI(TAG, "Calibration OK: order=%d", s_spectrum.calib_order);
            } else {
                // #CAL-1 (апстрим 1.2.28): пустая калибровка прибора калибровку платы не затирает
                ESP_LOGI(TAG, "Calibration from device is empty — board calibration kept");
            }
        } else {
            // #FW-13: LOGD — для -inf mismatch штатен (CRC-формат только у -cal),
            // WARN здесь печатался каждые 30 с в CDC-таске (см. комментарий выше).
            ESP_LOGD(TAG, "Calibration CRC mismatch: computed=%08x expected=%08x", (unsigned)cc, (unsigned)ce);
        }
    }

    if (!is_inf && lcount >= 11) s_cal_dump_seq++;   // #CAL-1: дамп разобран (принят или нет)
    if (!is_inf) {
        // #BRIDGE-3: -cal обновляет ТОЛЬКО серийник (L39), device_info/температуру
        // НЕ трогает и s_info_raw (сырой -inf для бэкапа) не перетирает. Серийник —
        // с проверкой: ровно 8 hex-символов и не «FFFFFFFF» (пустой слот). При
        // сдвиге строк lbuf[39] попадает в блок FFFFFFFF → отбрасываем, прежнее
        // валидное значение сохраняется (серийник больше не «портится» под bridge).
        if (lcount >= 40) {
            const char *sn = lbuf[39];
            size_t sl = strlen(sn);
            bool ok = (sl == 8);
            int fcnt = 0;
            for (size_t i = 0; ok && i < sl; i++) {
                char c = sn[i];
                if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'))) ok = false;
                if (c == 'F' || c == 'f') fcnt++;
            }
            if (ok && fcnt != 8) {
                strncpy(s_spectrum.serial_number, sn, sizeof(s_spectrum.serial_number) - 1);
                ESP_LOGI(TAG, "Serial: %s", s_spectrum.serial_number);
            } else {
                ESP_LOGD(TAG, "Serial rejected (misaligned -cal): \"%s\"", sn);
            }
        }
        SPEC_UNLOCK();
        return;
    }

    // ===== -inf: параметры прибора + температура + версия (серийника не несёт) =====
    // #DEV-6: сырой текст -inf для бэкапа настроек — структурный парсер ниже
    // хранит лишь подмножество полей (POT2/Tco[]/PileUp[]/PileUpThr/Prise/Pfall/
    // TCpot он молча пропускает), бэкап берёт полную строку.
    store_raw_trimmed(text, s_info_raw, sizeof(s_info_raw), &s_info_raw_len, &s_info_raw_seq);
    device_info_t *d = &s_device_info;
    memset(d, 0, sizeof(*d));
    const char *p = text;
    while (*p) {
        while (*p == ' ' || *p == '\n' || *p == '\r') p++;
        if (!*p) break;
        char key[32] = {0};
        int ki = 0;
        while (*p && *p != ' ' && *p != '\n' && ki < 30) key[ki++] = *p++;
        key[ki] = 0;
        while (*p == ' ') p++;
        if (strcmp(key, "DEV") == 0) d->dev = atoi(p);
        else if (strcmp(key, "VERSION") == 0) d->version = atoi(p);
        else if (strcmp(key, "RISE") == 0) d->rise = atoi(p);
        else if (strcmp(key, "FALL") == 0) d->fall = atoi(p);
        else if (strcmp(key, "Srise") == 0) d->srise = atoi(p);
        else if (strcmp(key, "Sfall") == 0) d->sfall = atoi(p);
        else if (strcmp(key, "NOISE") == 0) d->noise = atoi(p);
        else if (strcmp(key, "F") == 0) d->freq = atof(p);
        else if (strcmp(key, "MAX") == 0) d->max_integral = atoi(p);
        else if (strcmp(key, "HYST") == 0) d->hyst = atoi(p);
        else if (strcmp(key, "MODE") == 0) d->mode = atoi(p);
        else if (strcmp(key, "STEP") == 0) d->step = atoi(p);
        else if (strcmp(key, "t") == 0) d->time_sec = atoi(p);
        else if (strcmp(key, "POT") == 0) d->pot = atoi(p);
        else if (strcmp(key, "T1") == 0) d->t1 = atof(p);
        else if (strcmp(key, "T2") == 0) d->t2 = atof(p);
        else if (strcmp(key, "T3") == 0) d->t3 = atof(p);
        else if (strcmp(key, "TC") == 0) d->tc_on = (strncmp(p, "ON", 2) == 0);
        else if (strcmp(key, "TP") == 0) d->tp = atoi(p);
        if (*p == '[') { while (*p && *p != ']') p++; if (*p==']') p++; }
        else { while (*p && *p != ' ' && *p != '\n') p++; }
    }
    d->valid = true;
    s_spectrum.temperature[0] = d->t1;
    s_spectrum.temperature[1] = d->t2;
    s_spectrum.temperature[2] = d->t3;
    SPEC_UNLOCK();
}
// #DEV-6: ответ на -tc_pot? ("Tcpot [-40 51 -16 45 ...]") — отдельная команда,
// таблица баз. темп. компенсации НЕ входит в -inf (см. #DOC-3/BUG-AS-08).
void spectrum_process_tcpot_response(const char *text)
{
    SPEC_LOCK();
    store_raw_trimmed(text, s_tcpot_raw, sizeof(s_tcpot_raw), &s_tcpot_raw_len, &s_tcpot_raw_seq);
    SPEC_UNLOCK();
}

int spectrum_get_info_raw(char *out, size_t outsz, uint32_t *out_seq)
{
    SPEC_LOCK();
    int n = s_info_raw_len < (int)outsz - 1 ? s_info_raw_len : (int)outsz - 1;
    if (n > 0) memcpy(out, s_info_raw, n);
    out[n] = '\0';
    if (out_seq) *out_seq = s_info_raw_seq;
    SPEC_UNLOCK();
    return n;
}

int spectrum_get_tcpot_raw(char *out, size_t outsz, uint32_t *out_seq)
{
    SPEC_LOCK();
    int n = s_tcpot_raw_len < (int)outsz - 1 ? s_tcpot_raw_len : (int)outsz - 1;
    if (n > 0) memcpy(out, s_tcpot_raw, n);
    out[n] = '\0';
    if (out_seq) *out_seq = s_tcpot_raw_seq;
    SPEC_UNLOCK();
    return n;
}

// #SB-1: обнулить базу и закрыть переносы на SB_GUARD_US. Под SPEC_LOCK.
static void spectrum_base_clear_locked(void)
{
    s_fold_quiet = 0;
    if (s_base_bins) memset(s_base_bins, 0, SPECTRUM_CHANNELS * sizeof(uint32_t));
    s_base_time = s_base_counts = 0;
    s_base_guard_until_us = esp_timer_get_time() + SB_GUARD_US;
    s_base_dirty = true;
}

// #SB-1: прибору ушла команда сброса не через spectrum_reset() (/api/command,
// TCP-мост): сам прибор обнулится — база тоже, иначе старый спектр остался бы.
void spectrum_base_clear(void)
{
    SPEC_LOCK();
    spectrum_base_clear_locked();
    SPEC_UNLOCK();
}

// #SB-1: новый USB-сеанс (CDC открыт) — STAT прошлого сеанса не годится, первый
// свип без STAT откладывается (см. коммит).
void spectrum_usb_session_start(void)
{
    SPEC_LOCK();
    s_stat_stage.fresh = false;
    s_session_first = true;
    s_session_defers = 0;
    SPEC_UNLOCK();
}

static const char *base_path(void) { return sd_card_present() ? SB_FILE_SD : SB_FILE_FLASH; }

// #SB-1: из main loop раз в 10 с: запись базы и лог переноса. После переноса
// сразу и автосохранение: иначе после сбоя питания старый current.bin (до 60 с)
// оказался бы меньше базы (см. spectrum_restore_autosave).
void spectrum_base_tick(void)
{
    if (s_base_fold_log) {
        s_base_fold_log = false;
        ESP_LOGW(TAG, "#SB-1: перезапуск прибора — спектр перенесён в базу (%" PRIu32 " отсч., %" PRIu32 " с), переносов %" PRIu32,
                 s_base_counts, s_base_time, s_base_folds);
    }
    if (!s_base_dirty || !s_base_bins) return;
    s_base_dirty = false;
    uint32_t *buf = heap_caps_malloc((SPECTRUM_CHANNELS + 3) * sizeof(uint32_t), MALLOC_CAP_SPIRAM);
    if (!buf) { s_base_dirty = true; return; }
    SPEC_LOCK();
    buf[0] = SB_MAGIC;
    buf[1] = s_base_time;
    buf[2] = s_base_counts;
    memcpy(buf + 3, s_base_bins, SPECTRUM_CHANNELS * sizeof(uint32_t));
    SPEC_UNLOCK();
    const char *path = base_path();
    if (buf[2] == 0 && buf[1] == 0) {
        unlink(path);   // пустая база — файла нет
    } else {
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        ssize_t n = (SPECTRUM_CHANNELS + 3) * sizeof(uint32_t);
        if (fd < 0 || write(fd, buf, n) != n) {
            ESP_LOGE(TAG, "#SB-1: base write %s failed", path);
            s_base_dirty = true;
        }
        if (fd >= 0) close(fd);
        spectrum_autosave();
    }
    free(buf);
}

void spectrum_get_base_info(uint32_t *counts, uint32_t *time_sec, uint32_t *folds)
{
    SPEC_LOCK();
    if (counts)   *counts = s_base_counts;
    if (time_sec) *time_sec = s_base_time;
    if (folds)    *folds = s_base_folds;
    SPEC_UNLOCK();
}

void spectrum_reset(void)
{
    // #FW-8: свип, начатый до reset, не должен закоммитить дорезетные данные.
    // Вызов идёт из httpd-таска — гонка с CDC на s_stage_* worst-case даёт один
    // лишний commit старого свипа, который следующий свип перепишет; не критично.
    s_stage_next = UINT32_MAX;
    s_stage_ok = false;
    s_stat_stage.fresh = false;
    SPEC_LOCK();
    spectrum_base_clear_locked();   // #SB-1
    memset(s_spectrum.bins, 0, sizeof(s_spectrum.bins));
    s_spectrum.total_counts = 0;
    s_spectrum.total_time_sec = 0;
    s_spectrum.cps = 0;
    s_spectrum.lost_impulses = 0;
    s_spectrum.pulse_width = 0;
    s_spectrum.valid = false;
    SPEC_UNLOCK();
}

// ВНИМАНИЕ: возвращают сырой указатель на разделяемое состояние БЕЗ лока.
// Допустимо ТОЛЬКО для логов/статуса (отдельные скалярные поля, чтение которых
// атомарно на ESP32). Для согласованного снимка многополевых данных
// (bins[]+total+calib) использовать spectrum_get_snapshot().
const spectrum_data_t *spectrum_get_current(void) { return &s_spectrum; }
const device_info_t   *spectrum_get_device_info(void) { return &s_device_info; }

// Атомарный снимок спектра под локом — для сетевых читателей (export/spectrum).
// Лок держится только на время memcpy; httpd_resp_send идёт уже по копии.
bool spectrum_get_snapshot(spectrum_data_t *out)
{
    SPEC_LOCK();
    memcpy(out, &s_spectrum, sizeof(*out));
    SPEC_UNLOCK();
    return out->valid;
}

// #MON-1: атомарная пара (total_counts, total_time_sec) под тем же SPEC_LOCK,
// каким защищена публикация коммита свипа. Дешёвая альтернатива
// spectrum_get_snapshot() для потребителей раз-в-секунду (монитор CPS):
// не копирует 32 КБ bins на каждый тик.
void spectrum_get_totals(uint32_t *counts, uint32_t *time_sec)
{
    SPEC_LOCK();
    if (counts)   *counts   = s_spectrum.total_counts;
    if (time_sec) *time_sec = s_spectrum.total_time_sec;
    SPEC_UNLOCK();
}

// #ALM-1: CPS последнего свипа (то же число, что крупно на экране) и время набора —
// сигнализации раз в 50 мс; без копии 32 КБ bins.
bool spectrum_get_cps(uint32_t *cps, uint32_t *time_sec)
{
    SPEC_LOCK();
    bool v = s_spectrum.valid;
    if (cps)      *cps      = s_spectrum.cps;
    if (time_sec) *time_sec = s_spectrum.total_time_sec;
    SPEC_UNLOCK();
    return v;
}

// #SD-4 (запрос пользователя 2026-09-29): сохранённые спектры живут на SD-карте,
// если она смонтирована при старте, иначе — во внутреннем флеше, как раньше.
// Формат файлов тот же (spec_NNNN.bin = spectrum_data_t), поэтому список,
// наложение, экспорт и удаление в вебе работают без изменений — меняется каталог.
#define SPEC_DIR_SD  SD_MOUNT_POINT "/spec"
#define SD_RESERVE   (1024 * 1024)   // не забивать карту в ноль

bool spectrum_saved_on_sd(void) { return sd_card_present(); }
const char *spectrum_saved_dir(void) { return sd_card_present() ? SPEC_DIR_SD : SPEC_DIR; }

// Первый старт с картой: спектры, сохранённые раньше во флеш, копируются на
// карту (если там ещё нет ни одного), чтобы не пропасть из списка в вебе.
// Оригиналы во флеше НЕ удаляются.
void spectrum_saved_migrate_to_sd(void)
{
    if (!sd_card_present()) return;
    mkdir(SPEC_DIR_SD, 0777);
    DIR *d = opendir(SPEC_DIR_SD);
    struct dirent *de;
    int i;
    while (d && (de = readdir(d)) != NULL) {
        if (sscanf(de->d_name, "spec_%d.bin", &i) == 1) { closedir(d); return; }   // уже есть
    }
    if (d) closedir(d);
    spectrum_data_t *buf = heap_caps_malloc(sizeof(*buf), MALLOC_CAP_SPIRAM);
    DIR *src = opendir(SPEC_DIR);
    int copied = 0, failed = 0;
    while (buf && src && (de = readdir(src)) != NULL) {
        if (sscanf(de->d_name, "spec_%d.bin", &i) != 1 || i < 0) continue;
        char from[64], to[64];
        snprintf(from, sizeof(from), "%s/spec_%04d.bin", SPEC_DIR, i);
        snprintf(to, sizeof(to), "%s/spec_%04d.bin", SPEC_DIR_SD, i);
        FILE *f = fopen(from, "rb");
        size_t rd = f ? fread(buf, sizeof(*buf), 1, f) : 0;
        if (f) fclose(f);
        FILE *o = rd == 1 ? fopen(to, "wb") : NULL;
        if (o) setvbuf(o, NULL, _IONBF, 0);   // без stdio-буфера во внутренней RAM
        bool ok = o && fwrite(buf, sizeof(*buf), 1, o) == 1;
        if (o && fclose(o) != 0) ok = false;
        if (ok) copied++;
        else { failed++; remove(to); }
    }
    if (src) closedir(src);
    free(buf);
    if (copied || failed)
        ESP_LOGI(TAG, "#SD-4: сохранённые спектры перенесены на карту: %d, ошибок %d (во флеше оставлены)",
                 copied, failed);
}

int spectrum_save_to_flash(void)
{
    // #SD-5 (решение пользователя 2026-09-29): сохранять ТОЛЬКО на SD-карту.
    // Нет карты — ошибка, во внутренний флеш не пишем (запись во флеш роняет
    // свипы прибора, и флеш почти полон).
    if (!sd_card_present()) {
        ESP_LOGW(TAG, "Save rejected: no SD card");
        return -4;
    }
    spectrum_data_t *snap = malloc(sizeof(*snap));
    if (!snap) return -3;
    SPEC_LOCK();
    if (!s_spectrum.valid) { SPEC_UNLOCK(); free(snap); return -1; }  // #FW-24: нет валидного спектра
    s_spectrum.saved_at = time(NULL);
    memcpy(snap, &s_spectrum, sizeof(*snap));
    SPEC_UNLOCK();

    const char *dir = spectrum_saved_dir();   // #SD-4
    if (sd_card_present()) {
        uint64_t sd_total = 0, sd_free = 0;
        mkdir(SPEC_DIR_SD, 0777);
        if (!sd_card_space(&sd_total, &sd_free) || sd_free < SD_RESERVE + sizeof(spectrum_data_t)) {
            ESP_LOGW(TAG, "Save rejected: SD free=%llu", (unsigned long long)sd_free);
            free(snap);
            return -2;
        }
    } else {
        size_t total = 0, used = 0;
        esp_littlefs_info("storage", &total, &used);
        if (total - used < AUTOSAVE_RESERVE + sizeof(spectrum_data_t)) {
            ESP_LOGW(TAG, "Save rejected: free=%zu < reserve=%d", total - used, AUTOSAVE_RESERVE);
            free(snap);
            return -2;
        }
    }
    char path[64];
    int idx = 0;
    FILE *f;
    while (idx < 9999) {
        snprintf(path, sizeof(path), "%s/spec_%04d.bin", dir, idx);
        f = fopen(path, "r");
        if (!f) break;
        fclose(f);
        idx++;
    }
    f = fopen(path, "wb");
    if (!f) { ESP_LOGE(TAG, "Cannot create %s", path); free(snap); return -3; }
    setvbuf(f, NULL, _IONBF, 0);   // #SD-4: без stdio-буфера во внутренней RAM
    size_t wr = fwrite(snap, sizeof(*snap), 1, f);
    int fc = fclose(f);
    if (wr != 1 || fc != 0) {
        ESP_LOGE(TAG, "Write to %s failed (wr=%zu fc=%d), removing", path, wr, fc);
        remove(path);
        free(snap);
        return -3;
    }
    ESP_LOGI(TAG, "Saved spectrum to %s (%" PRIu32 " counts)", path, snap->total_counts);
    free(snap);
    return idx;
}

int spectrum_load_from_flash(int index, spectrum_data_t *out)
{
    char path[64];
    snprintf(path, sizeof(path), "%s/spec_%04d.bin", spectrum_saved_dir(), index);   // #SD-4
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t rd = fread(out, 1, sizeof(*out), f);
    fclose(f);
    return (rd == sizeof(*out)) ? 0 : -1;
}

// #WF-1: флеш-запись калибровки вынесена из-под SPEC_LOCK и из CDC/httpd-тасков.
// Парсер -inf/-cal (CDC-таск) и spectrum_set_calibration (httpd) лишь взводят
// s_calib_dirty под SPEC_LOCK; фактическую запись на LittleFS (freeze кэша
// обоих ядер) делает main loop (10-с тик) ВНЕ лока — SPEC_LOCK не держится
// на время flash-операции и приём USB не останавливается.
// Вызывать БЕЗ SPEC_LOCK (берёт его сам для снапшота).
void spectrum_save_calibration(void)
{
    if (!s_calib_dirty) return;
    calib_store_t st = {0};
    SPEC_LOCK();
    if (!s_spectrum.calib_valid) { s_calib_dirty = false; SPEC_UNLOCK(); return; }
    strncpy(st.serial, s_spectrum.serial_number, sizeof(st.serial) - 1);
    memcpy(st.calibration, s_spectrum.calibration, sizeof(st.calibration));
    st.calib_order = s_spectrum.calib_order;
    st.valid = 1;
    s_calib_dirty = false;
    SPEC_UNLOCK();
    FILE *f = fopen(CALIB_FILE, "wb");
    if (!f) return;
    size_t wr = fwrite(&st, sizeof(st), 1, f);
    if (fclose(f) != 0 || wr != 1) {
        ESP_LOGE(TAG, "Calibration write failed (wr=%zu)", wr);
        return;
    }
    ESP_LOGI(TAG, "Calibration saved for '%s'", st.serial);
    if (sd_card_present()) copy_file(CALIB_FILE, MIRROR_DIR "/calib.bin");   // #OTA-2
}

// #CAL-1: калибровка платы задана — флаг и хотя бы один ненулевой коэффициент
bool spectrum_calib_is_set(void)
{
    bool set = false;
    SPEC_LOCK();
    if (s_spectrum.calib_valid)
        for (int i = 0; i < CALIB_COEFFS; i++)
            if (s_spectrum.calibration[i] != 0.0) set = true;
    SPEC_UNLOCK();
    return set;
}

uint32_t spectrum_cal_dump_seq(void) { return s_cal_dump_seq; }

// #CAL-2: номер последнего принятого дампа -cal и его коэффициенты (под SPEC_LOCK).
uint32_t spectrum_cal_dump_last(double out[CALIB_COEFFS])
{
    SPEC_LOCK();
    uint32_t seq = s_cal_dump_ok_seq;
    if (out) memcpy(out, s_cal_dump_coef, sizeof(s_cal_dump_coef));
    SPEC_UNLOCK();
    return seq;
}

void spectrum_set_calibration(const double *coeffs, int order)
{
    SPEC_LOCK();
    for (int i = 0; i <= order && i < CALIB_COEFFS; i++)
        s_spectrum.calibration[i] = coeffs[i];
    for (int i = order + 1; i < CALIB_COEFFS; i++)
        s_spectrum.calibration[i] = 0.0;
    s_spectrum.calib_order = order;
    s_spectrum.calib_valid = true;
    ESP_LOGI(TAG, "Manual calibration set: order=%d c0=%.6g c1=%.6g", order,
             s_spectrum.calibration[0], s_spectrum.calibration[1]);
    s_calib_dirty = true;   // #WF-1: запись сделает main loop вне SPEC_LOCK
    SPEC_UNLOCK();
}

void spectrum_load_calibration(void)
{
    FILE *f = fopen(CALIB_FILE, "rb");
    if (!f) return;
    calib_store_t st = {0};
    size_t rd = fread(&st, 1, sizeof(st), f);
    fclose(f);
    if (rd != sizeof(st) || !st.valid) return;
    SPEC_LOCK();
    memcpy(s_spectrum.calibration, st.calibration, sizeof(st.calibration));
    s_spectrum.calib_order = st.calib_order;
    s_spectrum.calib_valid = true;
    if (st.serial[0] && !s_spectrum.serial_number[0])
        strncpy(s_spectrum.serial_number, st.serial, sizeof(s_spectrum.serial_number) - 1);
    SPEC_UNLOCK();
    ESP_LOGI(TAG, "Calibration loaded: order=%d serial='%s'", st.calib_order, st.serial);
}

void spectrum_autosave(void)
{
    spectrum_data_t *snap = malloc(sizeof(*snap));
    if (!snap) return;
    SPEC_LOCK();
    if (!s_spectrum.valid) { SPEC_UNLOCK(); free(snap); return; }
    memcpy(snap, &s_spectrum, sizeof(*snap));
    SPEC_UNLOCK();
    // #SD-8: одним write() без буфера stdio (он взял бы внутреннюю RAM).
    const char *path = sd_card_present() ? AUTOSAVE_FILE_SD : AUTOSAVE_FILE;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { ESP_LOGE(TAG, "Autosave open %s failed", path); free(snap); return; }
    ssize_t wr = write(fd, snap, sizeof(*snap));
    if (close(fd) != 0 || wr != (ssize_t)sizeof(*snap))
        ESP_LOGE(TAG, "Autosave %s write failed (wr=%d)", path, (int)wr);
    free(snap);
}

// #SD-8: прочитать автосохранение из path в s_spectrum (под SPEC_LOCK). true — годное.
static bool autosave_read(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    ssize_t rd = read(fd, &s_spectrum, sizeof(s_spectrum));
    close(fd);
    if (rd == (ssize_t)sizeof(s_spectrum) && s_spectrum.valid) {
        ESP_LOGI(TAG, "Restored autosave %s: %" PRIu32 " counts, %" PRIu32 "s", path,
                 s_spectrum.total_counts, s_spectrum.total_time_sec);
        return true;
    }
    memset(&s_spectrum, 0, sizeof(s_spectrum));
    return false;
}

void spectrum_restore_autosave(void)
{
    // #SD-8: с карты; нет её / файла / файл битый (сброс посреди записи) — с флеша
    // (там последняя копия до #SD-8 или без карты).
    SPEC_LOCK();
    if (!(sd_card_present() && autosave_read(AUTOSAVE_FILE_SD)))
        autosave_read(AUTOSAVE_FILE);
    // #SB-1: база. Показанный спектр = база + прибор >= базы; автосохранение
    // старше переноса (сбой питания в те 10 с) — взять базу как показанный.
    if (s_base_bins) {
        uint32_t *buf = heap_caps_malloc((SPECTRUM_CHANNELS + 3) * sizeof(uint32_t), MALLOC_CAP_SPIRAM);
        int fd = buf ? open(base_path(), O_RDONLY) : -1;
        ssize_t n = (SPECTRUM_CHANNELS + 3) * sizeof(uint32_t);
        if (fd >= 0 && read(fd, buf, n) == n && buf[0] == SB_MAGIC) {
            s_base_time = buf[1];
            s_base_counts = buf[2];
            memcpy(s_base_bins, buf + 3, SPECTRUM_CHANNELS * sizeof(uint32_t));
            if (!s_spectrum.valid || s_spectrum.total_counts < s_base_counts ||
                s_spectrum.total_time_sec < s_base_time) {
                memcpy(s_spectrum.bins, s_base_bins, SPECTRUM_CHANNELS * sizeof(uint32_t));
                s_spectrum.total_counts = s_base_counts;
                s_spectrum.total_time_sec = s_base_time;
                s_spectrum.valid = true;
            }
            ESP_LOGI(TAG, "#SB-1: база %" PRIu32 " отсч., %" PRIu32 " с", s_base_counts, s_base_time);
        }
        if (fd >= 0) close(fd);
        free(buf);
    }
    SPEC_UNLOCK();
}

int spectrum_delete_from_flash(int index)
{
    char path[64];
    snprintf(path, sizeof(path), "%s/spec_%04d.bin", spectrum_saved_dir(), index);   // #SD-4
    if (remove(path) != 0) return -1;
    ESP_LOGI(TAG, "Deleted %s", path);
    return 0;
}