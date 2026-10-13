// #ALM-1: сигнализация превышения порогов CPS — спикер на GPIO21 и мигание числа
// CPS на экране. Четыре порога по возрастанию; уровень — старший превышенный.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define CPS_ALARM_LEVELS   4
#define CPS_ALARM_TH_MAX   9999999u

typedef struct {
    bool     enabled;
    uint32_t th[CPS_ALARM_LEVELS];   // CPS, строго по возрастанию, th[0] >= 1
} cps_alarm_cfg_t;

void cps_alarm_init(void);
void cps_alarm_get_config(cps_alarm_cfg_t *out);
// 0 — сохранено; -1 — пороги не по возрастанию / вне диапазона; -2 — ошибка NVS
// (применено до перезагрузки). Пишет NVS — не из задачи со стеком в PSRAM.
int  cps_alarm_set_config(const cps_alarm_cfg_t *c);
// 0 — тревоги нет (выключена, нет набора, CPS ниже первого порога), 1..4 — уровень.
int  cps_alarm_level(void);

// #ALM-3: фактические длительности (мкс) писков, коротких пауз (между писками группы
// и на 4-м уровне) и длинных пауз (между группами) с последней смены уровня —
// замер ровности ритма на плате. min = INT32_MAX — таких отрезков ещё не было.
typedef struct {
    int      level;
    uint32_t beeps, gaps, pauses;
    int32_t  on_min_us, on_max_us, gap_min_us, gap_max_us, pause_min_us, pause_max_us;
} cps_alarm_diag_t;
void cps_alarm_diag(cps_alarm_diag_t *out);
