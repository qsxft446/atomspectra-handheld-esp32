#include "net_time.h"

// #FIELD-5: состояние источника времени. Гвардируется одним writer'ом (SNTP-cb /
// httpd-поток /api/time) — гонок нет (bool/enum атомарны на ESP32 для чтения UI).
static bool              s_sntp_synced = false;
static net_time_source_t s_source      = TIME_SRC_NONE;

// #FIELD-5 (A2): единственный источник истины про приём времени. Реализация —
// в заголовке подробно; здесь только предикат (тестируется host-тестом матрицей
// sntp_synced × manual × Δt).
bool net_time_should_accept(bool sntp_synced, bool manual, int64_t dt_abs_sec)
{
    if (sntp_synced) return false;   // SNTP приоритетнее — не перезаписываем (T1)
    if (manual)      return true;    // ручная установка — безусловно
    return dt_abs_sec > 5;           // auto — только при заметном расхождении (> 5 с)
}

void net_time_mark_sntp(void)
{
    s_sntp_synced = true;
    s_source      = TIME_SRC_SNTP;
}

bool net_time_sntp_synced(void)
{
    return s_sntp_synced;
}

void net_time_set_source(net_time_source_t src)
{
    s_source = src;
}

net_time_source_t net_time_source(void)
{
    return s_source;
}

const char *net_time_source_str(void)
{
    switch (s_source) {
        case TIME_SRC_SNTP:    return "sntp";
        case TIME_SRC_BROWSER: return "browser";
        case TIME_SRC_MANUAL:  return "manual";
        case TIME_SRC_RTC:     return "rtc";      // #RTC-1
        default:               return "none";
    }
}
