// #ISO-1: определение нуклидов (см. isoid.h). Шаги:
//  1. Пики: «цилиндр» по сглаженному спектру (как autocal.c), ширина — в кэВ по модели
//     NaI FWHM = FW_K*sqrt(E) и переводится в каналы по калибровке (шкалы приборов
//     разные: 2048/8192 каналов). Площадь над линейным фоном ±1 FWHM, значимость
//     net/sqrt(net + 2*фон). Широкие горбы (обратное рассеяние ~200 кэВ) — не пики.
//  2. Источники: бленды линий ближе 0.5 FWHM сливаются; вес линии = выход * отн.
//     эффективность NaI. Характерная — самая весомая гамма-линия в диапазоне шкалы;
//     она должна быть найдена со значимостью >= Z_CHAR. Масштаб — по её площади;
//     у остальных линий ожидаемая площадь и значимость. Достоверность = сумма весов
//     найденных (с поправкой «нашли меньше, чем ждали») / сумма весов линий, которые
//     при таком масштабе должны быть видны (ожидаемая значимость >= 3).
//  3. Жадно: лучший источник принимается, его ожидаемые площади вычитаются из пиков,
//     поиск повторяется (511 кэВ Na-22 / аннигиляция / Tl-208, 186 Ra-226 / U-235).
#include "isoid.h"
#include <math.h>
#include <string.h>

int isoid_debug;
#ifdef ISOID_HOST_DEBUG
#include <stdio.h>
#define DBG(...) do { if (isoid_debug) printf(__VA_ARGS__); } while (0)
#else
#define DBG(...) do { } while (0)
#endif

typedef struct { double kev, inten; unsigned char xray; } iso_line_t;
typedef struct { const char *name; short first, n; bool natural; } iso_src_t;

