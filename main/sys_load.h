#pragma once
// #SYS-1: загрузка ядер ESP32 для лога и шапки экрана (запрос 2026-09-30).
// Раз в секунду (esp_timer) берётся время, которое задача Idle каждого ядра
// провела в работе за эту секунду: загрузка = 100 % − доля Idle. Нужны
// CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS и CONFIG_FREERTOS_USE_TRACE_FACILITY
// (sdkconfig.defaults); часы счётчика — esp_timer, мкс.

void sys_load_init(void);
// Загрузка ядра 0/1 за последнюю секунду, %; -1 — ещё нет замера.
int sys_load_core(int core);
