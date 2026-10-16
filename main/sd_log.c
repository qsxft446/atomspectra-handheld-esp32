// #SD-3: см. sd_log.h. Внутренней RAM у платы впритык (#TFT-17, #SD-2), поэтому:
// stdio-буферы выключены (setvbuf _IONBF — newlib иначе берёт их из кучи
// по умолчанию), данные копятся в своём буфере в PSRAM и пишутся крупно.
#include "sd_log.h"
#include "sd_card.h"
#include "monitor.h"       // monitor_wall_of()
#include "boot_config.h"   // boot_config_tz_min()
#include "web_util.h"      // web_build_export_name() — префикс имён экспорта (#FW-42)

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <inttypes.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_log.h"
#include "esp_heap_caps.h"

static const char *TAG = "sdlog";

#define DIR_MON    SD_MOUNT_POINT "/monitor"
#define SANE_EPOCH 1700000000   // 2023-11-14: раньше — часы платы не выставлены
#define CHUNK      4096

// Локальное время отображения: Unix UTC + пояс из веба. 0 — часов нет.
static time_t local_now(void)
{
    time_t t = time(NULL);
    return t < SANE_EPOCH ? 0 : t + (time_t)boot_config_tz_min() * 60;
}

static void fmt_iso(time_t local, char *out, size_t n)   // isoLocal() веба
{
    struct tm tm;
    gmtime_r(&local, &tm);
    snprintf(out, n, "%04d-%02d-%02d %02d:%02d:%02d", tm.tm_year + 1900, tm.tm_mon + 1,
             tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
}

static void fmt_stamp(time_t local, char *out, size_t n)   // ГГГГММДД_ччммсс для имён
{
    struct tm tm;
    gmtime_r(&local, &tm);
    snprintf(out, n, "%04d%02d%02d_%02d%02d%02d", tm.tm_year + 1900, tm.tm_mon + 1,
             tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
}

static void tz_label(char *out, size_t n)   // tzLabel() веба
{
    int o = boot_config_tz_min(), a = o < 0 ? -o : o;
    snprintf(out, n, "UTC%c%02d:%02d", o < 0 ? '-' : '+', a / 60, a % 60);
}

static FILE *open_new(const char *dir, const char *base, char *path, size_t n)
{
    mkdir(dir, 0775);   // уже есть — EEXIST, не ошибка
    char named[128];
    web_build_export_name(named, sizeof named, base);
    snprintf(path, n, "%s/%s", dir, named);
    // #MON-8: два сохранения в одну секунду — одно имя; второе не затирает первое, а _2, _3...
    struct stat st;
    const char *dot = strrchr(named, '.');
    int stem = dot ? (int)(dot - named) : (int)strlen(named);
    for (int k = 2; stat(path, &st) == 0 && k < 100; k++)
        snprintf(path, n, "%s/%.*s_%d%s", dir, stem, named, k, dot ? dot : "");
    FILE *f = fopen(path, "wb");
    if (f) setvbuf(f, NULL, _IONBF, 0);
    return f;
}

// ---------------- мониторинг ----------------

static FILE    *s_mf;          // файл текущей сессии; NULL — не открыт
static bool     s_armed;       // сессия идёт — файл откроется на первом интервале
static uint16_t s_iv;
static uint64_t s_counts, s_dur;
static char     s_mpath[224];

static int mon_total(char *line, size_t cap, uint64_t counts, uint64_t dur);

static void mon_close_file(void)
{
    if (!s_mf) return;
    if (s_dur > 0) {   // итог сессии — как в exportCsv() веба
        char line[160];
        int n = mon_total(line, sizeof line, s_counts, s_dur);
        fwrite(line, 1, n, s_mf);
    }
    fclose(s_mf);
    s_mf = NULL;
    ESP_LOGI(TAG, "мониторинг: файл закрыт %s", s_mpath);
}

// Имя файла сессии: по часам платы, без часов (ни DS3231, ни браузера) — номер по порядку.
static void mon_base_name(time_t now, char *base, size_t n)
{
    if (now) {
        char stamp[48];
        fmt_stamp(now, stamp, sizeof stamp);
        snprintf(base, n, "cps_monitor_%s.csv", stamp);
        return;
    }
    struct stat st;
    char probe[224];
    int k = 0;
    do {
        snprintf(base, n, "cps_monitor_%04d.csv", k);
        snprintf(probe, sizeof probe, "%s/%s", DIR_MON, base);
    } while (stat(probe, &st) == 0 && ++k < 10000);
}

// Шапка CSV — колонки как «Выгрузить CSV» веба; what — «started …» / «saved …».
static int mon_header(char *hdr, size_t cap, time_t now, const char *what, uint16_t iv, const char *src)
{
    char iso[48], tz[16];
    tz_label(tz, sizeof tz);
    if (now) fmt_iso(now, iso, sizeof iso);
    else     strcpy(iso, "time not set");
    return snprintf(hdr, cap,
        "\xEF\xBB\xBF# AtomSpectra Gateway \xE2\x80\x94 CPS monitoring\r\n"
        "# %s %s  tz %s  interval %u s  (%s)\r\n"
        "timestamp;dev_time_start_s;dev_time_end_s;duration_s;counts;mean_cps;sigma_cps;rel_err_pct\r\n",
        what, iso, tz, (unsigned)iv, src);
}

// Строка интервала — closeBucket()/exportCsv() веба.
static int mon_row(char *line, size_t cap, uint32_t dev_start, uint32_t dev_end, uint32_t dur,
                   uint32_t counts, uint32_t up_end)
{
    char ts[48] = "";
    uint32_t w = monitor_wall_of(up_end);
    if (w) fmt_iso((time_t)w + (time_t)boot_config_tz_min() * 60, ts, sizeof ts);
    double cps = (double)counts / dur, sig = sqrt((double)counts) / dur;
    return counts
        ? snprintf(line, cap, "%s;%" PRIu32 ";%" PRIu32 ";%" PRIu32 ";%" PRIu32 ";%.6f;%.6f;%.4f\r\n",
                   ts, dev_start, dev_end, dur, counts, cps, sig, 100.0 / sqrt((double)counts))
        : snprintf(line, cap, "%s;%" PRIu32 ";%" PRIu32 ";%" PRIu32 ";%" PRIu32 ";%.6f;%.6f;\r\n",
                   ts, dev_start, dev_end, dur, counts, cps, sig);
}

static int mon_total(char *line, size_t cap, uint64_t counts, uint64_t dur)   // итог сессии
{
    double g = (double)counts / dur, gs = sqrt((double)counts) / dur;
    return counts
        ? snprintf(line, cap, "# session_mean_cps;%.6f;sigma;%.6f;rel_pct;%.4f\r\n",
                   g, gs, 100.0 / sqrt((double)counts))
        : snprintf(line, cap, "# session_mean_cps;%.6f;sigma;%.6f;rel_pct;\r\n", g, gs);
}

static bool mon_open_file(void)
{
    char base[96];
    time_t now = local_now();
    mon_base_name(now, base, sizeof base);
    s_mf = open_new(DIR_MON, base, s_mpath, sizeof s_mpath);
    if (!s_mf) {
        ESP_LOGW(TAG, "мониторинг: не создаётся %s", s_mpath);
        return false;
    }
    char hdr[320];
    int n = mon_header(hdr, sizeof hdr, now, "started", s_iv, "board SD log");
    if (fwrite(hdr, 1, n, s_mf) != (size_t)n) {
        fclose(s_mf);
        s_mf = NULL;
        return false;
    }
    ESP_LOGI(TAG, "мониторинг: пишу в %s", s_mpath);
    return true;
}

void sd_log_mon_new_session(bool running, uint16_t interval_s)
{
    mon_close_file();
    s_armed  = running && sd_card_present() && boot_config_mon_autosave();   // #MON-8
    s_iv     = interval_s;
    s_counts = s_dur = 0;
}

void sd_log_mon_interval(uint32_t dev_start, uint32_t dev_end, uint32_t dur,
                         uint32_t counts, uint32_t up_end)
{
    if (!s_armed || dur == 0) return;
    if (!s_mf && !mon_open_file()) {
        s_armed = false;   // карта не пишется — не долбить её каждый интервал
        return;
    }
    char line[160];
    int n = mon_row(line, sizeof line, dev_start, dev_end, dur, counts, up_end);
    // Каждая строка сразу на карту (fsync): выключили питание — теряется
    // максимум текущий интервал, а не вся сессия.
    if (fwrite(line, 1, n, s_mf) != (size_t)n || fsync(fileno(s_mf)) != 0) {
        ESP_LOGW(TAG, "мониторинг: запись в %s не удалась — запись на карту остановлена", s_mpath);
        fclose(s_mf);
        s_mf = NULL;
        s_armed = false;
        return;
    }
    s_counts += counts;
    s_dur    += dur;
}

void sd_log_mon_stop(void)
{
    s_armed = false;
    mon_close_file();
}

bool sd_log_mon_active(void) { return s_mf != NULL || s_armed; }

bool sd_log_mon_file(char *out, size_t cap)
{
    if (!s_mf || !cap) return false;
    const char *b = strrchr(s_mpath, '/');
    snprintf(out, cap, "%s", b ? b + 1 : s_mpath);
    return true;
}

// ---------------- #MON-8: сохранение сессии по кнопке ----------------

#define SAVE_SMP 512   // сэмплов за один снимок кольца (7 КБ PSRAM)

static bool s_saving;   // веб и экран могут нажать одновременно

static int mon_save(char *name, size_t cap, int *rows_out)
{
    monitor_sample_t *smp = heap_caps_malloc(SAVE_SMP * sizeof(*smp), MALLOC_CAP_SPIRAM);
    char *ob = heap_caps_malloc(CHUNK, MALLOC_CAP_SPIRAM);
    char path[224] = "";
    FILE *f = NULL;
    int rc = SDLOG_SAVE_OK, rows = 0;
    size_t ol = 0;
    if (!smp || !ob) { rc = SDLOG_SAVE_IO; goto out; }

    uint32_t seq = 0, lim = 0, ep0 = 0, rev0 = 0, iv = 1;
    bool first_pass = true;
    // Ведро интервала — pushBase()/closeBucket() веба и mon_push() экрана.
    bool b_open = false;
    uint32_t b_start = 0, b_end = 0, b_dur = 0, b_cnt = 0, b_up = 0;
    uint64_t t_cnt = 0, t_dur = 0;
    for (int hop = 0; hop < 200; hop++) {   // 6 ч по 1 с — ~43 снимка
        uint32_t first, next, epoch;
        monitor_session_t ss;
        size_t n = monitor_copy_since(first_pass ? UINT32_MAX - 1 : seq, smp, first_pass ? 0 : SAVE_SMP,
                                      &first, &next, &epoch, &ss);
        if (first_pass) {   // сессия и её граница — на момент нажатия
            if (!ss.start_seq) { rc = SDLOG_SAVE_EMPTY; goto out; }
            first_pass = false;
            ep0 = epoch; rev0 = ss.rev;
            iv  = ss.interval_s ? ss.interval_s : 1;
            lim = ss.running ? next - 1 : ss.stop_seq;
            seq = ss.start_seq - 1;
            continue;
        }
        if (epoch != ep0 || ss.rev != rev0) { rc = SDLOG_SAVE_CHANGED; goto out; }   // Старт/Очистить во время записи
        bool done = n == 0;
        for (size_t i = 0; i < n && !done; i++) {
            uint32_t s = first + (uint32_t)i;
            if (s <= seq) continue;
            if (s > lim) { done = true; break; }
            seq = s;
            const monitor_sample_t *m = &smp[i];
            if (!b_open) { b_open = true; b_start = m->end_sec - m->dur; b_dur = b_cnt = 0; }
            b_dur += m->dur; b_cnt += m->counts; b_end = m->end_sec; b_up = m->up_sec;
            if (b_dur < iv) continue;
            b_open = false;   // закрыт интервал -> строка
            if (!f) {
                char base[96];
                time_t now = local_now();
                mon_base_name(now, base, sizeof base);
                f = open_new(DIR_MON, base, path, sizeof path);
                if (!f) { rc = SDLOG_SAVE_IO; goto out; }
                ol = (size_t)mon_header(ob, CHUNK, now, "saved", (uint16_t)iv, "board SD save");
            }
            ol += (size_t)mon_row(ob + ol, CHUNK - ol, b_start, b_end, b_dur, b_cnt, b_up);
            t_cnt += b_cnt; t_dur += b_dur; rows++;
            if (ol > CHUNK - 200) {
                if (fwrite(ob, 1, ol, f) != ol) { rc = SDLOG_SAVE_IO; goto out; }
                ol = 0;
            }
        }
        if (done || seq >= lim || seq + 1 >= next) break;
    }
    // Недобранное ведро в файл не идёт — как в вебе (идущая сессия / Стоп).
    if (!rows) { rc = SDLOG_SAVE_EMPTY; goto out; }
    ol += (size_t)mon_total(ob + ol, CHUNK - ol, t_cnt, t_dur);
    if (fwrite(ob, 1, ol, f) != ol || fsync(fileno(f)) != 0) rc = SDLOG_SAVE_IO;
out:
    if (f) {
        fclose(f);
        if (rc != SDLOG_SAVE_OK) unlink(path);   // недописанный файл не оставлять
    }
    if (rc == SDLOG_SAVE_OK) {
        const char *b = strrchr(path, '/');
        if (name && cap) snprintf(name, cap, "%s", b ? b + 1 : path);
        if (rows_out) *rows_out = rows;
        ESP_LOGI(TAG, "мониторинг: сохранено %d интервалов в %s", rows, path);
    } else {
        ESP_LOGW(TAG, "мониторинг: сохранение не удалось (%d)", rc);
    }
    heap_caps_free(smp);
    heap_caps_free(ob);
    return rc;
}

int sd_log_mon_save(char *name, size_t cap, int *rows)
{
    if (!sd_card_present()) return SDLOG_SAVE_NOSD;
    if (__atomic_test_and_set(&s_saving, __ATOMIC_ACQUIRE)) return SDLOG_SAVE_BUSY;
    int rc = mon_save(name, cap, rows);
    __atomic_clear(&s_saving, __ATOMIC_RELEASE);
    return rc;
}
