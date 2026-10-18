// #LOG-1 (запрос 2026-10-18): лог ESP32 на странице «Сервис». Вывод ESP_LOG* перехватывается
// (esp_log_set_vprintf), идёт в UART как раньше и в кольцо 64 КБ в PSRAM; GET /api/esplog.
// Прямой printf() мимо ESP_LOG в кольцо не попадает; лог загрузчика и до log_capture_init() — тоже.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void   log_capture_init(void);
// Текст лога с позиции since (байт с начала работы) — в buf до cap байт, целыми строками.
// *next — позиция для следующего запроса; *lost — часть строк после since уже вытеснена из кольца.
// since больше текущей позиции (плата перезагрузилась) — с самой старой строки кольца.
size_t log_capture_read(uint32_t since, char *buf, size_t cap, uint32_t *next, bool *lost);
