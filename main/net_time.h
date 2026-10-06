#pragma once
#include <stdbool.h>
#include <stdint.h>

// #FIELD-5: источник времени платы (для индикатора в system.html).
typedef enum {
    TIME_SRC_NONE = 0,   // near-epoch (1970), не синхронизировано
    TIME_SRC_SNTP,       // от SNTP (только Indoor/STA с интернетом)
    TIME_SRC_BROWSER,    // авто от браузера телефона (POST /api/time)
    TIME_SRC_MANUAL,     // ручной ввод пользователя
    TIME_SRC_RTC,        // #RTC-1: из часов DS3231 при старте платы
} net_time_source_t;

// #FIELD-5 (A2): ЧИСТАЯ guard-логика приёма POST /api/time. Без ESP-зависимостей —
// host-тестируема (tests/host/). Единый предикат, режим инкапсулирован в sntp_synced:
//   - sntp_synced == true  → отклонить (SNTP приоритетнее, война источников недопустима; T1);
//   - sntp_synced == false → manual: принять безусловно; auto: принять при |Δ| > 5 с.
// В полевом AP init_sntp не поднят → sntp_synced всегда false → время от браузера идёт.
// В STA без интернета SNTP не отрабатывает → sntp_synced остаётся false → ручная коррекция
// возможна (T11). near-epoch как предикат НЕ используется (A2: ложно false после 1-го sync).
bool net_time_should_accept(bool sntp_synced, bool manual, int64_t dt_abs_sec);

void net_time_mark_sntp(void);                 // вызвать из SNTP time_sync_cb (main)
bool net_time_sntp_synced(void);
void net_time_set_source(net_time_source_t src);
net_time_source_t net_time_source(void);
const char *net_time_source_str(void);         // "none"/"sntp"/"browser"/"manual"/"rtc" — для UI
