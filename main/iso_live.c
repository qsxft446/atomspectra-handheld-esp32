// #ISO-1: см. iso_live.h. Задача iso: ядро 1, приоритет 1 (ниже LVGL и сети), стек в PSRAM —
// снимок спектра и расчёт не трогают флеш/NVS (правило #ACAL-3). Буферы — PSRAM: снимок
// спектра ~33 КБ, рабочий isoid ~100 КБ.
#include "iso_live.h"
#include "atomspectra.h"
#include <string.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "iso";

// Яркие, без бирюзового (линия спектра) и без серого (K-40).
const uint32_t ISO_PAL[ISO_PAL_N] = { 0xFF3030, 0xFFE600, 0x3DA5FF, 0xFF40E0, 0x50FF30, 0xFF9000, 0xB070FF, 0xFFFFFF };

static bool is_gray(const isoid_result_t *r, int i) { return !strcmp(isoid_name(r->f[i].src), "K-40"); }

static SemaphoreHandle_t s_lock;
static isoid_result_t   *s_res;   // PSRAM (внутренняя RAM дефицитна)
static uint32_t          s_seq, s_ms;
static isoid_tune_t      s_tune;   // #ISO-3: самонастройка под прибор (только задача iso)

uint32_t iso_live_color(const isoid_result_t *r, int i)
{
    if (is_gray(r, i)) return ISO_COL_NATURAL;
    int k = 0;
    for (int j = 0; j < i; j++) k += !is_gray(r, j);
    return ISO_PAL[k % ISO_PAL_N];
}

uint32_t iso_live_get(isoid_result_t *out)
{
    if (!s_lock) { memset(out, 0, sizeof *out); return 0; }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = *s_res;
    uint32_t q = s_seq;
    xSemaphoreGive(s_lock);
    return q;
}

uint32_t iso_live_last_ms(void) { return s_ms; }

static void publish(const isoid_result_t *r, bool valid)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (valid) *s_res = *r;
    else memset(s_res, 0, sizeof *s_res);
    s_seq = valid ? s_seq + 1 : 0;
    xSemaphoreGive(s_lock);
}

static void iso_task(void *arg)
{
    (void)arg;
    spectrum_data_t *snap = heap_caps_malloc(sizeof *snap, MALLOC_CAP_SPIRAM);
    void *ws = heap_caps_malloc(isoid_ws_size(SPECTRUM_CHANNELS), MALLOC_CAP_SPIRAM);
    isoid_result_t *r = heap_caps_malloc(sizeof *r, MALLOC_CAP_SPIRAM);
    if (!snap || !ws || !r) {
        ESP_LOGW(TAG, "no PSRAM for isotope identification - disabled");
        vTaskDelete(NULL);
        return;
    }
    uint32_t last_counts = UINT32_MAX, seen = 0;
    isoid_tune_default(&s_tune);
    TickType_t last_run = 0;
    for (;;) {
        // #ISO-2 (запрос 2026-10-18): очистка спектра (кнопка, свайп, веб, ПК через мост, старт
        // автокалибровки) — найденное обнуляется сразу: счёт сверяется каждые 0.5 с, полный разбор —
        // раз в ISO_PERIOD_MS.
        vTaskDelay(pdMS_TO_TICKS(500));
        uint32_t cnt = 0, tsec = 0;
        spectrum_get_totals(&cnt, &tsec);
        if (cnt < seen) {
            publish(NULL, false);
            isoid_tune_default(&s_tune);   // #ISO-3: новый спектр — настройка заново
            last_counts = UINT32_MAX;
            last_run = 0;
            ESP_LOGI(TAG, "spectrum cleared - identification reset");
        }
        seen = cnt;
        if (last_run && xTaskGetTickCount() - last_run < pdMS_TO_TICKS(ISO_PERIOD_MS)) continue;
        last_run = xTaskGetTickCount();
        if (!last_run) last_run = 1;
        if (!spectrum_get_snapshot(snap) || !snap->calib_valid) {
            if (last_counts != UINT32_MAX) publish(NULL, false);
            last_counts = UINT32_MAX;
            continue;
        }
        if (snap->total_counts == last_counts) continue;   // спектр не изменился — результат тот же
        last_counts = snap->total_counts;
        int64_t t0 = esp_timer_get_time();
        isoid_tune_t tn;
        int n = isoid_run(snap->bins, SPECTRUM_CHANNELS, snap->calibration, snap->calib_order, ws, &s_tune, &tn, r);
        // #ISO-3: оценки по этому спектру — плавно (половина шага за прогон)
        if (tn.n_w) s_tune.kw += 0.5f * (tn.kw - s_tune.kw);
        if (tn.n_g) { s_tune.ga += 0.5f * (tn.ga - s_tune.ga); s_tune.gb += 0.5f * (tn.gb - s_tune.gb); }
        s_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
        publish(r, n >= 0);
    }
}

void iso_live_init(void)
{
    s_res = heap_caps_calloc(1, sizeof *s_res, MALLOC_CAP_SPIRAM);
    if (!s_res) return;
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return;
    if (xTaskCreatePinnedToCoreWithCaps(iso_task, "iso", 6144, NULL, 1, NULL, 1,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS)
        ESP_LOGW(TAG, "task create failed - isotope identification disabled");
}
