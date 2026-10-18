// #ISO-1 (запрос 2026-10-17): определение нуклидов по спектру во время набора.
// Метод — от нуклида к спектру (методика ЛСРМ, как identify.py в spectravibe-toolkit,
// VibeEngineering LLC, MIT): сначала характерная линия нуклида, потом остальные его линии
// и их ожидаемая сила; найденное вычитается из площадей пиков (общая линия не достаётся
// двоим). Библиотека — 30 источников (цепочки объединены), линии и выходы — из
// data/nuclides.json того же репозитория (ENSDF/NuDat). Чистый C без ESP-IDF —
// тестируется на хосте (tests/host/test_isoid.c) на реальных спектрах AtomSpectra.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ISOID_MAX_FOUND  8
#define ISOID_MAX_MARK   6     // отмечаемых пиков на найденный источник

typedef struct {
    int   src;                         // индекс в библиотеке (isoid_name)
    bool  natural;                     // природный (K-40, ряды U/Th) — показывать серым
    float conf;                        // 0..1: доля ожидаемых линий, что найдены
    float sig;                         // значимость характерной линии, сигм
    int   quality;                     // #ISO-7: качество обнаружения 0..100 % (оценка, не вероятность)
    int   n_mark;
    float mark_ch[ISOID_MAX_MARK];     // центры пиков его линий, канал
    float mark_kev[ISOID_MAX_MARK];
} isoid_found_t;

typedef struct {
    int           n;                   // найдено источников (по убыванию значимости)
    isoid_found_t f[ISOID_MAX_FOUND];
    int           n_peaks;             // пиков в спектре (диагностика)
} isoid_result_t;

// #ISO-3 (2026-10-18, «AtomSpectra — серия приборов»): самонастройка под подключённый прибор.
// kw — множитель модели ширины пика (разрешение кристаллов серии разное), ga/gb — поправка
// шкалы E' = E * (ga + gb*E) (шкала плывёт от загрузки и температуры, у прибора пользователя
// до 3-4 % вверху). isoid_run() работает с tin и в tout кладёт новую оценку по этому спектру:
// ширина — по сильным уединённым пикам выше 200 кэВ, шкала — по линиям уверенно найденных
// источников. Вызывающий усредняет оценки между прогонами и сбрасывает при очистке спектра.
typedef struct { float kw, ga, gb; int n_w, n_g; } isoid_tune_t;
void        isoid_tune_default(isoid_tune_t *t);

int         isoid_count(void);         // источников в библиотеке
const char *isoid_name(int src);
bool        isoid_natural(int src);
size_t      isoid_ws_size(int n);      // байт рабочего буфера для спектра из n каналов
// bins — спектр (n каналов), cal/order — калибровка E(ch) = sum cal[i]*ch^i. ws —
// isoid_ws_size(n) байт. Возвращает число найденных (= out->n), -1 — нет калибровки.
int         isoid_run(const uint32_t *bins, int n, const double *cal, int order, void *ws,
                      const isoid_tune_t *tin, isoid_tune_t *tout, isoid_result_t *out);
// Отладка (хост): печать пиков и решений по источникам.
extern int isoid_debug;
