// #ACAL-1: автоматическая энергетическая калибровка по источнику.
// #ACAL-6: источники Ra-226, Th-232, Na-22, Cs-137+K-40 (таблицы линий в autocal.c).
// Чистый C без ESP-IDF — тестируется на хосте (tests/host/test_autocal.c) на
// реальных спектрах прибора пользователя (tests/host/data/) и синтетических.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define AUTOCAL_MAX_LINES   16
#define AUTOCAL_MAX_COEFFS  5      // = CALIB_COEFFS платы: полином до 4-й степени
#define AUTOCAL_SIGMA_MAX   2.5    // #ACAL-4/5: кэВ — точность худшей линии для остановки набора (было 1.5)

enum {
    AUTOCAL_OK = 0,
    AUTOCAL_E_NODATA,   // пустой/слишком бедный спектр
    AUTOCAL_E_NOPAIR,   // не найдена опорная пара линий источника
    AUTOCAL_E_FEW,      // опознано слишком мало линий
    AUTOCAL_E_FIT,      // подгонка немонотонна или с большой невязкой
    AUTOCAL_E_WRONGSRC, // спектр лучше объясняет другой источник (wrong_src)
};

// #ACAL-6: источники. Порядок = кнопки на экране.
enum { AUTOCAL_SRC_RA226 = 0, AUTOCAL_SRC_TH232, AUTOCAL_SRC_NA22, AUTOCAL_SRC_CS137_K40, AUTOCAL_SRC_COUNT };

// Как получена калибровка.
enum {
    AUTOCAL_MODE_POLY = 0,   // полином по линиям источника
    AUTOCAL_MODE_MAP,        // мало линий: текущая калибровка, каналы растянуты/сдвинуты (форма сохранена)
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
    bool        has_top;                        // найдена верхняя опора источника (Ra: 1764.5, Th: 2614.5)
    double      net609, net_top;                // площади: 609 (Ra) и верхней опоры
    // #ACAL-4: статистическая погрешность центра пика, кэВ: sigma/sqrt(net) x2 (фон под
    // пиком). Набор останавливается, когда худшая линия не хуже AUTOCAL_SIGMA_MAX.
    double      sigma_kev[AUTOCAL_MAX_LINES];
    double      max_sigma_kev;
    int         source;                         // AUTOCAL_SRC_*
    int         mode;                           // AUTOCAL_MODE_*
    double      map_a, map_b;                   // MAP: канал старой шкалы = a*ch + b
    int         wrong_src;                      // E_WRONGSRC: какой источник подходит лучше
    int         err;
} autocal_result_t;

// bins — спектр (n каналов), ws — рабочий буфер не меньше 2*n float.
// old_c/old_order — текущая калибровка (NULL — нет): нужна MAP-режиму (Na-22, Cs+K).
// Возвращает out->err.
int    autocal_run(int source, const uint32_t *bins, int n, float *ws,
                   const double *old_c, int old_order, autocal_result_t *out);
int    autocal_ra226(const uint32_t *bins, int n, float *ws, autocal_result_t *out);   // = autocal_run(RA226, без старой)
double autocal_eval(const double *coeffs, int order, double ch);
// Статистики хватает, чтобы остановить набор (по требованиям источника).
bool   autocal_enough(const autocal_result_t *r);
const char *autocal_source_name(int source);   // "Ra-226" ...
int    autocal_source_max_s(int source);        // предел набора, с
