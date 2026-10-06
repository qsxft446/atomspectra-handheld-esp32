#include "board_i2c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "board_i2c";
static i2c_master_bus_handle_t s_bus = NULL;
static bool s_init_done = false;
static SemaphoreHandle_t s_lock = NULL;

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