// Сгенерировано из spectravibe-toolkit data/nuclides.json (MIT License, Copyright (c) 2026 SpectraVibe —
// текст лицензии: tests/host/data/svk/LICENSE; данные ENSDF/NuDat):
// гамма-линии >= 30 кэВ с выходом >= 1 % на распад головы цепочки в равновесии. x = рентген
// (добавлен вручную, выходы приближённые) — только опора: не бывает характерной линией и
// не входит в достоверность, но объясняет пик (рентген Bi/Pb ~75-90 кэВ у рядов U/Th).
static const iso_line_t LINES[] = {
    { 1460.82, 10.66, 0 },
    { 53.23, 1.07, 0 },
    { 76.30, 17.00, 1 },
    { 87.30, 5.00, 1 },
    { 186.21, 3.56, 0 },
    { 242.00, 7.26, 0 },
    { 295.22, 18.47, 0 },
    { 351.93, 35.72, 0 },
    { 609.32, 45.44, 0 },
    { 665.45, 1.54, 0 },
    { 768.36, 4.89, 0 },
    { 785.96, 1.06, 0 },
    { 806.18, 1.26, 0 },
    { 934.06, 3.09, 0 },
    { 1120.29, 14.90, 0 },
    { 1155.21, 1.63, 0 },
    { 1238.12, 5.83, 0 },
    { 1280.98, 1.44, 0 },
    { 1377.67, 3.98, 0 },
    { 1401.52, 1.33, 0 },
    { 1407.99, 2.39, 0 },
    { 1509.21, 2.13, 0 },
    { 1661.27, 1.05, 0 },
    { 1729.60, 2.87, 0 },
    { 1764.49, 15.29, 0 },
    { 1847.43, 2.03, 0 },
    { 2118.51, 1.16, 0 },
    { 2204.10, 4.92, 0 },
    { 2447.69, 1.54, 0 },
    { 39.86, 1.06, 0 },
    { 76.00, 29.00, 1 },
    { 87.50, 9.00, 1 },
    { 92.60, 6.00, 1 },
    { 99.51, 1.26, 0 },
    { 129.06, 2.42, 0 },
    { 209.25, 3.89, 0 },
    { 238.63, 43.60, 0 },
    { 240.99, 4.10, 0 },
    { 270.25, 3.46, 0 },
    { 277.37, 2.37, 0 },
    { 300.09, 3.30, 0 },
    { 328.00, 2.95, 0 },
    { 338.32, 11.27, 0 },
    { 409.46, 1.92, 0 },
    { 463.00, 4.40, 0 },
    { 510.77, 8.12, 0 },
    { 583.19, 30.55, 0 },
    { 674.75, 2.10, 0 },
    { 727.33, 6.67, 0 },
    { 755.32, 1.00, 0 },
    { 772.29, 1.49, 0 },
    { 785.37, 1.10, 0 },
    { 794.95, 4.25, 0 },
    { 835.71, 1.61, 0 },
    { 860.56, 4.49, 0 },
    { 911.20, 25.80, 0 },
    { 964.77, 4.99, 0 },
    { 968.97, 15.80, 0 },
    { 1588.20, 3.22, 0 },
    { 1620.50, 1.47, 0 },
    { 1630.63, 1.51, 0 },
    { 2614.51, 35.85, 0 },
    { 63.29, 3.67, 0 },
    { 92.38, 2.13, 1 },
    { 92.80, 2.10, 1 },
    { 109.18, 1.64, 0 },
    { 143.76, 10.93, 0 },
    { 163.36, 5.07, 0 },
    { 185.71, 57.20, 0 },
    { 202.11, 1.07, 0 },
    { 205.31, 5.03, 0 },
    { 661.66, 85.10, 0 },
    { 475.37, 1.48, 0 },
    { 563.25, 8.34, 0 },
    { 569.33, 15.37, 0 },
    { 604.72, 97.62, 0 },
    { 795.86, 85.46, 0 },
    { 801.95, 8.69, 0 },
    { 1167.97, 1.79, 0 },
    { 1365.18, 3.02, 0 },
    { 1173.23, 99.85, 0 },
    { 1332.49, 99.98, 0 },
    { 59.54, 35.90, 0 },
    { 53.16, 2.14, 0 },
    { 79.61, 2.65, 0 },
    { 81.00, 32.95, 1 },
    { 276.40, 7.16, 0 },
    { 302.85, 18.34, 0 },
    { 356.01, 62.05, 0 },
    { 383.85, 8.94, 0 },
    { 511.00, 180.70, 2 },   // Na-22: аннигиляция — обязательна, но не характерная (см. evaluate)
    { 1274.54, 99.94, 0 },
    { 121.78, 28.53, 0 },
    { 244.70, 7.55, 0 },
    { 344.28, 26.59, 0 },
    { 411.12, 2.24, 0 },
    { 443.96, 2.83, 0 },
    { 778.90, 12.93, 0 },
    { 867.38, 4.23, 0 },
    { 964.06, 14.51, 0 },
    { 1085.84, 10.11, 0 },
    { 1089.74, 1.73, 0 },
    { 1112.08, 13.67, 0 },
    { 1212.95, 1.42, 0 },
    { 1299.14, 1.63, 0 },
    { 1408.01, 20.87, 0 },
    { 123.07, 40.40, 0 },
    { 247.93, 6.89, 0 },
    { 591.75, 4.95, 0 },
    { 692.42, 1.78, 0 },
    { 723.30, 20.06, 0 },
    { 756.80, 4.52, 0 },
    { 873.18, 12.08, 0 },
    { 996.29, 10.48, 0 },
    { 1004.76, 18.01, 0 },
    { 1274.43, 34.80, 0 },
    { 1596.48, 1.80, 0 },
    { 80.19, 2.62, 0 },
    { 284.31, 6.12, 0 },
    { 364.49, 81.50, 0 },
    { 636.99, 7.16, 0 },
    { 722.91, 1.77, 0 },
    { 140.51, 89.06, 0 },
    { 122.06, 85.60, 0 },
    { 136.47, 10.68, 0 },
    { 834.85, 99.98, 0 },
    { 112.95, 6.23, 0 },
    { 208.37, 10.41, 0 },
    { 898.04, 93.68, 0 },
    { 1836.06, 99.24, 0 },
    { 46.54, 4.25, 0 },     // Pb-210 (вместо Zn-65: его 1115 не отличить от Bi-214 1120)
    { 569.70, 97.75, 0 },
    { 1063.66, 74.50, 0 },
    { 1770.23, 6.87, 0 },
    { 328.76, 20.32, 0 },
    { 432.49, 2.90, 0 },
    { 487.02, 45.51, 0 },
    { 751.64, 4.33, 0 },
    { 815.77, 23.28, 0 },
    { 867.85, 5.50, 0 },
    { 919.55, 2.66, 0 },
    { 925.19, 6.90, 0 },
    { 1596.21, 95.40, 0 },
    { 2521.40, 3.46, 0 },
    { 165.86, 79.90, 0 },
    { 35.49, 4.37, 0 },
    { 176.31, 6.84, 0 },
    { 380.45, 1.52, 0 },
    { 427.87, 29.60, 0 },
    { 463.37, 10.49, 0 },
    { 600.60, 17.65, 0 },
    { 606.71, 4.98, 0 },
    { 635.95, 11.22, 0 },
    { 671.44, 1.79, 0 },
    { 91.27, 3.11, 0 },
    { 93.31, 38.81, 0 },
    { 184.58, 21.41, 0 },
    { 208.95, 2.46, 0 },
    { 300.22, 16.64, 0 },
    { 393.53, 4.56, 0 },
    { 171.28, 90.65, 0 },
    { 245.35, 94.09, 0 },
    { 159.00, 83.60, 0 },
    { 528.97, 1.27, 0 },
    { 70.80, 73.00, 1 },
    { 80.30, 20.00, 1 },
    { 135.34, 2.60, 0 },
    { 167.43, 10.00, 0 },
    { 82.80, 40.00, 1 },
    { 94.90, 11.00, 1 },
    { 122.32, 1.30, 0 },
    { 144.24, 3.47, 0 },
    { 154.21, 6.02, 0 },
    { 269.46, 13.30, 0 },
    { 271.23, 10.80, 0 },
    { 323.87, 3.64, 0 },
    { 338.28, 2.60, 0 },
    { 401.81, 6.59, 0 },
    { 404.85, 3.78, 0 },
    { 427.09, 1.76, 0 },
    { 445.03, 1.22, 0 },
    { 832.01, 3.52, 0 },
    { 477.60, 10.44, 0 },   // Be-7 (вместо Sm-153: его 69.7/103 — внутри рентгеновского комплекса
                            // Pb/Bi/Th 75-100 кэВ, на NaI не отличить — ложный Sm-153 в фоне, #ISO-5)
};
static const iso_src_t SRC[] = {
    { "K-40", 0, 1, true },
    { "Ra-226", 1, 28, true },
    { "Th-232", 29, 33, true },
    { "U-238", 62, 3, true },
    { "U-235", 65, 6, true },
    { "Cs-137", 71, 1, false },
    { "Cs-134", 72, 8, false },
    { "Co-60", 80, 2, false },
    { "Am-241", 82, 1, false },
    { "Ba-133", 83, 7, false },
    { "Na-22", 90, 2, false },
    { "Eu-152", 92, 14, false },
    { "Eu-154", 106, 11, false },
    { "I-131", 117, 5, false },
    { "Tc-99m", 122, 1, false },
    { "Co-57", 123, 2, false },
    { "Mn-54", 125, 1, false },
    { "Lu-177", 126, 2, false },
    { "Y-88", 128, 2, false },
    { "Pb-210", 130, 1, true },
    { "Bi-207", 131, 3, false },
    { "La-140", 134, 10, false },
    { "Ce-139", 144, 1, false },
    { "Sb-125", 145, 9, false },
    { "Ga-67", 154, 6, false },
    { "In-111", 160, 2, false },
    { "I-123", 162, 2, false },
    { "Tl-201", 164, 4, false },
    { "Ra-223", 168, 14, false },
    { "Be-7", 182, 1, true },
};

#define NSRC        ((int)(sizeof SRC / sizeof SRC[0]))
#define MAX_PEAKS   128
#define MAX_BL      32       // бленд-линий на источник
#define FW_K        0.85     // FWHM, кэВ = FW_K * E^0.6: NaI ~6.3 % на 662, ~11 % на 186 (ширины
                             // пиков Ra-226 прибора пользователя; sqrt(E) завышал низ в 1.5 раза)
#define Z_FIND      3.0      // кандидат в пик
#define Z_FIND2     5.0      // второй проход (по остатку после вычитания найденных пиков)
#define Z_CHAR      4.0      // характерная линия
#define CONF_MIN    0.5
#define PB_MIN      0.04     // техногенный: характерная линия >= 4 % над подложкой (не неровность)
#define NAT_EXPL    0.3      // природная цепочка «объясняет» пик, если её ожидаемая площадь >= 30 % его
#define NAT_SHARE   0.15     // ... или её линия здесь заметна: вес >= 15 % характерной (в долгом фоне
                             // соотношения линий плывут: 1127 кэВ при Ra по 352 объяснялся лишь на 14 %)
#define SW          7        // сглаживание, каналов
#define HUMP_K      0.6      // #ISO-6: пик без кривизны подложки < 60 % отклика — горб подложки
#define LOW_KEV     120      // #ISO-6: ниже — горб подложки NaI и рентген
#define PB_LOW      0.2      // #ISO-6: опознание только по линиям ниже LOW_KEV — пик >= 20 % над подложкой,
#define SIG_LOW     25       //         если он слабее 25σ

