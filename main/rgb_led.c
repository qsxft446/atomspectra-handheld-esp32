// #LED-1: индикация статуса USB-спектрометра бортовым RGB-светодиодом
// (WS2812, одна линия данных, драйвер через RMT — компонент espressif/led_strip).
//
// ESP32-S3-N16R8 DevKitC-1: светодиод на GPIO48 (на части ревизий платы — GPIO38;
// если после прошивки ничего не горит, поменяйте RGB_LED_GPIO ниже).
//
// Логика:
//   спектрометр не подключён (USB)                -> выключен
//   подключён, но данные сейчас не идут (простой)  -> горит ровно синим
//   подключён и активно идёт набор спектра         -> плавно "дышит" синим (fade in/out)
//
// «Активно идёт набор» определяется по свежести last_shproto_ts_ms из USB-диагностики
// (метка времени последнего валидного SHPROTO-пакета — гистограмма/статистика/осциллограмма
// приходят только пока прибор реально набирает и стримит данные).

#include "rgb_led.h"
#include "atomspectra.h"     // usb_host_cdc_is_connected/diag_snapshot, usb_diag_snapshot_t
#include "led_strip.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>

#ifndef RGB_LED_GPIO
#define RGB_LED_GPIO 48       // если LED не загорается — попробуйте GPIO 38
#endif

#define RGB_LED_BLUE_LEVEL    40    // пиковая яркость синего (0-255), приглушённая
#define RGB_LED_TICK_MS       30    // шаг анимации "дыхания" — держим плавным
#define RGB_LED_STATUS_MS     300   // период опроса USB-статуса (реже, чем анимация)
#define RGB_LED_BREATH_MS     1800  // длительность одного цикла вдох-выдох
#define RGB_LED_ACTIVE_AGE_MS 1500  // младше этого возраста last_shproto_ts_ms считаем "идут данные"
#define RGB_LED_TWO_PI         6.28318530717958647692f   // не полагаемся на M_PI (не всегда есть в math.h)

static const char *TAG = "rgb_led";
static led_strip_handle_t s_strip;

static inline void led_set(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_strip) return;
    led_strip_set_pixel(s_strip, 0, r, g, b);
    led_strip_refresh(s_strip);
}

static void rgb_led_task(void *arg)
{
    (void)arg;
    bool connected = false;
    bool active = false;
    uint32_t since_status_ms = RGB_LED_STATUS_MS;   // форсируем опрос статуса на первом шаге
    uint32_t breath_t_ms = 0;                       // фаза цикла "дыхания"

    while (1) {
        if (since_status_ms >= RGB_LED_STATUS_MS) {
            since_status_ms = 0;
            connected = usb_host_cdc_is_connected();
            if (connected) {
                usb_diag_snapshot_t d;
                usb_host_cdc_diag_snapshot(&d);
                uint32_t age = d.uptime_ms - d.last_shproto_ts_ms;   // wraparound-safe (uint32)
                active = (d.last_shproto_ts_ms != 0) && (age < RGB_LED_ACTIVE_AGE_MS);
            } else {
                active = false;
            }
        }

        if (!connected) {
            led_set(0, 0, 0);
            breath_t_ms = 0;
        } else if (active) {
            // Плавное "дыхание": яркость идёт по синусоиде 0 -> max -> 0, без рывков
            // на границах (в отличие от треугольной волны/резкого blink).
            float phase = (RGB_LED_TWO_PI * (float)breath_t_ms) / (float)RGB_LED_BREATH_MS;
            float level = (1.0f - cosf(phase)) * 0.5f;   // 0..1
            uint8_t b = (uint8_t)(level * RGB_LED_BLUE_LEVEL + 0.5f);
            led_set(0, 0, b);
            breath_t_ms = (breath_t_ms + RGB_LED_TICK_MS) % RGB_LED_BREATH_MS;
        } else {
            led_set(0, 0, RGB_LED_BLUE_LEVEL);   // подключён, но данные не идут — горит ровно
            breath_t_ms = 0;                     // следующий заход в "дыхание" — снова с нуля (плавный старт от 0)
        }

        vTaskDelay(pdMS_TO_TICKS(RGB_LED_TICK_MS));
        since_status_ms += RGB_LED_TICK_MS;
    }
}

void rgb_led_init(void)
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = RGB_LED_GPIO,
        .max_leds = 1,
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,
        .led_model = LED_MODEL_WS2812,
    };
    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
    };
    esp_err_t err = led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "led_strip init failed on GPIO%d: %s (RGB LED disabled)",
                 RGB_LED_GPIO, esp_err_to_name(err));
        return;
    }
    led_strip_clear(s_strip);
    xTaskCreatePinnedToCore(rgb_led_task, "rgb_led", 2560, NULL, 2, NULL, 0);
    ESP_LOGI(TAG, "RGB LED init OK (GPIO%d)", RGB_LED_GPIO);
}
