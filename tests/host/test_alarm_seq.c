// #ALM-3: host-тест ритма сигнализации CPS (main/alarm_seq.h).
// Отдельный исполняемый файл: make alarm (gcc) или cl /W4 test_alarm_seq.c (MSVC).
// Ждём "ALARM SEQ: ALL PASSED", код возврата 0.
#include <stdio.h>
#include <string.h>
#include "alarm_seq.h"

int g_failures;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_failures++; } } while (0)

#define MAXRUN 4096
typedef struct { bool on; int ms; } run_t;

// Прогнать ticks тиков с уровнем lv(t) и собрать серии одинакового состояния.
static int simulate(int (*lvf)(int t_ms), int ms, run_t *runs, alarm_seq_t *s)
{
    int n = 0;
    for (int t = 0; t < ms; t += ALARM_SEQ_TICK_MS) {
        bool on = alarm_seq_tick(s, lvf(t));
        if (n && runs[n - 1].on == on) runs[n - 1].ms += ALARM_SEQ_TICK_MS;
        else if (n < MAXRUN) { runs[n].on = on; runs[n].ms = ALARM_SEQ_TICK_MS; n++; }
    }
    return n;
}

static int g_const;
static int lv_const(int t) { (void)t; return g_const; }

// Постоянный уровень: серии строго по шаблону (кроме последней — обрезана концом прогона).
static void test_constant_levels(void)
{
    static run_t r[MAXRUN];
    for (int lv = 1; lv <= 4; lv++) {
        alarm_seq_t s = {0};
        g_const = lv;
        int n = simulate(lv_const, 20000, r, &s);
        CHECK(n > 10);
        CHECK(r[0].on);                               // начинается сразу с писка
        int bad = 0;
        for (int i = 0; i < n - 1; i++) {
            int want;
            if (lv == 4) want = ALARM_FAST_MS;
            else if (r[i].on) want = ALARM_BEEP_MS;
            else {
                // тишина: GAP внутри группы, PAUSE после N-го писка
                int beeps_before = 0;
                for (int j = i - 1; j >= 0 && (r[j].on || r[j].ms == ALARM_GAP_MS); j--) if (r[j].on) beeps_before++;
                want = beeps_before == lv ? ALARM_PAUSE_MS : ALARM_GAP_MS;
            }
            if (r[i].ms != want) { bad++; if (bad < 4) printf("  lv %d run %d: %s %d ms, want %d\n", lv, i, r[i].on ? "on" : "off", r[i].ms, want); }
        }
        CHECK(bad == 0);
        // число писков в группе: N на уровнях 1..3 (между паузами), 1 на уровне 4
        int beeps = 0, groups_ok = 1;
        for (int i = 0; i < n - 1; i++) {
            if (r[i].on) beeps++;
            if (!r[i].on && (lv == 4 || r[i].ms == ALARM_PAUSE_MS)) {
                if (beeps != (lv == 4 ? 1 : lv)) groups_ok = 0;
                beeps = 0;
            }
        }
        CHECK(groups_ok);
        int period = lv == 4 ? 2 * ALARM_FAST_MS : lv * ALARM_BEEP_MS + (lv - 1) * ALARM_GAP_MS + ALARM_PAUSE_MS;
        printf("  level %d: %d runs, period %d ms, ok\n", lv, n, period);
    }
}

static void test_level_zero(void)
{
    static run_t r[MAXRUN];
    alarm_seq_t s = {0};
    g_const = 0;
    int n = simulate(lv_const, 5000, r, &s);
    CHECK(n == 1 && !r[0].on);
}

// Пропал уровень посреди писка — тишина с того же тика.
static int lv_cut(int t) { return t < 50 ? 4 : 0; }
static void test_cut_mid_beep(void)
{
    static run_t r[MAXRUN];
    alarm_seq_t s = {0};
    int n = simulate(lv_cut, 1000, r, &s);
    CHECK(n == 2 && r[0].on && r[0].ms == 50 && !r[1].on);
}

// Рост уровня в паузе — новая группа сразу; падение в паузе — пауза доигрывает.
static int lv_rise(int t) { return t < 400 ? 1 : 3; }   // в паузе после 1-го писка (100..1100)
static int lv_fall(int t) { return t < 400 ? 3 : 1; }   // уровень 3: писки до 500 мс, падение внутри группы
static void test_level_change(void)
{
    static run_t r[MAXRUN];
    alarm_seq_t s = {0};
    int n = simulate(lv_rise, 3000, r, &s);
    // писк 100, тишина 100..400 (300 мс, оборвана), затем группа уровня 3
    CHECK(n > 4 && r[0].on && r[0].ms == 100 && !r[1].on && r[1].ms == 300 && r[2].on && r[2].ms == 100 &&
          r[3].ms == ALARM_GAP_MS);

    memset(&s, 0, sizeof s);
    n = simulate(lv_fall, 4000, r, &s);
    // группа уровня 3 доигрывается целиком (3 писка + пауза), потом уровень 1
    CHECK(n > 7 && r[0].on && r[2].on && r[4].on && r[4].ms == 100 && !r[5].on && r[5].ms == ALARM_PAUSE_MS &&
          r[6].on && r[6].ms == 100 && !r[7].on && r[7].ms == ALARM_PAUSE_MS);
}

// Уровень 4 при постоянном превышении: 120 мс период без единого отклонения на 60 с.
static void test_fast_long(void)
{
    static run_t r[MAXRUN];
    alarm_seq_t s = {0};
    g_const = 4;
    int n = simulate(lv_const, 60000, r, &s);
    int bad = 0;
    for (int i = 0; i < n; i++) if (r[i].ms != ALARM_FAST_MS) bad++;
    CHECK(n == 1000 && bad == 0);
}

static void test_level_of(void)
{
    const uint32_t th[4] = { 100, 200, 400, 800 };
    CHECK(alarm_level_of(0, th, 4) == 0);
    CHECK(alarm_level_of(99, th, 4) == 0);
    CHECK(alarm_level_of(100, th, 4) == 1);
    CHECK(alarm_level_of(399, th, 4) == 2);
    CHECK(alarm_level_of(400, th, 4) == 3);
    CHECK(alarm_level_of(800, th, 4) == 4);
    CHECK(alarm_level_of(4000000000u, th, 4) == 4);
}

static void test_dur_table(void)
{
    bool on;
    CHECK(alarm_seq_dur(1, 0, &on) == 100 && on);
    CHECK(alarm_seq_dur(1, 1, &on) == 1000 && !on);
    CHECK(alarm_seq_dur(1, 2, &on) == 0);
    CHECK(alarm_seq_dur(3, 4, &on) == 100 && on);
    CHECK(alarm_seq_dur(3, 5, &on) == 1000 && !on);
    CHECK(alarm_seq_dur(4, 1, &on) == 60 && !on);
    CHECK(alarm_seq_dur(4, 2, &on) == 0);
    CHECK(alarm_seq_dur(0, 0, &on) == 0 && !on);
}

int main(void)
{
    test_dur_table();
    test_level_of();
    test_constant_levels();
    test_level_zero();
    test_cut_mid_beep();
    test_level_change();
    test_fast_long();
    if (g_failures) { printf("ALARM SEQ: %d FAILED\n", g_failures); return 1; }
    printf("ALARM SEQ: ALL PASSED\n");
    return 0;
}
