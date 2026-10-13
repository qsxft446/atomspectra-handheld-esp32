// #ALM-1 (запрос 2026-10-13): сигнализация превышения порогов CPS.
//
// Спикер — обычный PC-спикер/пищалка на GPIO21 (магнитный — через транзистор):
// LEDC, меандр ALM_TONE_HZ, 50 %. Покой — выход в 0.
// Уровень — старший превышенный порог по CPS прибора за 1 с (то же число, что
// крупно на экране; выбор пользователя — без усреднения). Тревога только пока идёт
// набор: время набора росло за ALM_FRESH_MS — иначе последний CPS остался бы в
// памяти и пищал бы вечно после Стопа/отключения прибора.
// Ритм — main/alarm_seq.h (#ALM-2: «-» у пользователя — пауза): N писков и пауза
// 1 с, на 4-м уровне частое «та-та-та».
//
// #ALM-3 (аудит 2026-10-14, «та-та-та неровное»): ритм раньше отсчитывала задача
// vTaskDelay-ями: тик FreeRTOS 10 мс (CONFIG_FREERTOS_HZ=100) давал каждому
// интервалу 60 мс ±10 мс, а сама задача (приоритет 2, ядро 1 вместе с WiFi, tcpip
// и LVGL, который раз в секунду рисует кадр 300-450 мс) стояла, пока её вытесняли.
// Теперь ритм — периодический esp_timer 10 мс (задача esp_timer, приоритет 22,
// без дрейфа): тон включается/выключается из его колбэка по alarm_seq_tick().
// Задача alarm только считает уровень раз в 50 мс. Колбэк замеряет фактические
// длительности писков/пауз (cps_alarm_diag, /api/alarm "diag") — проверка ровности.
// Экран мигает числом CPS красным по cps_alarm_level() (tft_display.c).
#include "cps_alarm.h"
#include "alarm_seq.h"
#include "atomspectra.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include <string.h>

static const char *TAG = "alarm";

#define ALM_GPIO        21
#define ALM_TONE_HZ     2000
#define ALM_DUTY_RES    LEDC_TIMER_10_BIT
#define ALM_DUTY_ON     512             // 50 % при 10 битах
#define ALM_TIMER       LEDC_TIMER_0
#define ALM_CHANNEL     LEDC_CHANNEL_0
#define ALM_MODE        LEDC_LOW_SPEED_MODE
#define ALM_POLL_MS     50              // опрос CPS задачей alarm
#define ALM_FRESH_MS    2500            // как ACQ_HOLD_MS экрана: свип раз в 1 с
#define ALM_MAGIC       0x314D4C41u     // "ALM1"

typedef struct { uint32_t magic; uint8_t enabled; uint8_t pad[3]; uint32_t th[CPS_ALARM_LEVELS]; } alm_nvs_t;

static portMUX_TYPE    s_mux = portMUX_INITIALIZER_UNLOCKED;   // s_cfg и s_diag
static cps_alarm_cfg_t s_cfg = { .enabled = false, .th = { 100, 200, 400, 800 } };
static volatile int    s_level;
static bool            s_ledc_ok;
static uint32_t        s_last_t;         // время набора на прошлом опросе (задача alarm)
static int64_t         s_t_adv_us;       // когда оно последний раз росло (0 — не росло)

// Только колбэк esp_timer.
static alarm_seq_t     s_seq;
static bool            s_tone_on;
static int64_t         s_edge_us;        // время последнего переключения (0 — не было)
static cps_alarm_diag_t s_diag;

static void tone(bool on)
{
    if (!s_ledc_ok) return;
    ledc_set_duty(ALM_MODE, ALM_CHANNEL, on ? ALM_DUTY_ON : 0);
    ledc_update_duty(ALM_MODE, ALM_CHANNEL);
}

static void diag_reset_locked(int lv)
{
    memset(&s_diag, 0, sizeof s_diag);
    s_diag.level = lv;
    s_diag.on_min_us = s_diag.gap_min_us = s_diag.pause_min_us = INT32_MAX;
}

// Отрезок [s_edge_us, now) с тоном was_on закончился — в статистику.
static void diag_edge(bool was_on, int64_t now)
{
    if (!s_edge_us) return;
    int32_t d = (int32_t)(now - s_edge_us);
    portENTER_CRITICAL(&s_mux);
    int32_t *mn, *mx;
    if (was_on) {
        mn = &s_diag.on_min_us; mx = &s_diag.on_max_us; s_diag.beeps++;
    } else if (d < (ALARM_PAUSE_MS / 2) * 1000) {
        mn = &s_diag.gap_min_us; mx = &s_diag.gap_max_us; s_diag.gaps++;
    } else {
        mn = &s_diag.pause_min_us; mx = &s_diag.pause_max_us; s_diag.pauses++;
    }
    if (d < *mn) *mn = d;
    if (d > *mx) *mx = d;
    portEXIT_CRITICAL(&s_mux);
}

static void seq_timer_cb(void *arg)
{
    (void)arg;
    int lv = s_level;
    if (lv != s_diag.level) {   // новый уровень — новая статистика
        portENTER_CRITICAL(&s_mux);
        diag_reset_locked(lv);
        portEXIT_CRITICAL(&s_mux);
        s_edge_us = 0;
    }
    bool on = alarm_seq_tick(&s_seq, lv);
    if (on != s_tone_on) {
        int64_t now = esp_timer_get_time();
        tone(on);
        diag_edge(s_tone_on, now);
        s_edge_us = now;
        s_tone_on = on;
    }
}

