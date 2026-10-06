// #FIELD-6: host-тесты guard-логики источника времени (main/net_time.c).
// net_time.c зависит только от stdbool/stdint — ESP-IDF не нужен, тестируется
// на хосте обычным gcc. Проверяем матрицу решения net_time_should_accept
// (sntp_synced × manual × Δt) и переходы источника (net_time_source_str).
// Соответствие пунктам приёмки ТЗ: T1 (SNTP приоритетнее), T3 (авто-коррекция
// от браузера при расхождении), T11 (ручная коррекция без интернета).
#include "net_time.h"
#include "test_util.h"
#include <string.h>

#define STR_EQ(a, b)  (strcmp((a), (b)) == 0)

// --- Матрица предиката приёма (чистая функция, без глобального состояния) ---

// T1: SNTP синхронизирован → любой POST /api/time отклоняется (война источников
// недопустима), даже ручной. sntp_synced доминирует над manual и над Δt.
static void test_sntp_priority(void)
{
    CHECK(net_time_should_accept(true, false, 0)      == false);  // авто, идеал
    CHECK(net_time_should_accept(true, false, 100000) == false);  // авто, большой Δ
    CHECK(net_time_should_accept(true, true,  0)      == false);  // ручной — тоже нет
    CHECK(net_time_should_accept(true, true,  100000) == false);  // ручной, большой Δ
}

// T11: без SNTP (полевой AP или STA без интернета) ручная установка принимается
// безусловно — оператор знает точное время лучше near-epoch часов платы.
static void test_manual_unconditional(void)
{
    CHECK(net_time_should_accept(false, true, 0)   == true);   // Δ=0, всё равно да
    CHECK(net_time_should_accept(false, true, 3)   == true);   // Δ<порог — да
    CHECK(net_time_should_accept(false, true, 999) == true);   // Δ большой — да
}

// T3: без SNTP авто-коррекция от браузера принимается ТОЛЬКО при заметном
// расхождении (> 5 с) — чтобы не дёргать часы на дребезге ±секунды при каждом
// открытии страницы. Граница строгая: 5 отклоняется, 6 принимается.
static void test_auto_threshold(void)
{
    CHECK(net_time_should_accept(false, false, 0)  == false);  // уже синхронно
    CHECK(net_time_should_accept(false, false, 5)  == false);  // ровно порог — нет
    CHECK(net_time_should_accept(false, false, 6)  == true);   // за порогом — да
    CHECK(net_time_should_accept(false, false, 60) == true);   // большой Δ — да
}

// --- Переходы источника времени (глобальное состояние net_time.c) ---
// Идёт последним: net_time_mark_sntp() необратимо ставит sntp_synced=true,
// после него предикат-тесты выше уже дали бы иные результаты (потому и раньше).
static void test_source_transitions(void)
{
    CHECK(net_time_sntp_synced() == false);          // старт: не синхронизировано
    CHECK(STR_EQ(net_time_source_str(), "none"));

    net_time_set_source(TIME_SRC_BROWSER);
    CHECK(STR_EQ(net_time_source_str(), "browser"));  // авто от браузера

    net_time_set_source(TIME_SRC_MANUAL);
    CHECK(STR_EQ(net_time_source_str(), "manual"));   // ручной ввод

    net_time_set_source(TIME_SRC_RTC);
    CHECK(STR_EQ(net_time_source_str(), "rtc"));      // #RTC-1: из DS3231 при старте

    net_time_mark_sntp();                             // пришёл реальный SNTP
    CHECK(net_time_sntp_synced() == true);
    CHECK(STR_EQ(net_time_source_str(), "sntp"));     // источник → sntp
}

void nettime_suite(void)
{
    test_sntp_priority();
    test_manual_unconditional();
    test_auto_threshold();
    test_source_transitions();
}
