// #HANG-1: детектор зависания (см. hang_watch.h).
#include "hang_watch.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "atomspectra.h"
#include "spectrogram.h"
#include "tft_display.h"
#include "battery.h"
#include "ota_update.h"
#include "sd_card.h"

static const char *TAG = "hang";

#define HANG_STALL_S   45          // молчит дольше — зависание (циклы задач просыпаются раз в 0.5-5 с,
                                   // самое долгое штатное — подключение WiFi с экрана, до 20 с)
#define HANG_MAGIC     0x4A4E4148u
#define REP_SZ         1800

static RTC_NOINIT_ATTR uint32_t s_rep_magic;
static RTC_NOINIT_ATTR char     s_rep[REP_SZ];

static const char *const HB_NAME[HB_N] = { "lvgl", "tft", "wf", "wf_fs", "httpd", "main" };
static volatile int64_t s_hb[HB_N];         // время последней отметки, мкс; 0 — задача ещё не отмечалась
static volatile bool    s_restart;
static int              s_len;

void hang_watch_beat(hang_hb_t id)
{
    if (id < HB_N) s_hb[id] = esp_timer_get_time();
}

static void rep(const char *fmt, ...)
{
    if (s_len >= REP_SZ - 1) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(s_rep + s_len, REP_SZ - s_len, fmt, ap);
    va_end(ap);
    if (n > 0) s_len += n;
    if (s_len > REP_SZ - 1) s_len = REP_SZ - 1;
}

static const char *task_name(void *h)
{
    return h ? pcTaskGetName((TaskHandle_t)h) : "-";
}

static void build_report(int64_t now)
{
    s_len = 0;
    rep("uptime %lld s, stalled > %d s:", (long long)(now / 1000000), HANG_STALL_S);
    for (int i = 0; i < HB_N; i++)
        if (s_hb[i] && now - s_hb[i] > HANG_STALL_S * 1000000LL) rep(" %s", HB_NAME[i]);
    rep("\nbeat age, s:");
    for (int i = 0; i < HB_N; i++)
        rep(" %s %lld", HB_NAME[i], s_hb[i] ? (long long)((now - s_hb[i]) / 1000000) : -1LL);
    int64_t lv_since = 0;
    void *lv = tft_display_lock_owner(&lv_since);
    void *ring = NULL, *fs = NULL;
    spectrogram_lock_holders(&ring, &fs);
    rep("\nlocks: lvgl/spi %s", task_name(lv));
    if (lv) rep(" (%lld s)", (long long)((now - lv_since) / 1000000));
    rep(", spectrum %s, wf ring %s, wf files %s", task_name(spectrum_lock_holder()), task_name(ring), task_name(fs));
    rep("; lvgl flushing %d", tft_display_flushing());
    rep("\nmem: int %u/%u min %u, dma %u/%u",
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
    battery_status_t b;
    battery_get_status(&b);
    if (b.present)
        rep("\npower: bat %.2f V, sys %.2f V, vbus %.2f V, chg %d, faults boost %lu chrg %lu",
            (double)b.voltage, (double)b.vsys_voltage, (double)b.vbus_voltage, b.charging,
            (unsigned long)b.boost_faults, (unsigned long)b.chrg_faults);
    void *tt = NULL;
    int64_t tsince = 0;
    uint32_t terr = 0;
    int tst = sd_card_txn_state(&tt, &tsince, &terr);
    rep("\nsd %d, usb %d; sd txn: %s", sd_card_present(), usb_host_cdc_is_connected(),
        tst == 1 ? "waiting spi lock" : tst == 2 ? "in transfer" : "none");
    if (tst) rep(" by %s for %lld s", task_name(tt), (long long)((now - tsince) / 1000000));
    rep(", txn errors %lu", (unsigned long)terr);
    // Задачи: имя, состояние (R бежит, r готова, B ждёт, S приостановлена), приоритет, запас стека
    UBaseType_t n = uxTaskGetNumberOfTasks();
    TaskStatus_t *ts = heap_caps_malloc(n * sizeof *ts, MALLOC_CAP_SPIRAM);
    if (ts) {
        n = uxTaskGetSystemState(ts, n, NULL);
        rep("\ntasks:");
        for (UBaseType_t i = 0; i < n; i++) {
            static const char ST[] = "RrBSD?";
            int st = ts[i].eCurrentState <= eDeleted ? ts[i].eCurrentState : 5;
            rep(" %s:%c%u/%u", ts[i].pcTaskName, ST[st], (unsigned)ts[i].uxCurrentPriority,
                (unsigned)ts[i].usStackHighWaterMark);
        }
        free(ts);
    }
}

// Перезагрузка — из задачи esp_timer (стек во внутренней RAM): у hang_task стек в PSRAM.
static void restart_cb(void *arg)
{
    (void)arg;
    if (s_restart) esp_restart();
}

// #RAM-2: минимальный остаток стека каждой задачи с её старта — в лог через 2 мин и раз в 10 мин
// (W, если у кого-то меньше 768 Б): проверка урезанных стеков.
static void stack_report(void)
{
    UBaseType_t n = uxTaskGetNumberOfTasks();
    TaskStatus_t *ts = heap_caps_malloc(n * sizeof *ts, MALLOC_CAP_SPIRAM);
    if (!ts) return;
    n = uxTaskGetSystemState(ts, n, NULL);
    char line[900];
    int len = 0, low = 0;
    for (UBaseType_t i = 0; i < n && len < (int)sizeof line - 32; i++) {
        len += snprintf(line + len, sizeof line - len, " %s %u", ts[i].pcTaskName, (unsigned)ts[i].usStackHighWaterMark);
        // системные задачи ESP-IDF (IDLE, ipc) штатно живут с остатком 0.5-0.7 КБ
        bool sys = !strncmp(ts[i].pcTaskName, "IDLE", 4) || !strncmp(ts[i].pcTaskName, "ipc", 3);
        low |= !sys && ts[i].usStackHighWaterMark < 768;
    }
    free(ts);
    if (low) ESP_LOGW(TAG, "#RAM-2 stack free min, B:%s", line);
    else     ESP_LOGI(TAG, "#RAM-2 stack free min, B:%s", line);
}

static void hang_task(void *arg)
{
    (void)arg;
    int tick = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        if (++tick == 60 || tick % 300 == 0) stack_report();
        web_server_hb_kick();   // отметка httpd — из его же задачи (httpd_queue_work)
        if (ota_update_busy()) {   // OTA держит задачу httpd ~60 с — это не зависание
            s_hb[HB_HTTPD] = esp_timer_get_time();
            continue;
        }
        int64_t now = esp_timer_get_time();
        bool stall = false;
        for (int i = 0; i < HB_N; i++) stall |= s_hb[i] && now - s_hb[i] > HANG_STALL_S * 1000000LL;
        if (!stall) continue;
        build_report(now);
        s_rep_magic = HANG_MAGIC;
        ESP_LOGE(TAG, "#HANG-1: hang detected, restarting\n%s", s_rep);
        vTaskDelay(pdMS_TO_TICKS(300));   // строка успевает в UART
        s_restart = true;
        vTaskDelete(NULL);
    }
}

