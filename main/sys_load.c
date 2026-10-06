// #SYS-1: см. sys_load.h.
#include "sys_load.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "sysload";

static volatile int s_load[2] = {-1, -1};
static uint32_t     s_idle_prev[2];
static int64_t      s_t_prev;

static uint32_t idle_runtime(int core)
{
    TaskStatus_t st;
    vTaskGetInfo(xTaskGetIdleTaskHandleForCore(core), &st, pdFALSE, eInvalid);
    return st.ulRunTimeCounter;   // мкс (CONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER)
}

static void sample_cb(void *arg)
{
    (void)arg;
    int64_t now = esp_timer_get_time();
    int64_t dt = now - s_t_prev;
    for (int c = 0; c < 2; c++) {
        uint32_t idle = idle_runtime(c);
        uint32_t d = idle - s_idle_prev[c];   // 32-битный счётчик: разность верна и через переполнение
        s_idle_prev[c] = idle;
        if (s_t_prev && dt > 0) {
            int busy = 100 - (int)((int64_t)d * 100 / dt);
            s_load[c] = busy < 0 ? 0 : busy > 100 ? 100 : busy;
        }
    }
    s_t_prev = now;
}

void sys_load_init(void)
{
    const esp_timer_create_args_t a = { .callback = sample_cb, .name = "sys_load" };
    esp_timer_handle_t h;
    if (esp_timer_create(&a, &h) != ESP_OK || esp_timer_start_periodic(h, 1000000) != ESP_OK) {
        ESP_LOGW(TAG, "таймер замера загрузки не запущен");
        return;
    }
    sample_cb(NULL);   // опорная точка
}

int sys_load_core(int core)
{
    return (core == 0 || core == 1) ? s_load[core] : -1;
}
