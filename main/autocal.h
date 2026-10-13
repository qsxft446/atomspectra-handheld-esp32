// #ACAL-1: автоматическая энергетическая калибровка по источнику (пока Ra-226).
// Чистый C без ESP-IDF — тестируется на хосте (tests/host/test_autocal.c) на
// реальных спектрах прибора пользователя (tests/host/data/).
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define AUTOCAL_MAX_LINES   16
#define AUTOCAL_MAX_COEFFS  5      // = CALIB_COEFFS платы: полином до 4-й степени

enum {
    AUTOCAL_OK = 0,
    AUTOCAL_E_NODATA,   // пустой/слишком бедный спектр
    AUTOCAL_E_NOPAIR,   // не найдена опорная пара Pb-214 352 / Bi-214 609
    AUTOCAL_E_FEW,      // опознано меньше 4 линий
    AUTOCAL_E_FIT,      // подгонка немонотонна или с большой невязкой
};

typedef struct {
    int         n;                              // опознанных линий
    double      ch[AUTOCAL_MAX_LINES];          // центр пика, канал
    double      kev[AUTOCAL_MAX_LINES];         // энергия линии
    double      fit_kev[AUTOCAL_MAX_LINES];     // энергия по найденной калибровке
    double      net[AUTOCAL_MAX_LINES];         // площадь пика над фоном, отсчёты
    const char *name[AUTOCAL_MAX_LINES];
    double      coeffs[AUTOCAL_MAX_COEFFS];     // E(ch) = sum coeffs[i] * ch^i
    int         order;                          // степень полинома
    double      rms_kev, max_abs_kev;
    bool        has_top;                        // найдена верхняя опора (Bi-214 1764.5)
    double      net609, net_top;                // площади 609 и 1764 — для «хватает статистики»
    int         err;
} autocal_result_t;

// bins — спектр (n каналов), ws — рабочий буфер не меньше 2*n float.
// Возвращает out->err.
int    autocal_ra226(const uint32_t *bins, int n, float *ws, autocal_result_t *out);
double autocal_eval(const double *coeffs, int order, double ch);
// Статистики хватает, чтобы остановить набор (полином 4-й степени, все опоры).
bool   autocal_enough(const autocal_result_t *r);