typedef struct { float ch, kev, net, sig, res, pbr, wr; bool bs; } peak_t;   // pbr — нетто/подложка; wr — ширина/модель; bs — артефакт

int         isoid_count(void)      { return NSRC; }
const char *isoid_name(int s)      { return s >= 0 && s < NSRC ? SRC[s].name : "?"; }
bool        isoid_natural(int s)   { return s >= 0 && s < NSRC && SRC[s].natural; }
typedef struct eval_s eval_t;
static size_t ws_tail(void);
size_t      isoid_ws_size(int n)   { return (size_t)n * 5 * sizeof(float) + MAX_PEAKS * sizeof(peak_t) + ws_tail(); }

static double s_kw = 1.0;   // #ISO-3: множитель ширины на прогон (isoid_tune_t.kw)
static double fwhm_kev(double e) { return s_kw * FW_K * pow(e > 20 ? e : 20, 0.6); }

// Относительная эффективность пика полного поглощения NaI (точечный источник у торца):
// плато ~100-150 кэВ, выше ~E^-0.9, ниже 45 кэВ — поглощение в корпусе. Грубо: служит
// только для весов линий и «ждём ли эту линию».
static double eff(double e)
{
    if (e >= 150) return pow(150.0 / e, 0.9);
    if (e >= 45)  return 1.0;
    return (e / 45) * (e / 45);
}

static double ev(const double *c, int order, double ch)
{
    double e = 0, p = 1;
    for (int i = 0; i <= order; i++) { e += c[i] * p; p *= ch; }
    return e;
}

// Таблицы на прогон (isoid_run): энергия и FWHM в каналах по каждому каналу. double на ESP32-S3
// программный — пересчёт полинома и pow() в каждом цикле занимал ~1.5 с на прогон.
static const float *s_E, *s_W;
static int s_n;

static void tables(const double *c, int order, int n, float *E, float *W, const isoid_tune_t *t)
{
    for (int i = 0; i < n; i++) {
        double e = ev(c, order, i);
        E[i] = (float)(e * (t->ga + t->gb * e));   // #ISO-3: поправка шкалы
    }
    for (int i = 0; i < n; i++) {
        double sl = (E[i < n - 1 ? i + 1 : i] - E[i > 0 ? i - 1 : i]) / (i > 0 && i < n - 1 ? 2.0 : 1.0);
        if (sl < 1e-6) sl = 1e-6;
        double w = fwhm_kev(E[i]) / sl;
        W[i] = (float)(w < 3 ? 3 : w);
    }
    s_E = E; s_W = W; s_n = n;
}

static double e_at(double ch)   // энергия дробного канала — интерполяция таблицы
{
    if (ch <= 0) return s_E[0];
    if (ch >= s_n - 1) return s_E[s_n - 1];
    int i = (int)ch;
    return s_E[i] + (s_E[i + 1] - s_E[i]) * (ch - i);
}

// Канал энергии e (шкала монотонна) — двоичный поиск по таблице.
static double ch_of(const double *c, int order, double e, double lo, double hi)
{
    (void)c; (void)order; (void)lo; (void)hi;
    int a = 0, b = s_n - 1;
    if (e <= s_E[0]) return 0;
    if (e >= s_E[b]) return b;
    while (b - a > 1) { int m = (a + b) / 2; if (s_E[m] < e) a = m; else b = m; }
    return a + (e - s_E[a]) / (s_E[b] - s_E[a]);
}

static double fwhm_ch(const double *c, int order, double ch)
{
    (void)c; (void)order;
    int i = (int)(ch + 0.5);
    return s_W[i < 0 ? 0 : i >= s_n ? s_n - 1 : i];
}

// Площадь над линейным фоном в ±1 FWHM вокруг ch; фон — среднее по 5 каналов на ±1.4 FWHM.
// y — исходный спектр или остаток второго прохода; raw — исходные отсчёты (*tot — их сумма в
// окне, для дисперсии). false — окно не влезает. mu — центр тяжести нетто, wr — ширина/модель.
static bool area_at(const float *y, const uint32_t *raw, int n, double ch, double w, double *net, double *bg,
                    double *tot, double *mu, double *wr)
{
    int a = (int)(ch - w), b = (int)(ch + w), la = (int)(ch - 1.4 * w), rb = (int)(ch + 1.4 * w);
    if (la - 3 < 0 || rb + 3 >= n || b <= a) return false;
    double L = 0, R = 0;
    for (int i = -2; i <= 2; i++) { L += y[la + i]; R += y[rb + i]; }
    L /= 5; R /= 5;
    double s = 0, B = 0, T = 0, m1 = 0, m2 = 0, sw = 0;
    for (int i = a; i <= b; i++) {
        double base = L + (R - L) * (i - la) / (double)(rb - la);
        double v = y[i] - base;
        s += v;
        B += base;
        T += raw[i];
        if (v > 0) { m1 += v * i; sw += v; }
    }
    if (sw <= 0) return false;
    double c = m1 / sw;
    for (int i = a; i <= b; i++) {
        double base = L + (R - L) * (i - la) / (double)(rb - la);
        double v = y[i] - base;
        if (v > 0) m2 += v * (i - c) * (i - c);
    }
    *net = s;
    *bg = B > 0 ? B : 0;
    *tot = T;
    *mu = c;
    (void)m2;
    // #ISO-3: ширина — по полувысоте (FWHM) нетто, сглаженного по ±w/10 каналам: не зависит от
    // окна (второй момент в окне ±1 модельной FWHM рос вместе с моделью — обратная связь).
    int h = (int)(w / 10); if (h < 1) h = 1;
    double vmax = 0; int im = (int)(c + 0.5);
    for (int i = a + h; i <= b - h; i++) {
        double v = 0;
        for (int j = -h; j <= h; j++) v += y[i + j] - (L + (R - L) * (i + j - la) / (double)(rb - la));
        v /= 2 * h + 1;
        if (v > vmax) { vmax = v; im = i; }
    }
    double xl = -1, xr = -1;
    for (int dir = -1; dir <= 1; dir += 2) {
        double prev = vmax;
        for (int i = im + dir; i >= la && i <= rb; i += dir) {
            double v = 0; int k = 0;
            for (int j = -h; j <= h; j++) if (i + j >= 0 && i + j < n) { v += y[i + j] - (L + (R - L) * (i + j - la) / (double)(rb - la)); k++; }
            v /= k;
            if (v <= vmax / 2) {
                double x = i - dir * (vmax / 2 - v) / (prev - v + 1e-9);   // интерполяция
                if (dir < 0) xl = x; else xr = x;
                break;
            }
            prev = v;
        }
    }
    *wr = (xl >= 0 && xr >= 0 && vmax > 0) ? (xr - xl) / w : 1.0;
    return true;
}