static int eval_level(void)
{
    uint32_t cps = 0, t = 0;
    bool valid = spectrum_get_cps(&cps, &t);
    int64_t now = esp_timer_get_time();
    if (valid && t != s_last_t) {
        if (s_last_t) s_t_adv_us = now;   // первый опрос — только опорная точка
        s_last_t = t;
    }
    bool fresh = valid && s_t_adv_us && now - s_t_adv_us < (int64_t)ALM_FRESH_MS * 1000;
    cps_alarm_cfg_t c;
    portENTER_CRITICAL(&s_mux);
    c = s_cfg;
    portEXIT_CRITICAL(&s_mux);
    int lv = c.enabled && fresh ? alarm_level_of(cps, c.th, CPS_ALARM_LEVELS) : 0;
    s_level = lv;
    return lv;
}

static void alarm_task(void *arg)
{
    (void)arg;
    for (;;) {
        eval_level();
        vTaskDelay(pdMS_TO_TICKS(ALM_POLL_MS));
    }
}

static bool cfg_valid(const cps_alarm_cfg_t *c)
{
    if (c->th[0] < 1) return false;
    for (int i = 0; i < CPS_ALARM_LEVELS; i++) {
        if (c->th[i] > CPS_ALARM_TH_MAX) return false;
        if (i && c->th[i] <= c->th[i - 1]) return false;
    }
    return true;
}

void cps_alarm_get_config(cps_alarm_cfg_t *out)
{
    portENTER_CRITICAL(&s_mux);
    *out = s_cfg;
    portEXIT_CRITICAL(&s_mux);
}

int cps_alarm_set_config(const cps_alarm_cfg_t *c)
{
    if (!cfg_valid(c)) return -1;
    portENTER_CRITICAL(&s_mux);
    s_cfg = *c;
    portEXIT_CRITICAL(&s_mux);
    alm_nvs_t n = { .magic = ALM_MAGIC, .enabled = c->enabled };
    memcpy(n.th, c->th, sizeof n.th);
    nvs_handle_t h;
    esp_err_t err = nvs_open("alarm", NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, "cfg", &n, sizeof n);
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "config: %s, thresholds %lu/%lu/%lu/%lu CPS, NVS %s", c->enabled ? "on" : "off",
             (unsigned long)c->th[0], (unsigned long)c->th[1], (unsigned long)c->th[2], (unsigned long)c->th[3],
             esp_err_to_name(err));
    return err == ESP_OK ? 0 : -2;
}

int cps_alarm_level(void) { return s_level; }

void cps_alarm_diag(cps_alarm_diag_t *out)
{
    portENTER_CRITICAL(&s_mux);
    *out = s_diag;
    portEXIT_CRITICAL(&s_mux);
}

void cps_alarm_init(void)
{
    nvs_handle_t h;
    alm_nvs_t n;
    size_t sz = sizeof n;
    if (nvs_open("alarm", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_blob(h, "cfg", &n, &sz) == ESP_OK && sz == sizeof n && n.magic == ALM_MAGIC) {
            cps_alarm_cfg_t c = { .enabled = n.enabled != 0 };
            memcpy(c.th, n.th, sizeof c.th);
            if (cfg_valid(&c)) s_cfg = c;
        }
        nvs_close(h);
    }
    diag_reset_locked(0);

    ledc_timer_config_t tc = {
        .speed_mode = ALM_MODE, .duty_resolution = ALM_DUTY_RES, .timer_num = ALM_TIMER,
        .freq_hz = ALM_TONE_HZ, .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_channel_config_t cc = {
        .gpio_num = ALM_GPIO, .speed_mode = ALM_MODE, .channel = ALM_CHANNEL,
        .timer_sel = ALM_TIMER, .duty = 0, .hpoint = 0,
    };
    esp_err_t err = ledc_timer_config(&tc);
    if (err == ESP_OK) err = ledc_channel_config(&cc);
    if (err != ESP_OK) {
        // Как прочая периферия проекта: без спикера мигание на экране всё равно работает.
        ESP_LOGW(TAG, "LEDC on GPIO%d failed: %s — alarm without sound", ALM_GPIO, esp_err_to_name(err));
    } else {
        s_ledc_ok = true;
    }
    // Стек в PSRAM (#RAM-1): задача во флеш не пишет (NVS пишет вызывающий set_config).
    if (xTaskCreatePinnedToCoreWithCaps(alarm_task, "alarm", 3072, NULL, 2, NULL, 1,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS)
        ESP_LOGW(TAG, "task create failed — alarm disabled");
    // #ALM-3: ритм — esp_timer, а не задача (точность и независимость от загрузки ядер).
    const esp_timer_create_args_t ta = { .callback = seq_timer_cb, .name = "alarm_seq" };
    esp_timer_handle_t th;
    if (esp_timer_create(&ta, &th) != ESP_OK || esp_timer_start_periodic(th, ALARM_SEQ_TICK_MS * 1000) != ESP_OK)
        ESP_LOGW(TAG, "esp_timer failed — alarm without sound");
    ESP_LOGI(TAG, "GPIO%d, %d Hz, %s, thresholds %lu/%lu/%lu/%lu CPS", ALM_GPIO, ALM_TONE_HZ,
             s_cfg.enabled ? "on" : "off", (unsigned long)s_cfg.th[0], (unsigned long)s_cfg.th[1],
             (unsigned long)s_cfg.th[2], (unsigned long)s_cfg.th[3]);
}
