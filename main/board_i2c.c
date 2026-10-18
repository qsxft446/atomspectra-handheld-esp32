#include "board_i2c.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "board_i2c";
static i2c_master_bus_handle_t s_bus = NULL;
static bool s_init_done = false;
static SemaphoreHandle_t s_lock = NULL;

// #I2C-1 (2026-10-18, «иногда при включении нет экрана и WiFi»): расклинивание шины до её
// создания. Ведомый, сброшенный не вместе с ESP32 (DS3231 живёт от батарейки, BQ25895 — от АКБ),
// может держать SDA в нуле, пока не дотактует начатый байт (даташит DS3231: тактовать SCL, пока
// SDA не отпустят). Драйвер ESP-IDF при создании шины этого не делает. До 16 тактов SCL, потом
// STOP. Возвращает число тактов (0 — шина была свободна), -1 — SDA не отпустили, -2 — SCL в нуле.
static int bus_recover(void)
{
    const int sda = BOARD_I2C_SDA_GPIO, scl = BOARD_I2C_SCL_GPIO;
    gpio_set_level(sda, 1);
    gpio_set_level(scl, 1);
    gpio_config_t c = {
        .pin_bit_mask = (1ULL << sda) | (1ULL << scl),
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&c);
    esp_rom_delay_us(20);
    if (gpio_get_level(scl) == 0) return -2;   // ведомый держит такт — тактами не помочь
    int n = 0;
    while (gpio_get_level(sda) == 0 && n < 16) {
        gpio_set_level(scl, 0); esp_rom_delay_us(10);
        gpio_set_level(scl, 1); esp_rom_delay_us(10);
        n++;
    }
    bool ok = gpio_get_level(sda) == 1;
    gpio_set_level(scl, 0); esp_rom_delay_us(10);   // STOP: SDA 0 -> 1 при SCL = 1
    gpio_set_level(sda, 0); esp_rom_delay_us(10);
    gpio_set_level(scl, 1); esp_rom_delay_us(10);
    gpio_set_level(sda, 1); esp_rom_delay_us(10);
    return ok ? n : -1;
}

i2c_master_bus_handle_t board_i2c_get_bus(void)
{
    if (!s_lock) {
        // #DISP-1: гонка на самом первом вызове из двух задач теоретически
        // возможна, но обе (battery_init/oled_display_init) вызываются
        // последовательно из app_main() — на практике безопасно.
        s_lock = xSemaphoreCreateMutex();
    }
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);

    if (!s_init_done) {
        s_init_done = true;
        int rc = bus_recover();   // #I2C-1
        if (rc > 0)       ESP_LOGW(TAG, "#I2C-1: SDA was held low — released after %d SCL clocks", rc);
        else if (rc == -1) ESP_LOGE(TAG, "#I2C-1: SDA held low, not released after 16 SCL clocks");
        else if (rc == -2) ESP_LOGE(TAG, "#I2C-1: SCL held low by a device");
        i2c_master_bus_config_t bus_config = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = BOARD_I2C_SDA_GPIO,
            .scl_io_num = BOARD_I2C_SCL_GPIO,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .flags.enable_internal_pullup = true,
        };
        esp_err_t err = i2c_new_master_bus(&bus_config, &s_bus);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "i2c_new_master_bus failed: %s (SDA=%d SCL=%d)",
                     esp_err_to_name(err), BOARD_I2C_SDA_GPIO, BOARD_I2C_SCL_GPIO);
            s_bus = NULL;
        } else {
            ESP_LOGI(TAG, "shared I2C bus up (SDA=%d SCL=%d)", BOARD_I2C_SDA_GPIO, BOARD_I2C_SCL_GPIO);
        }
    }

    i2c_master_bus_handle_t bus = s_bus;
    if (s_lock) xSemaphoreGive(s_lock);
    return bus;
}