static void mark_art(peak_t *pk, int np)
{
    // Артефакты сильного пика p — не характерная линия (флаг bs):
    //  - обратное рассеяние E/(1+2E/511) (горб ~150-250 кэВ; рассеяние не строго назад —
    //    горб сдвинут вверх: у Cs-137 теория 184, на спектре 200 — окно до +1.2 FWHM);
    //  - «пик» перед краем Комптона Ec = E - E/(1+2E/511): «цилиндр» видит ступеньку вниз
    //    как пик за 0..1.5 FWHM до края (KCl: край K-40 1243, «пик» 1170-1180);
    //  - пики одиночного/двойного вылета E-511, E-1022 (E > 1.5 МэВ).
    for (int q = 0; q < np; q++) {
        double eq = pk[q].kev, tq = 0.4 * fwhm_kev(eq) + 3;
        for (int p = 0; p < np && !pk[q].bs; p++) {
            double e = pk[p].kev;
            if (p == q || pk[q].net > 0.5 * pk[p].net) continue;
            double eb = e / (1 + 2 * e / 511.0), ec = e - eb, fc = fwhm_kev(ec);
            // горб обратного рассеяния на спектрах — 2-16 % пика-источника (Cs-137 2 %, Ra 352 -> 146: 12 %);
            // не больше 30 %, иначе под флаг попадала 238 Pb-212 (42 % от K-40 1460 в фоне)
            bool art = (e >= 250 && eq >= 100 && eq <= 260 && eq >= eb - tq && eq <= eb + 1.2 * fwhm_kev(eb) + 3 &&
                        pk[q].net <= 0.3 * pk[p].net) ||
                       (e >= 400 && eq >= ec - 1.5 * fc && eq <= ec + 0.3 * fc) ||
                       (e >= 1500 && pk[q].net <= 0.15 * pk[p].net &&   // вылет у NaI — единицы %
                        (fabs(eq - (e - 511)) <= tq || fabs(eq - (e - 1022)) <= tq));
            if (art && !pk[q].bs) DBG("  art   %7.1f keV <- %7.1f keV\n", eq, e);
            if (art) pk[q].bs = true;
        }
    }
}

// Один проход поиска по y (исходный или остаток) — пики дописываются к pk[0..np).
static int find_pass(const float *y, const uint32_t *raw, int n, const double *c, int order, float *s, float *z,
                     peak_t *pk, int np, int pass)
{
    int np0 = np;
    double zmin = pass ? Z_FIND2 : Z_FIND;
    double acc = 0;
    for (int i = 0; i < n; i++) {
        acc += y[i];
        if (i >= SW) acc -= y[i - SW];
        s[i] = (float)(acc / (i + 1 < SW ? i + 1 : SW));
    }
    for (int i = 0; i < n - SW / 2; i++) s[i] = s[i + SW / 2];
    int lo = 8;
    while (lo < n && s_E[lo] < 25) lo++;
    for (int i = 0; i < n; i++) {
        z[i] = 0;
        if (i < lo) continue;
        int w = (int)fwhm_ch(c, order, i);
        if (i - w < 0 || i + w >= n) continue;
        double side = 0.5 * (s[i - w] + s[i + w]);
        double net = s[i] - side;
        double noise = sqrt((s[i] + 0.5 * side) / SW + 1.0);
        z[i] = (float)(net > 0 ? net / noise : 0);
    }
    for (int i = lo; i < n - 8 && np < MAX_PEAKS; i++) {
        if (z[i] < zmin) continue;
        int h = (int)(fwhm_ch(c, order, i) / 2);
        bool top = true;
        for (int j = i - h; j <= i + h && top; j++)
            if (j >= 0 && j < n && j != i && (z[j] > z[i] || (z[j] == z[i] && j < i))) top = false;
        if (!top) continue;
        double w = fwhm_ch(c, order, i), net, bg, tot, mu, wr;
        // #ISO-6: «цилиндр» отвечает и на выпуклую подложку (горб фона NaI 40-80 кэВ при большом наборе
        // давал «пики» -> U-238 63, Am-241 59.5, Pb-210 46.5 на спектре без пиков). Отклик на ±1 и ±2 FWHM:
        // у гауссианы n1 = 0.94A, n2 = A; у параболы n2 = 4·n1 -> A = (4·n1 - n2)/2.75 без кривизны.
        if (!pass) {
            int w1 = (int)w, w2 = 2 * w1;
            if (i - w2 >= 0 && i + w2 < n) {
                double n1 = s[i] - 0.5 * (s[i - w1] + s[i + w1]);
                double n2 = s[i] - 0.5 * (s[i - w2] + s[i + w2]);
                double A = (4 * n1 - n2) / 2.75;
                if (A < HUMP_K * n1) {
                    DBG("  hump  ch %6d  %7.1f keV  z %5.1f  A/n1 %.2f\n", i, s_E[i], z[i], n1 > 0 ? A / n1 : 0);
                    continue;
                }
            }
        }
        // Второй проход ищет только «плечо» сильного пика: в 0.8..2.2 FWHM от найденного в
        // первом. Дальше первый проход уже всё видел, а остатки вычитания у очень сильных пиков
        // (форма NaI не чисто гауссова) иначе рождают ложные пики.
        int nb = -1;
        bool near = false;
        for (int k = 0; k < np0 && !near; k++) {
            double d = fabs(pk[k].ch - i);
            near = d < 0.8 * w;
            if (d <= 2.2 * w && !pk[k].bs && (nb < 0 || pk[k].net > pk[nb].net)) nb = k;
        }
        if (near || (pass && nb < 0)) continue;
        // ниже 120 кэВ — рентгеновские комплексы (Kα+Kβ Pb/Bi ~72-90 кэВ): не гауссовы, вычитание
        // оставляет «плечи» (66 и 95 кэВ у Cs/Co пользователя -> ложные U-238 63, Sm-153 103)
        if (pass && s_E[i] < 120) continue;
        if (!area_at(y, raw, n, i, w, &net, &bg, &tot, &mu, &wr)) continue;
        double sg = net > 0 ? net / sqrt(tot + bg + 1) : 0;   // дисперсия: все отсчёты окна + оценка фона
        if (sg < zmin) continue;
        if (pass && (net < 0.05 * pk[nb].net || net > 1.5 * pk[nb].net || (bg > 0 && net / bg < 0.05))) continue;
        if (wr > (pass ? 1.5 : 1.7)) {   // горб шире пика (обратное рассеяние, комптоновский край)
            DBG("  wide  ch %6.1f  %7.1f keV  sig %5.1f  w/model %.2f\n", mu, e_at(mu), sg, wr);
            continue;
        }
        pk[np].ch = (float)mu;
        pk[np].kev = (float)e_at(mu);
        pk[np].net = pk[np].res = (float)net;
        pk[np].sig = (float)sg;
        pk[np].pbr = (float)(bg > 0 ? net / bg : 99);
        pk[np].wr = (float)wr;
        pk[np].bs = false;
        DBG("  peak%c ch %6.1f  %7.1f keV  net %9.0f  sig %6.1f  p/b %.3f  w/model %.2f\n", pass ? '2' : ' ', mu,
            pk[np].kev, net, sg, pk[np].pbr, wr);
        np++;
    }
    return np;
}

