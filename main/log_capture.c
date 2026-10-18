// #LOG-1: перехват ESP_LOG* в кольцо PSRAM (см. log_capture.h).
#include "log_capture.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define RING_SZ   (64 * 1024)
#define LINE_MAX  384

static char             *s_ring;
static uint32_t          s_wpos;           // всего записано байт; место в кольце — s_wpos % RING_SZ
static SemaphoreHandle_t s_mtx;
static char              s_line[LINE_MAX]; // форматирование под s_mtx — не на стеке вызывающей задачи
static vprintf_like_t    s_prev;

static void ring_put(const char *p, size_t n)
{
    size_t at = s_wpos % RING_SZ, a = n < RING_SZ - at ? n : RING_SZ - at;
    memcpy(s_ring + at, p, a);
    memcpy(s_ring, p + a, n - a);
    s_wpos += n;
}

// Строка форматируется один раз: в UART (как штатный vprintf) и в кольцо. Вызов из задач
// (ESP_LOG из ISR не ходит через vprintf). Замок занят дольше 20 мс — строка только в UART.
static int cap_vprintf(const char *fmt, va_list ap)
{
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(20)) != pdTRUE) return s_prev(fmt, ap);
    int n = vsnprintf(s_line, LINE_MAX, fmt, ap);
    if (n > 0) {
        size_t len = n < LINE_MAX ? (size_t)n : LINE_MAX - 1;
        if ((size_t)n >= LINE_MAX) s_line[len - 1] = '\n';   // обрезанная строка — с переводом
        fwrite(s_line, 1, len, stdout);
        ring_put(s_line, len);
    }
    xSemaphoreGive(s_mtx);
    return n;
}

void log_capture_init(void)
{
    s_ring = heap_caps_malloc(RING_SZ, MALLOC_CAP_SPIRAM);
    s_mtx = xSemaphoreCreateMutex();
    if (!s_ring || !s_mtx) {
        ESP_LOGW("logcap", "#LOG-1: no memory, ESP32 log is UART only");
        return;
    }
    s_prev = esp_log_set_vprintf(cap_vprintf);
}

size_t log_capture_read(uint32_t since, char *buf, size_t cap, uint32_t *next, bool *lost)
{
    *lost = false;
    if (!s_ring) { *next = 0; return 0; }
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    uint32_t w = s_wpos, oldest = w > RING_SZ ? w - RING_SZ : 0, from = since;
    bool mid = false;   // начало посреди строки — пропустить до первой целой
    if (from > w) { from = oldest; mid = oldest > 0; }
    if (from < oldest) { from = oldest; *lost = true; mid = oldest > 0; }
    if (w - from > cap) { from = w - cap; *lost = true; mid = true; }
    size_t n = w - from, at = from % RING_SZ, a = n < RING_SZ - at ? n : RING_SZ - at;
    memcpy(buf, s_ring + at, a);
    memcpy(buf + a, s_ring, n - a);
    xSemaphoreGive(s_mtx);
    *next = w;
    size_t off = 0;
    if (mid) {
        while (off < n && buf[off] != '\n') off++;
        if (off < n) off++;
        memmove(buf, buf + off, n - off);
    }
    return n - off;
}
