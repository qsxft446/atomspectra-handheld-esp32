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

static void mon_close_file(void)
{
    if (!s_mf) return;
    if (s_dur > 0) {   // итог сессии — как в exportCsv() веба
        double g = (double)s_counts / s_dur, gs = sqrt((double)s_counts) / s_dur;
        char line[160];
        int n = s_counts
            ? snprintf(line, sizeof line, "# session_mean_cps;%.6f;sigma;%.6f;rel_pct;%.4f\r\n",
                       g, gs, 100.0 / sqrt((double)s_counts))
            : snprintf(line, sizeof line, "# session_mean_cps;%.6f;sigma;%.6f;rel_pct;\r\n", g, gs);
        fwrite(line, 1, n, s_mf);
    }
    fclose(s_mf);
    s_mf = NULL;
    ESP_LOGI(TAG, "мониторинг: файл закрыт %s", s_mpath);
}

static bool mon_open_file(void)
{
    char base[96], stamp[48], iso[48], tz[16];
    time_t now = local_now();
    if (now) {
        fmt_stamp(now, stamp, sizeof stamp);
        snprintf(base, sizeof base, "cps_monitor_%s.csv", stamp);
    } else {
        // Часов нет (ни DS3231, ни браузера) — номер по порядку.
        struct stat st;
        char probe[224];
        int k = 0;
        do {
            snprintf(base, sizeof base, "cps_monitor_%04d.csv", k);
            snprintf(probe, sizeof probe, "%s/%s", DIR_MON, base);
        } while (stat(probe, &st) == 0 && ++k < 10000);
    }
    s_mf = open_new(DIR_MON, base, s_mpath, sizeof s_mpath);
    if (!s_mf) {
        ESP_LOGW(TAG, "мониторинг: не создаётся %s", s_mpath);
        return false;
    }
    tz_label(tz, sizeof tz);
    if (now) fmt_iso(now, iso, sizeof iso);
    else     strcpy(iso, "time not set");
    char hdr[320];
    int n = snprintf(hdr, sizeof hdr,
        "\xEF\xBB\xBF# AtomSpectra Gateway \xE2\x80\x94 CPS monitoring\r\n"
        "# started %s  tz %s  interval %u s  (board SD log)\r\n"
        "timestamp;dev_time_start_s;dev_time_end_s;duration_s;counts;mean_cps;sigma_cps;rel_err_pct\r\n",
        iso, tz, (unsigned)s_iv);
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
    s_armed  = running && sd_card_present();
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
    char ts[48] = "";
    uint32_t w = monitor_wall_of(up_end);
    if (w) fmt_iso((time_t)w + (time_t)boot_config_tz_min() * 60, ts, sizeof ts);
    double cps = (double)counts / dur, sig = sqrt((double)counts) / dur;
    char line[160];
    int n = counts
        ? snprintf(line, sizeof line, "%s;%" PRIu32 ";%" PRIu32 ";%" PRIu32 ";%" PRIu32 ";%.6f;%.6f;%.4f\r\n",
                   ts, dev_start, dev_end, dur, counts, cps, sig, 100.0 / sqrt((double)counts))
        : snprintf(line, sizeof line, "%s;%" PRIu32 ";%" PRIu32 ";%" PRIu32 ";%" PRIu32 ";%.6f;%.6f;\r\n",
                   ts, dev_start, dev_end, dur, counts, cps, sig);
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