// Пики в два прохода: 2-й ищет по остатку после вычитания гауссиан найденных — так виден
// пик в ~1 FWHM от сильного соседа (Cs-137 662 у Bi-214 609, Co-60 1173 у Bi-214 1120),
// которого «цилиндр» не видит: его боковое окно лежит на склоне соседа.
static int find_peaks(const uint32_t *raw, int n, const double *c, int order, float *ws, peak_t *pk)
{
    float *y = ws + 2 * n;
    for (int i = 0; i < n; i++) y[i] = (float)raw[i];
    int np = find_pass(y, raw, n, c, order, ws, ws + n, pk, 0, 0);
    mark_art(pk, np);   // 2-й проход не ищет «плечо» у артефакта (край Комптона и т.п.)
    for (int k = 0; k < np; k++) {
        double wr = pk[k].wr < 0.6 ? 0.6 : pk[k].wr > 1.3 ? 1.3 : pk[k].wr;
        double sg = fwhm_ch(c, order, pk[k].ch) * wr / 2.355, A = pk[k].net / 0.98 / (sg * 2.5066);
        int a = (int)(pk[k].ch - 3 * sg), b = (int)(pk[k].ch + 3 * sg);
        for (int i = a < 0 ? 0 : a; i <= b && i < n; i++) {
            double d = (i - pk[k].ch) / sg;
            y[i] -= (float)(A * exp(-0.5 * d * d));
        }
    }
    np = find_pass(y, raw, n, c, order, ws, ws + n, pk, np, 1);
    mark_art(pk, np);
    return np;
}

typedef struct { double kev, w; bool xray, need; } bl_t;   // need — обязательная опора (511 Na-22)

// Бленды линий источника s (ближе 1 FWHM — NaI их не разделяет) в [emin, emax]. Возвращает число.
// #ISO-4: было 1.2 — Ac-228 911 и 969 (1.05 FWHM) сливались в бленд ~935, а NaI показывает пик
// почти чистой 911 (903 у прибора пользователя), и торий его не узнавал (ложный Y-88 898 в бетоне).
static int blends(int s, double emin, double emax, bl_t *b)
{
    int nb = 0;
    for (int k = 0; k < SRC[s].n; k++) {
        const iso_line_t *l = &LINES[SRC[s].first + k];
        if (l->kev < emin || l->kev > emax) continue;
        double w = l->inten * eff(l->kev);
        // техногенные: 70-95 кэВ — область рентгена Pb (защита), Bi/Po/Th (ряды U/Th) — только опора
        unsigned char xr = l->xray || (!SRC[s].natural && l->kev >= 70 && l->kev <= 95);
        if (nb > 0 && l->kev - b[nb - 1].kev < 1.0 * fwhm_kev(l->kev) && xr == b[nb - 1].xray) {
            double W = b[nb - 1].w + w;
            b[nb - 1].kev = (b[nb - 1].kev * b[nb - 1].w + l->kev * w) / W;
            b[nb - 1].w = W;
            continue;
        }
        if (nb >= MAX_BL) break;
        b[nb].kev = l->kev;
        b[nb].w = w;
        b[nb].xray = xr;
        b[nb].need = l->xray == 2;
        nb++;
    }
    return nb;
}

// Пик для линии: самый значимый в допуске (ближайший мог бы оказаться слабым соседним).
static int nearest(const peak_t *pk, int np, double e, double tol)
{
    int best = -1;
    for (int i = 0; i < np; i++)
        if (fabs(pk[i].kev - e) <= tol && (best < 0 || pk[i].sig > pk[best].sig)) best = i;
    return best;
}

// Природные цепочки, чья характерная линия видна (по исходным площадям), и их масштаб — для
// проверки, что пик техногенного нуклида не объясняется линией природной цепочки при её
// найденной интенсивности (Zn-65 1115 / Bi-214 1120, Ba-133 356 / Pb-214 352, Cs-134 796 /
// Ac-228 795; а Cs-137 662 рядом со слабой Bi-214 665 — вес 4 %, объясняется на ~15 %).
static double *s_nat_S;   // [64], 0 — цепочки не видно; лежит в рабочем буфере (PSRAM на плате)
// #ISO-4: локальные масштабы цепочки — по каждой её найденной заметной линии (энергия, площадь/вес).
// Модель эффективности одна на весь диапазон, и масштабы по разным линиям расходятся в разы (Th в
// бетоне: по 238 — 60, по 583 — 160, по 2614 — 375); для проверки пика берётся ближайшая по энергии.
#define NAT_PTS 16
// Найденная гамма-линия природной цепочки: энергия, пик, вес, локальный масштаб, «заметная».
typedef struct { float e, S, w; short p; bool prom; } natpt_t;
static natpt_t (*s_nat_pt)[NAT_PTS];
static int *s_nat_np;

static bool overlaps_natural(int pp, double e, double net)
{
    for (int s = 0; s < NSRC && s < 64; s++) {
        if (!SRC[s].natural || s_nat_S[s] <= 0) continue;
        double wc = 0, S = s_nat_S[s], de = 1e9;
        for (int k = 0; k < s_nat_np[s]; k++) {
            const natpt_t *q = &s_nat_pt[s][k];
            if (q->w > wc) wc = q->w;
            // ближайшая заметная — на ДРУГОМ пике (на этом же пик объяснял бы сам себя: Am-241 59.5,
            // слившийся с 53 радия)
            if (q->prom && q->p != pp && fabs(q->e - e) < de) { de = fabs(q->e - e); S = q->S; }
        }
        for (int k = 0; k < s_nat_np[s]; k++) {
            const natpt_t *q = &s_nat_pt[s][k];
            if (q->p == pp && (S * q->w >= NAT_EXPL * net || q->w >= NAT_SHARE * wc)) {
                DBG("    overlap: %.1f keV peak is %s %.1f (S %.0f w %.2f -> %.0f of %.0f, share %.2f)\n", e,
                    SRC[s].name, q->e, S, q->w, S * q->w, net, wc > 0 ? q->w / wc : 0);
                return true;
            }
        }
    }
    return false;
}

