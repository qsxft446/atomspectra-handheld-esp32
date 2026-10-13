// #ACAL-1: host-тест автокалибровки по Ra-226 (main/autocal.c) на реальных спектрах
// прибора пользователя (tests/host/data/ra226_*.txt, 2026-10-14).
// Эталон — калибровка, подобранная вручную по 12 линиям и записанная на плату:
// 7.76253299801587 + 0.267995214878602 ch + 8.07264835375737e-05 ch^2
//   - 3.28147308346862e-08 ch^3 + 4.60405912953082e-12 ch^4.
// Сборка: make autocal (gcc) или MSVC: cl /W4 /I ..\..\main test_autocal.c ..\..\main\autocal.c
// Запускать из tests/host (пути к data/ относительные). Ждём "AUTOCAL: ALL PASSED".
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "autocal.h"

int g_failures;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_failures++; } } while (0)

#define N 8192
static const double REF[5] = { 7.76253299801587, 0.267995214878602, 8.07264835375737e-05,
                               -3.28147308346862e-08, 4.60405912953082e-12 };
static float ws[2 * N];

static int load(const char *path, uint32_t *bins)
{
    FILE *f = fopen(path, "r");
    if (!f) { printf("cannot open %s\n", path); return 0; }
    char line[256];
    int n = 0;
    while (n < N && fgets(line, sizeof line, f)) {
        if (line[0] == '#') continue;
        bins[n++] = (uint32_t)strtoul(line, NULL, 10);
    }
    fclose(f);
    return n;
}

static void print_result(const char *title, const autocal_result_t *r)
{
    printf("  %s: err %d, %d lines, order %d, rms %.2f keV, max %.2f keV, net609 %.0f, net1764 %.0f, enough %d\n",
           title, r->err, r->n, r->order, r->rms_kev, r->max_abs_kev, r->net609, r->net_top, autocal_enough(r));
    for (int i = 0; i < r->n; i++)
        printf("     %-14s ch %7.1f  %7.1f keV -> %7.1f (%+.1f)\n", r->name[i], r->ch[i], r->kev[i], r->fit_kev[i],
               r->fit_kev[i] - r->kev[i]);
}

static bool has_line(const autocal_result_t *r, double kev)
{
    for (int i = 0; i < r->n; i++) if (fabs(r->kev[i] - kev) < 0.5) return true;
    return false;
}

// Наибольшее расхождение найденной калибровки с эталоном (каналы масштабированы на s).
static double max_dev_vs_ref(const autocal_result_t *r, double s, double ch_lo, double ch_hi)
{
    double mx = 0;
    for (double ch = ch_lo; ch <= ch_hi; ch += 10) {
        double d = fabs(autocal_eval(r->coeffs, r->order, ch * s) - autocal_eval(REF, 4, ch));
        if (d > mx) mx = d;
    }
    return mx;
}

// Спектр, растянутый по каналам в s раз (имитация смены усиления/напряжения ФЭУ).
static void rescale(const uint32_t *in, uint32_t *out, double s)
{
    memset(out, 0, N * sizeof *out);
    for (int i = 0; i < N; i++) {
        double a = i * s, b = (i + 1) * s;   // канал i -> [a, b)
        for (int j = (int)a; j < (int)ceil(b) && j < N; j++) {
            double lo = j > a ? j : a, hi = j + 1 < b ? j + 1 : b;
            if (hi > lo) out[j] += (uint32_t)(in[i] * (hi - lo) / (b - a) + 0.5);
        }
    }
}

