// #ALM-3: чистая логика сигнализации CPS — уровень по порогам и ритм писка.
// Без ESP-IDF: тестируется на хосте (tests/host/test_alarm_seq.c).
//
// Ритм задаёт периодический тик ALARM_SEQ_TICK_MS (на плате — esp_timer, а не
// vTaskDelay задачи: тик FreeRTOS 10 мс давал каждому интервалу ±10 мс, а задача
// с приоритетом 2 на ядре 1 стояла, пока LVGL рисовал кадр, — «та-та-та» было
// неровным). Все длительности кратны тику, поэтому ритм точный.
//
// Уровень N (1..3): N писков ALARM_BEEP_MS через ALARM_GAP_MS и пауза ALARM_PAUSE_MS
// («-» в записи пользователя «.-.-» — пауза, #ALM-2). Уровень 4: писк/тишина
// ALARM_FAST_MS. Новый уровень — с начала следующей группы; рост уровня во время
// паузы начинает новую группу сразу; уровень 0 — тишина с этого же тика.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define ALARM_SEQ_TICK_MS  10
#define ALARM_BEEP_MS      100
#define ALARM_GAP_MS       100
#define ALARM_PAUSE_MS     1000
#define ALARM_FAST_MS      60
#define ALARM_SEQ_LEVELS   4

// Уровень: сколько порогов (по возрастанию) не выше cps.
static inline int alarm_level_of(uint32_t cps, const uint32_t *th, int n)
{
    int lv = 0;
    while (lv < n && cps >= th[lv]) lv++;
    return lv;
}

// Длительность шага step шаблона уровня lv; *on — писк или тишина. 0 — шагов больше нет.
static inline int alarm_seq_dur(int lv, int step, bool *on)
{
    int d = 0;
    bool o = false;
    if (lv >= ALARM_SEQ_LEVELS) {                 // 4: [писк, тишина]
        if (step < 2) { o = step == 0; d = ALARM_FAST_MS; }
    } else if (lv >= 1) {                         // N писков, между ними GAP, в конце PAUSE
        int last = 2 * lv - 1;                    // индекс паузы
        if (step < last)       { o = (step % 2) == 0; d = o ? ALARM_BEEP_MS : ALARM_GAP_MS; }
        else if (step == last) { o = false; d = ALARM_PAUSE_MS; }
    }
    if (on) *on = o;
    return d;
}

typedef struct {
    int  level;     // уровень текущей группы, 0 — тишина
    int  step;
    int  left_ms;   // осталось у шага
    bool on;
} alarm_seq_t;

static inline void alarm_seq_start(alarm_seq_t *s, int lv)
{
    s->level = lv;
    s->step = 0;
    s->left_ms = alarm_seq_dur(lv, 0, &s->on);
}

// Вызывать каждые ALARM_SEQ_TICK_MS с текущим уровнем. Возвращает, звучит ли
// тон в этот тик.
static inline bool alarm_seq_tick(alarm_seq_t *s, int lv)
{
    if (lv > ALARM_SEQ_LEVELS) lv = ALARM_SEQ_LEVELS;
    if (lv <= 0) {
        s->level = 0;
        s->on = false;
        return false;
    }
    if (s->level == 0) {
        alarm_seq_start(s, lv);
    } else if (lv > s->level && !s->on && alarm_seq_dur(s->level, s->step + 1, 0) == 0) {
        alarm_seq_start(s, lv);   // рост уровня в паузе группы — не ждать её конца
    }
    bool out = s->on;
    s->left_ms -= ALARM_SEQ_TICK_MS;
    if (s->left_ms <= 0) {
        bool on;
        int d = alarm_seq_dur(s->level, s->step + 1, &on);
        if (d) { s->step++; s->left_ms = d; s->on = on; }
        else   alarm_seq_start(s, lv);           // группа кончилась — следующая по текущему уровню
    }
    return out;
}