struct eval_s {
    bool   ok;
    double conf, sig, S, zsum;   // zsum — сумма значимостей найденных гамма-линий
    int    nb;
    bl_t   b[MAX_BL];
    int    pk[MAX_BL];   // пик бленда, -1 — не найден
};

// Хвост рабочего буфера: две оценки и масштабы природных цепочек (не .bss — внутренняя RAM).
typedef struct { eval_t E, best; double nat_S[64]; natpt_t nat_pt[64][NAT_PTS]; int nat_np[64]; } tail_t;
static size_t ws_tail(void) { return sizeof(tail_t) + 16; }

// Оценка источника s по остаточным площадям пиков.
static void evaluate(int s, const uint32_t *y, int n, const double *c, int order,
                     const peak_t *pk, int np, double emin, double emax, eval_t *E)
{
    memset(E, 0, sizeof *E);
    E->nb = blends(s, emin, emax, E->b);
    int ch_i = -1;
    for (int i = 0; i < E->nb; i++)
        if (!E->b[i].xray && (ch_i < 0 || E->b[i].w > E->b[ch_i].w)) ch_i = i;
    if (ch_i < 0) return;
    for (int i = 0; i < E->nb; i++) {
        // + запас на погрешность калибровки выше 500 кэВ (2 % от превышения): у прибора
        // пользователя Co-60 стоял на 1147/1311 вместо 1173/1332 (сжатие шкалы вверху) и не
        // находился, а 1147 «цеплял» Zn-65 1115. Ровные 1.5 % на всех энергиях отдавали пик
        // Am-241 59.5 линии U-238 63.3 и плодили Cs-134.
        double ek = E->b[i].kev, tol = 0.4 * fwhm_kev(ek) + 2 + (ek > 500 ? 0.02 * (ek - 500) : 0);
        E->pk[i] = nearest(pk, np, E->b[i].kev, tol);
    }
    int pc = E->pk[ch_i];
    if (pc < 0) return;
    if (pk[pc].bs) {
        // Природной цепочке пик-«артефакт» годится, если у неё есть ещё >= 2 сильные линии:
        // 238 Pb-212 в фоне попадает в окно обратного рассеяния K-40 1460, а торий там очевиден
        // по 583 и 2614. Техногенным — нет (Lu-177/Tc-99m на горбе Cs-137, U-235 на 184-200).
        int strong = 0;
        for (int i = 0; i < E->nb && SRC[s].natural; i++) {
            int p = E->pk[i];
            if (i == ch_i || E->b[i].xray || p < 0 || pk[p].bs) continue;
            strong += pk[p].sig >= Z_CHAR;
        }
        if (strong < 2) return;
    }
    double sig_c = pk[pc].res / (pk[pc].net / pk[pc].sig);   // остаток / σ площади
    if (sig_c < Z_CHAR) return;
    if (!SRC[s].natural && pk[pc].pbr < PB_MIN) return;      // неровность подложки, а не пик
    if (E->b[ch_i].kev < LOW_KEV && pk[pc].pbr < PB_LOW && pk[pc].sig < SIG_LOW) {
        // #ISO-6: опознание держится только на линиях ниже 120 кэВ (Am-241 59.5, Pb-210 46.5, U-238
        // 63.3) — там горб подложки NaI; настоящие такие пики стоят над подложкой на 0.3-0.4, горб — 0.05-0.13.
        // Сильнее SIG_LOW горб уже отсекла проверка кривизны (HUMP_K), а Pb-210 свинцовой защиты — 0.13 при 41σ
        bool hi = false;
        for (int i = 0; i < E->nb && !hi; i++) {
            int p = E->pk[i];
            hi = !E->b[i].xray && p >= 0 && !pk[p].bs && E->b[i].kev >= LOW_KEV && pk[p].sig >= Z_CHAR;
        }
        if (!hi) return;
    }
    double S = pk[pc].res / E->b[ch_i].w;
    double found = 0, expect = 0, zsum = 0;
    bool strong_miss = false;
    for (int i = 0; i < E->nb; i++) {
        if (E->b[i].need && E->pk[i] < 0) {   // обязательная опора (511 Na-22) не найдена
            double ch = ch_of(c, order, E->b[i].kev, 0, n - 1), w = fwhm_ch(c, order, ch), B = 0;
            int a = (int)(ch - w), bb = (int)(ch + w);
            if (a >= 0 && bb < n) {
                for (int k = a; k <= bb; k++) B += y[k];
                if (S * E->b[i].w / sqrt(2 * B + 1) >= 3) return;   // была бы видна — её нет
            }
        }
        if (E->b[i].xray) continue;   // рентген — только опора (см. LINES)
        double X = S * E->b[i].w, sgm;
        int p = E->pk[i];
        if (p >= 0) {
            sgm = pk[p].net / pk[p].sig;
        } else {   // σ площади, если бы пик был: фон в ±1 FWHM
            double ch = ch_of(c, order, E->b[i].kev, 0, n - 1);
            double w = fwhm_ch(c, order, ch), B = 0;
            int a = (int)(ch - w), bb = (int)(ch + w);
            if (a < 0 || bb >= n) continue;
            for (int k = a; k <= bb; k++) B += y[k];
            sgm = sqrt(2 * B + 1);
        }
        double xs = X / sgm;          // ожидаемая значимость
        if (xs < 3 && p < 0) continue;   // не видна бы и так — не учитываем
        // #ISO-5: у техногенного нет линии, которая при этом масштабе была бы видна >= 6 сигм, —
        // это не он (Y-88 без 1836 на Ac-228 911 бетона: 1836 ждали ~400 отсчётов почти без подложки)
        if (p < 0 && xs >= 6 && !SRC[s].natural) strong_miss = true;
        expect += E->b[i].w;
        if (p >= 0 && pk[p].res > 0) {
            double fr = pk[p].res / X;
            found += E->b[i].w * (fr < 1 ? fr : 1);
            double zr = pk[p].res / sgm;   // значимость остатка, но не больше ожидаемой
            zsum += zr < xs ? zr : xs;
        }
    }
    E->conf = expect > 0 ? found / expect : 0;
    if (!SRC[s].natural) {   // нужна хоть одна значимая линия вне заметных природных
        bool indep = false;
        for (int i = 0; i < E->nb && !indep; i++) {
            int p = E->pk[i];
            // обязательная опора (511 Na-22) — тоже независимое подтверждение: у радия её нет
            if ((E->b[i].xray && !E->b[i].need) || p < 0 || pk[p].bs) continue;
            if (pk[p].res / (pk[p].net / pk[p].sig) < Z_CHAR) continue;
            // линия должна заметно объяснять пик (>= 30 %): слабая 1168 Cs-134 (1.8 %) на пике
            // Co-60 1173 не подтверждение
            if (S * E->b[i].w < 0.3 * pk[p].net) continue;
            indep = !overlaps_natural(p, E->b[i].kev, pk[p].net);
        }
        if (!indep) { E->sig = sig_c; return; }   // ok = false: не отличить от природной линии
    }
    E->sig = sig_c;
    E->zsum = zsum;
    E->S = S;
    E->ok = E->conf >= CONF_MIN && !strong_miss;
}