int main(void)
{
    static uint32_t s164[N], s926[N], tmp[N];
    autocal_result_t r;
    CHECK(load("data/ra226_164s.txt", s164) == N);
    CHECK(load("data/ra226_926s.txt", s926) == N);

    // 1. Длинный спектр: все опоры, полином 4-й степени, близко к ручной калибровке.
    autocal_ra226(s926, N, ws, &r);
    print_result("926 s", &r);
    CHECK(r.err == AUTOCAL_OK && r.order == 4 && r.has_top && r.n >= 10);
    CHECK(has_line(&r, 351.93) && has_line(&r, 609.32) && has_line(&r, 1120.29) && has_line(&r, 1764.49));
    CHECK(r.max_abs_kev <= 12.0);
    double dv = max_dev_vs_ref(&r, 1.0, 250, 4600);
    double lo = 100, hi = 6000;
    for (int i = 0; i < 50; i++) { double m = (lo + hi) / 2; if (autocal_eval(r.coeffs, r.order, m) < 1460.82) lo = m; else hi = m; }
    printf("     vs manual calibration, ch 250..4600: max %.1f keV; K-40 1460.8 at ch %.0f (manual: 4021)\n", dv, lo);
    CHECK(fabs(lo - 4021) <= 25);
    CHECK(dv <= 8.0);
    CHECK(autocal_enough(&r));

    // 2. Короткий спектр (164 с): тоже должен откалиброваться.
    autocal_ra226(s164, N, ws, &r);
    print_result("164 s", &r);
    CHECK(r.err == AUTOCAL_OK && r.n >= 8);
    CHECK(max_dev_vs_ref(&r, 1.0, 250, r.order == 4 ? 4600 : 3500) <= 10.0);

    // 3. Смена усиления ±20 %: калибровка находится по рисунку пиков, а не по старой.
    const double scales[] = { 0.8, 0.9, 1.1, 1.2 };
    for (size_t k = 0; k < sizeof scales / sizeof scales[0]; k++) {
        double s = scales[k];
        rescale(s926, tmp, s);
        autocal_ra226(tmp, N, ws, &r);
        double top = (4600 * s < N - 400) ? 4600 : (N - 400) / s;
        double d = r.err == AUTOCAL_OK ? max_dev_vs_ref(&r, s, 250, r.order == 4 ? top : 3500) : 1e9;
        printf("  gain x%.1f: err %d, %d lines, order %d, max dev vs ref %.1f keV\n", s, r.err, r.n, r.order, d);
        CHECK(r.err == AUTOCAL_OK && r.n >= 8);
        CHECK(d <= 12.0);
    }

    // 4. Бедная статистика (прореживание 164 с до ~33 с и ~8 с): либо отказ, либо
    // калибровка не хуже 15 кэВ — но никогда не «статистики хватает» на 8 с.
    const double keep[] = { 0.2, 0.05 };
    unsigned seed = 12345;
    for (size_t k = 0; k < sizeof keep / sizeof keep[0]; k++) {
        for (int i = 0; i < N; i++) {
            uint32_t c = 0;
            for (uint32_t j = 0; j < s164[i]; j++) {
                seed = seed * 1103515245u + 12345u;
                if ((seed >> 8) % 100000 < (unsigned)(keep[k] * 100000)) c++;
            }
            tmp[i] = c;
        }
        autocal_ra226(tmp, N, ws, &r);
        double d = r.err == AUTOCAL_OK ? max_dev_vs_ref(&r, 1.0, 250, r.order == 4 ? 4600 : 3500) : 0;
        printf("  thinned to %.0f s: err %d, %d lines, order %d, max dev vs ref %.1f keV, enough %d\n",
               164 * keep[k], r.err, r.n, r.order, d, autocal_enough(&r));
        CHECK(d <= 15.0);
        if (keep[k] < 0.1) CHECK(!autocal_enough(&r));
    }

    // 5. Нет источника — честный отказ, а не выдуманная калибровка.
    for (int i = 0; i < N; i++) tmp[i] = (uint32_t)(200.0 * exp(-i / 1500.0) + 5);
    autocal_ra226(tmp, N, ws, &r);
    printf("  smooth background: err %d, %d lines\n", r.err, r.n);
    CHECK(r.err != AUTOCAL_OK);
    memset(tmp, 0, sizeof tmp);
    CHECK(autocal_ra226(tmp, N, ws, &r) == AUTOCAL_E_NODATA);

    if (g_failures) { printf("AUTOCAL: %d FAILED\n", g_failures); return 1; }
    printf("AUTOCAL: ALL PASSED\n");
    return 0;
}
