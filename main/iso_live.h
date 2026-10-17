// #ISO-1: определение нуклидов во время набора — фоновая задача над isoid.c. Раз в
// ISO_PERIOD_MS, если спектр изменился и у него есть калибровка: снимок, isoid_run(),
// результат под замком. Экран (tft_display.c) и веб (/api/isotopes) только читают.
#pragma once
#include <stdint.h>
#include "isoid.h"

#define ISO_PERIOD_MS  5000

// Цвета найденного (экран и веб одинаково): яркие из ISO_PAL по порядку в списке; серый
// ISO_COL_NATURAL — только K-40 (запрос 2026-10-17: «серый только для K-40»).
#define ISO_PAL_N        8
#define ISO_COL_NATURAL  0x9AA0A6
extern const uint32_t ISO_PAL[ISO_PAL_N];
uint32_t iso_live_color(const isoid_result_t *r, int i);

void     iso_live_init(void);                  // после spectrum_init()
// Копия последнего результата; возвращает его номер (0 — ещё не было, спектра/калибровки нет).
uint32_t iso_live_get(isoid_result_t *out);
// Длительность последнего прогона, мс (диагностика).
uint32_t iso_live_last_ms(void);