void isoid_tune_default(isoid_tune_t *t)
{
    t->kw = 1.0f; t->ga = 1.0f; t->gb = 0.0f; t->n_w = t->n_g = 0;
}

#define GAIN_MAX  0.08   // поправка шкалы не больше ±8 % на 3 МэВ (дальше — пусть калибруют)
#define MAX_GP    32

int isoid_run(const uint32_t *bins, int n, const double *cal, int order, void *ws,
              const isoid_tune_t *tin, isoid_tune_t *tout, isoid_result_t *out)
{
    memset(out, 0, sizeof *out);
    isoid_tune_t tdef;
    if (!tin) { isoid_tune_default(&tdef); tin = &tdef; }
    if (tout) { *tout = *tin; tout->n_w = tout->n_g = 0; }
    s_kw = tin->kw > 0.3f && tin->kw < 3.0f ? tin->kw : 1.0;
    if (!cal || order < 1 || n < 256) return -1;
    double e0 = ev(cal, order, 0), e1 = ev(cal, order, n - 1), em = ev(cal, order, n / 2);
    if (!(e1 > em && em > e0) || e1 < 200) return -1;   // шкала не возрастает — калибровки нет
    float *f = (float *)ws;
    tables(cal, order, n, f + 3 * n, f + 4 * n, tin);
    peak_t *pk = (peak_t *)(f + 5 * n);   // f[0..5n): сглаженный, «цилиндр», остаток 2-го прохода, E, FWHM
    uintptr_t tp = ((uintptr_t)(pk + MAX_PEAKS) + 15) & ~(uintptr_t)15;
    tail_t *T = (tail_t *)tp;
    s_nat_S = T->nat_S;
    s_nat_pt = T->nat_pt;
    s_nat_np = T->nat_np;
    int np = find_peaks(bins, n, cal, order, f, pk);
    out->n_peaks = np;
    // #ISO-3: разрешение — медиана «ширина/модель» сильных уединённых пиков 250-1300 кэВ: ниже —
    // рентген-комплексы (шире гауссианы), выше — бленды и сжатие шкалы (Ra 1764/2204 у прибора
    // пользователя на 20-24 % шире модели, а 352-1120 — в пределах 4 %).
    {
        float wr[MAX_PEAKS];
        int nw = 0;
        for (int i = 0; i < np; i++) {
            if (pk[i].sig < 20 || pk[i].bs || pk[i].kev < 250 || pk[i].kev > 1300 || pk[i].wr < 0.3f) continue;
            double w = fwhm_ch(cal, order, pk[i].ch);
            bool iso = true;
            for (int j = 0; j < np && iso; j++) iso = j == i || fabs(pk[j].ch - pk[i].ch) > 2.5 * w;
            if (iso) wr[nw++] = pk[i].wr;
        }
        for (int i = 1; i < nw; i++)   // сортировка вставками (nw мало)
            for (int j = i; j > 0 && wr[j] < wr[j - 1]; j--) { float t = wr[j]; wr[j] = wr[j - 1]; wr[j - 1] = t; }
        if (tout && nw >= 2) {
            double r = wr[nw / 2];
            if (r > 1.15) r = 1.15;   // шагом не больше 15 % за прогон
            if (r < 0.87) r = 0.87;
            double k = s_kw * r;
            tout->kw = (float)(k < 0.5 ? 0.5 : k > 2.0 ? 2.0 : k);
            tout->n_w = nw;
        }
        DBG("  tune: kw %.3f (in %.3f, %d peaks)\n", tout ? tout->kw : s_kw, s_kw, nw);
    }
    double emin = s_E[40], emax = s_E[n - 40];
    bool used[64] = { false };
    double gx[MAX_GP], gq[MAX_GP], gw[MAX_GP];
    int ngp = 0;
#define E    (T->E)      // ~1 КБ каждая — в рабочем буфере, не на стеке задачи
#define best (T->best)
    for (int s = 0; s < NSRC && s < 64; s++) {
        s_nat_S[s] = 0;
        s_nat_np[s] = 0;
        if (!SRC[s].natural) continue;
        evaluate(s, bins, n, cal, order, pk, np, emin, emax, &E);
        {   // #ISO-4: найденные гамма-линии цепочки — пик, вес, локальный масштаб
            double wc0 = 0;
            for (int i = 0; i < E.nb; i++) if (!E.b[i].xray && E.b[i].w > wc0) wc0 = E.b[i].w;
            for (int i = 0; i < E.nb && s_nat_np[s] < NAT_PTS; i++) {
                int p = E.pk[i];
                if (E.b[i].xray || p < 0) continue;
                natpt_t *q = &s_nat_pt[s][s_nat_np[s]++];
                q->e = (float)E.b[i].kev;
                q->w = (float)E.b[i].w;
                q->S = (float)(pk[p].net / E.b[i].w);
                q->p = (short)p;
                q->prom = !pk[p].bs && pk[p].sig >= Z_CHAR && E.b[i].w >= 0.05 * wc0;
            }
        }
    }
    // #ISO-4: какие природные цепочки присутствуют — предварительный жадный проход только по ним, с
    // вычитанием (как основной). «Характерная видна» обманывалась чужими пиками: 238 тория стоит на
    // 242 Pb-214 радия, и «торий» в чистом Ra отнимал у Cs-137 его 662. Площади потом восстанавливаются.
    {
        float res0[MAX_PEAKS];
        bool nused[64] = { false };
        for (int k = 0; k < np; k++) res0[k] = pk[k].res;
        for (;;) {
            int bs = -1;
            double bscore = 0, bS = 0;
            for (int s = 0; s < NSRC && s < 64; s++) {
                if (!SRC[s].natural || nused[s]) continue;
                evaluate(s, bins, n, cal, order, pk, np, emin, emax, &E);
                if (!E.ok) continue;
                double score = E.zsum * E.conf;
                if (score > bscore) { bscore = score; bs = s; bS = E.S; }
            }
            if (bs < 0) break;
            nused[bs] = true;
            s_nat_S[bs] = bS;
            evaluate(bs, bins, n, cal, order, pk, np, emin, emax, &E);
            for (int i = 0; i < E.nb; i++) {
                int p = E.pk[i];
                if (p < 0) continue;
                pk[p].res -= (float)(E.S * E.b[i].w);
                if (pk[p].res < 0) pk[p].res = 0;
            }
        }
        for (int k = 0; k < np; k++) pk[k].res = res0[k];
    }
    // Характерная не видна (238 тория под ураном), но видны >= 2 другие заметные линии (583 и 2614
    // тория гранитных стен) — цепочка тоже присутствует; масштаб — по самой сильной из них. Иначе 583
    // Tl-208 при шкале -3 % отдавалась Bi-207 570. Засчитываются только пики, не занятые цепочкой с
    // видимой характерной: в чистом Ra линии тория 338/583 стоят на пиках радия 352/609 — «торий»
    // завышенного масштаба отнимал у Cs-137 его 662 (через Ac-228 675).
    for (int s = 0; s < NSRC && s < 64; s++) {
        if (!SRC[s].natural || s_nat_S[s] > 0) continue;
        double Sm = 0;
        int seen = 0;
        for (int k = 0; k < s_nat_np[s]; k++) {
            const natpt_t *q = &s_nat_pt[s][k];
            if (!q->prom) continue;
            bool taken = false;
            for (int o = 0; o < NSRC && o < 64 && !taken; o++) {
                if (o == s || !SRC[o].natural || s_nat_S[o] <= 0) continue;
                for (int j = 0; j < s_nat_np[o] && !taken; j++) taken = s_nat_pt[o][j].p == q->p;
            }
            if (taken) continue;
            seen++;
            if (q->S > Sm) Sm = q->S;
        }
        if (seen >= 2) s_nat_S[s] = Sm;
    }
    while (out->n < ISOID_MAX_FOUND) {
        int bs = -1;
        double bscore = 0;
        for (int s = 0; s < NSRC && s < 64; s++) {
            if (used[s]) continue;
            evaluate(s, bins, n, cal, order, pk, np, emin, emax, &E);
            if (E.sig > 0) DBG("    %-7s conf %.2f sig %6.1f%s\n", SRC[s].name, E.conf, E.sig, E.ok ? "  ok" : "");
            if (!E.ok) continue;
            double score = E.zsum * E.conf;   // больше подтверждённых линий — раньше
            if (score > bscore) { bscore = score; bs = s; best = E; }
        }
        if (bs < 0) break;
        used[bs] = true;
        isoid_found_t *F = &out->f[out->n++];
        F->src = bs;
        F->natural = SRC[bs].natural;
        F->conf = (float)best.conf;
        F->sig = (float)best.sig;
        {   // #ISO-7: качество — по сумме значимостей найденных линий (4σ ~30 %, 8σ ~80 %, 12σ ~95 %),
            // неполный набор линий (conf < 0.8) снижает до 20 %: у рядов слабые линии тонут в фоне
            double qz = 1 - exp(-(best.zsum - 2.9) / 3), cn = (best.conf - 0.5) / 0.3;
            if (qz < 0) qz = 0;
            if (cn < 0) cn = 0; else if (cn > 1) cn = 1;
            F->quality = (int)(100 * qz * (0.8 + 0.2 * cn) + 0.5);
        }
        DBG("  => %s (conf %.2f, sig %.1f, zsum %.1f, q %d)\n", SRC[bs].name, best.conf, best.sig, best.zsum, F->quality);
        // #ISO-3: точки шкалы — гамма-линии уверенно найденного источника (линия / центр пика)
        for (int i = 0; i < best.nb && best.conf >= 0.7 && ngp < MAX_GP; i++) {
            int p = best.pk[i];
            if (best.b[i].xray || p < 0 || pk[p].bs || pk[p].sig < 8 || pk[p].kev < 150) continue;
            double sg = pk[p].sig < 50 ? pk[p].sig : 50;
            gx[ngp] = pk[p].kev; gq[ngp] = best.b[i].kev / pk[p].kev; gw[ngp] = sg * sg; ngp++;
        }
        // Отметки: найденные пики источника по убыванию веса; вычесть ожидаемое.
        for (int k = 0; k < ISOID_MAX_MARK; k++) {
            int bi = -1;
            for (int i = 0; i < best.nb; i++)
                if (best.pk[i] >= 0 && (bi < 0 || best.b[i].w > best.b[bi].w)) bi = i;
            if (bi < 0) break;
            int p = best.pk[bi];
            bool dup = false;
            for (int j = 0; j < F->n_mark; j++) if (F->mark_ch[j] == pk[p].ch) dup = true;
            if (!dup) {
                F->mark_ch[F->n_mark] = pk[p].ch;
                F->mark_kev[F->n_mark] = pk[p].kev;
                F->n_mark++;
            }
            best.pk[bi] = -1;
            k -= dup;
        }
        evaluate(bs, bins, n, cal, order, pk, np, emin, emax, &E);   // заново: pk[] у best испорчены
        // Вычитается полная ожидаемая площадь. Пробовал снимать слабыми линиями (< 15 % характерной)
        // не больше половины пика (слабый Na-22 1274 рядом с Bi-214 1238/1281 сильного Ra) — остатки
        // слабых линий Ra/Th дали ложные Y-88, Eu-152, Cs-134; откатил. Слабый Na-22 на сильном Ra —
        // ограничение NaI.
        for (int i = 0; i < E.nb; i++) {
            int p = E.pk[i];
            if (p < 0) continue;
            pk[p].res -= (float)(E.S * E.b[i].w);
            if (pk[p].res < 0) pk[p].res = 0;
        }
    }
    // #ISO-3: поправка шкалы q(E) = 1 + b*E (взвешенный МНК через ноль): сдвиг у прибора
    // пользователя растёт с энергией (609 -1.1 %, 1120 -2 %, 1764 -3.7 %), а единственная точка
    // K-40 множителем двигала и рентген 70-95 кэВ (ложный Sm-153). Складывается с текущей,
    // зажата в ±GAIN_MAX на 3000 кэВ.
    if (tout && ngp >= 1) {
        double XX = 0, XQ = 0;
        for (int i = 0; i < ngp; i++) { XX += gw[i] * gx[i] * gx[i]; XQ += gw[i] * gx[i] * (gq[i] - 1); }
        double b = XX > 0 ? XQ / XX : 0;
        double gb = tin->gb + b * (1 + tin->gb * 1460);   // G(E) = 1 + gb*E; новая точка поверх старой
        if (gb * 3000 > GAIN_MAX) gb = GAIN_MAX / 3000;
        if (gb * 3000 < -GAIN_MAX) gb = -GAIN_MAX / 3000;
        tout->ga = 1.0f;
        tout->gb = (float)gb;
        tout->n_g = ngp;
        DBG("  tune: gain x%.4f @662, x%.4f @1460, x%.4f @2614 (%d lines)\n", tout->ga + tout->gb * 662,
            tout->ga + tout->gb * 1460, tout->ga + tout->gb * 2614, ngp);
    }
    return out->n;
#undef E
#undef best
}