#define HANG_TEST 0   // 1 — проверка детектора: через 90 с после старта замок экрана/SPI занят навсегда
#if HANG_TEST
static void hang_test_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(90000));
    ESP_LOGW(TAG, "HANG_TEST: taking LVGL/SPI lock forever");
    tft_display_spi_lock();
    vTaskDelay(portMAX_DELAY);
}
#endif

void hang_watch_start(void)
{
#if HANG_TEST
    xTaskCreatePinnedToCoreWithCaps(hang_test_task, "hangtest", 3072, NULL, 3, NULL, 1, MALLOC_CAP_SPIRAM);
#endif
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < HB_N; i++) if (s_hb[i]) s_hb[i] = now;   // отсчёт — от конца старта
    const esp_timer_create_args_t a = { .callback = restart_cb, .name = "hang_rst" };
    esp_timer_handle_t t;
    if (esp_timer_create(&a, &t) != ESP_OK || esp_timer_start_periodic(t, 500000) != ESP_OK) {
        ESP_LOGW(TAG, "#HANG-1: no timer, detector off");
        return;
    }
    // Стек в PSRAM: задача не трогает флеш/NVS (правило #ACAL-3); перезагрузку делает restart_cb.
    if (xTaskCreatePinnedToCoreWithCaps(hang_task, "hang", 4096, NULL, 12, NULL, 1, MALLOC_CAP_SPIRAM) != pdPASS)
        ESP_LOGW(TAG, "#HANG-1: no task, detector off");
}

void hang_watch_boot_report(void)
{
    if (s_rep_magic != HANG_MAGIC) return;
    s_rep[REP_SZ - 1] = '\0';
    ESP_LOGE(TAG, "#HANG-1: previous run hung and was restarted by the detector:");
    char line[200];
    const char *p = s_rep;
    while (*p) {
        const char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        // длинная строка задач — кусками
        for (size_t o = 0; o < n; o += sizeof line - 1) {
            size_t k = n - o < sizeof line - 1 ? n - o : sizeof line - 1;
            memcpy(line, p + o, k);
            line[k] = '\0';
            ESP_LOGE(TAG, "  %s", line);
        }
        p += n + (e ? 1 : 0);
    }
}

void hang_watch_save_sd(void)
{
    if (s_rep_magic != HANG_MAGIC) return;
    s_rep_magic = 0;
    if (!sd_card_present()) return;
    FILE *f = fopen("/sdcard/hang.log", "a");
    if (!f) return;
    time_t t = time(NULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    fprintf(f, "=== boot %04d-%02d-%02d %02d:%02d:%02d UTC, previous run hung ===\n%s\n\n",
            tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, s_rep);
    fclose(f);
    ESP_LOGW(TAG, "#HANG-1: report appended to /sdcard/hang.log");
}
