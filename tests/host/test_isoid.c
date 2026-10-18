// #ISO-1: host-тест определения нуклидов на реальных спектрах AtomSpectra.
// data/svk/*.txt — фикстуры spectravibe-toolkit (MIT, VibeEngineering LLC): Cs-137, KCl,
// фоны, радон, медальон, черника, маринелли, лабораторный; data/ra226_*.txt — Ra-226
// прибора пользователя (калибровка — ручная REF, как в test_autocal.c).
// MSVC: cl /W4 /WX /DISOID_HOST_DEBUG /I ..\..\main test_isoid.c ..\..\main\isoid.c
// Запуск из tests/host; аргумент v — печать пиков и решений, имя файла — только он.
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "isoid.h"

#define NMAX 8192
static const double REF[5] = { 7.76253299801587, 0.267995214878602, 8.07264835375737e-05,
                               -3.28147308346862e-08, 4.60405912953082e-12 };
static uint32_t bins[NMAX];
static unsigned char ws[NMAX * 20 + 49152];
int g_failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_failures++; } } while (0)

static int load(const char *path, double *cal, int *order)
{
    FILE *f = fopen(path, "r");
    if (!f) { printf("no file %s\n", path); return 0; }
    char line[512];
    int n = 0;
    *order = -1;
    while (fgets(line, sizeof line, f) && n < NMAX) {
        if (!strncmp(line, "#cal", 4)) {
            char *p = line + 4;
            int k = 0;
            for (;;) { char *e; double v = strtod(p, &e); if (e == p || k >= 5) break; cal[k++] = v; p = e; }
            *order = k - 1;
            continue;
        }
        if (line[0] == '#') continue;
        bins[n++] = (uint32_t)strtoul(line, NULL, 10);
    }
    fclose(f);
    if (*order < 0) { memcpy(cal, REF, sizeof REF); *order = 4; }
    return n;
}

// #ISO-3: как на плате — несколько прогонов, настройка переносится (здесь полным шагом).
static isoid_tune_t g_tune;
static void run_tuned(int n, const double *cal, int order, isoid_result_t *r)
{
    isoid_tune_t t, tn;
    isoid_tune_default(&t);
    for (int k = 0; k < 4; k++) {
        isoid_run(bins, n, cal, order, ws, &t, &tn, r);
        if (tn.n_w) t.kw = tn.kw;
        if (tn.n_g) { t.ga = tn.ga; t.gb = tn.gb; }
    }
    g_tune = t;
}

static int has(const isoid_result_t *r, const char *name)
{
    for (int i = 0; i < r->n; i++) if (!strcmp(isoid_name(r->f[i].src), name)) return 1;
    return 0;
}

static int manmade(const isoid_result_t *r)
{
    int k = 0;
    for (int i = 0; i < r->n; i++) k += !r->f[i].natural;
    return k;
}

static void run(const char *file, isoid_result_t *r)
{
    char path[256];
    double cal[5] = { 0 };
    int order;
    if (strchr(file, ':') || file[0] == '/') snprintf(path, sizeof path, "%s", file);   // полный путь (отладка)
    else snprintf(path, sizeof path, "data/%s", file);
    int n = load(path, cal, &order);
    if (isoid_debug) printf("--- %s (%d ch)\n", file, n);
    run_tuned(n, cal, order, r);
    printf("%-26s peaks %3d kw %.2f g662 %.3f g1460 %.3f :", file, r->n_peaks, g_tune.kw,
           g_tune.ga + g_tune.gb * 662, g_tune.ga + g_tune.gb * 1460);
    for (int i = 0; i < r->n; i++)
        printf(" %s%s(%.2f,%.0f,%d%%)", isoid_name(r->f[i].src), r->f[i].natural ? "*" : "", r->f[i].conf, r->f[i].sig, r->f[i].quality);
    printf("\n");
}

// Детерминированный ГСЧ и пуассоновское прореживание (биномиальное: каждый отсчёт с p).
static uint64_t rng = 88172645463325252ull;
static double urand(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (rng >> 11) * (1.0 / 9007199254740992.0); }
static uint32_t binom(uint32_t n, double p)
{
    if (n < 64) { uint32_t k = 0; for (uint32_t i = 0; i < n; i++) k += urand() < p; return k; }
    double m = n * p, sd = sqrt(m * (1 - p)), u1 = urand(), u2 = urand();
    double g = sqrt(-2 * log(u1 + 1e-300)) * cos(6.283185307 * u2);
    double v = m + sd * g + 0.5;
    return v < 0 ? 0 : (uint32_t)v;
}
static double ev5(const double *c, double ch) { double e = 0, p = 1; for (int i = 0; i < 5; i++) { e += c[i] * p; p *= ch; } return e; }

// Короткий набор: спектр Ra-226 926 с, прореженный до ~sec секунд.
static void thin_test(double sec)
{
    double cal[5]; int order;
    int n = load("data/ra226_926s.txt", cal, &order);
    for (int i = 0; i < n; i++) bins[i] = binom(bins[i], sec / 926.0);
    isoid_result_t r;
    run_tuned(n, cal, order, &r);
    printf("Ra-226 thinned to %4.0f s     :", sec);
    for (int i = 0; i < r.n; i++) printf(" %s%s(%.2f,%.0f,%d%%)", isoid_name(r.f[i].src), r.f[i].natural ? "*" : "", r.f[i].conf, r.f[i].sig, r.f[i].quality);
    printf("\n");
    CHECK(manmade(&r) == 0);                // никогда не чужой
    if (sec >= 45) CHECK(has(&r, "Ra-226"));
}

// Смесь: к Ra-226 (164 с) добавлены гауссовы пики линий (E, отсчётов) — как источник рядом.
static void mix_test(const char *label, const double *E, const double *N, int k, const char *want1, const char *want2)
{
    double cal[5]; int order;
    int n = load("data/ra226_164s.txt", cal, &order);
    for (int j = 0; j < k; j++) {
        // ширина подмешиваемых пиков — как у прибора пользователя (по Ra-226 926 с: ~1.1 модели isoid)
        double fw = 1.1 * 0.85 * pow(E[j], 0.6), sl = ev5(cal, 2001) - ev5(cal, 2000);
        double c0 = 0; for (int i = 0; i < n; i++) if (ev5(cal, i) >= E[j]) { c0 = i; sl = ev5(cal, i + 1) - ev5(cal, i); break; }
        double sg = fw / sl / 2.355;
        for (int i = (int)(c0 - 5 * sg); i <= (int)(c0 + 5 * sg); i++) {
            if (i < 0 || i >= n) continue;
            double m = N[j] * exp(-0.5 * (i - c0) * (i - c0) / (sg * sg)) / (sg * 2.5066);
            bins[i] += binom((uint32_t)(m * 2 + 0.5), 0.5);
        }
    }
    isoid_result_t r;
    run_tuned(n, cal, order, &r);
    printf("%-30s:", label);
    for (int i = 0; i < r.n; i++) printf(" %s%s(%.2f,%.0f,%d%%)", isoid_name(r.f[i].src), r.f[i].natural ? "*" : "", r.f[i].conf, r.f[i].sig, r.f[i].quality);
    printf("\n");
    CHECK(has(&r, "Ra-226"));
    if (want1) CHECK(has(&r, want1));
    if (want2) CHECK(has(&r, want2));
}

// Ложные пики: гладкая подложка как у фона (два экспоненциальных склона) + пуассоновский шум,
// без единого пика; всё найденное — ложное. total — отсчётов в спектре.
static void noise_test(double total, int runs, int *peaks_out, int *ids_out)
{
    double cal[5]; memcpy(cal, REF, sizeof REF);
    static double shape[NMAX];
    double sum = 0;
    for (int i = 0; i < NMAX; i++) {
        double e = ev5(cal, i);
        // плавный порог внизу (горб ~70 кэВ шириной в десятки кэВ, как у фона NaI) — без изломов
        shape[i] = e < 5 ? 0 : (exp(-e / 120.0) * 6 + exp(-e / 700.0)) / (1 + exp(-(e - 45) / 10.0));
        sum += shape[i];
    }
    int pk = 0, id = 0;
    for (int r = 0; r < runs; r++) {
        for (int i = 0; i < NMAX; i++) {
            double m = shape[i] / sum * total;
            uint32_t n = (uint32_t)(m * 4 + 0.5);
            bins[i] = m < 1e-9 ? 0 : binom(n, m / (n ? n : 1));
        }
        isoid_result_t res;
        isoid_tune_t t; isoid_tune_default(&t);
        isoid_run(bins, NMAX, cal, 4, ws, &t, NULL, &res);
        pk += res.n_peaks; id += res.n;
        if (isoid_debug && res.n) for (int k = 0; k < res.n; k++) printf("    noise id: %s\n", isoid_name(res.f[k].src));
    }
    *peaks_out = pk; *ids_out = id;
}

int main(int argc, char **argv)
{
    static const char *FILES[] = {
        "svk/cs137.txt", "svk/kcl.txt", "svk/medallion.txt", "svk/radon.txt", "svk/blueberry.txt",
        "svk/altai_marinelli.txt", "svk/dski_gs5050.txt", "svk/bg_cs137.txt", "svk/bg_kcl.txt",
        "svk/bg_office.txt", "ra226_164s.txt", "ra226_926s.txt", "live_cs137_co60_63s.txt",
        "live_cs137_co60_525s.txt", "live_cs137_co60_36s.txt",
        "live_uglass_759s.txt", "live_bg_concrete_571s.txt",
        "live_bg_195s.txt", "live_bg_247s.txt", "live_bg_543s.txt", "live_bg_1024s.txt",
    };
    isoid_result_t r[sizeof FILES / sizeof FILES[0]];
    const char *only = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "v")) isoid_debug = 1;
        else only = argv[i];
    }
    if (sizeof ws < isoid_ws_size(NMAX)) { printf("ws too small\n"); return 1; }
    for (size_t i = 0; i < sizeof FILES / sizeof FILES[0] && !(only && !strcmp(only, "noise")); i++) {
        if (only && (strchr(only, ':') || only[0] == '/')) { run(only, &r[0]); return 0; }   // файл по пути
        if (only && !strstr(FILES[i], only)) { r[i].n = -1; continue; }
        run(FILES[i], &r[i]);
    }
    if (only && strcmp(only, "noise")) return 0;
    {   // #ISO-6: спектр без пиков — ни одного опознания
        static const double T[] = { 1e4, 3e4, 1e5, 3e5, 1e6, 3e6, 1e7 };
        for (int k = 0; k < 7; k++) {
            int p, d;
            noise_test(T[k], 30, &p, &d);
            printf("noise %8.0f counts: %5.2f false peaks / spectrum, %d identifications in 30 spectra\n", T[k], p / 30.0, d);
            CHECK(d == 0);
        }
    }
    if (only) return g_failures ? 1 : 0;
    thin_test(5); thin_test(15); thin_test(45); thin_test(150);
    { const double E[] = { 661.66 }, N[] = { 20000 };                 mix_test("Ra-226 + Cs-137 (20k)", E, N, 1, "Cs-137", NULL); }
    { const double E[] = { 1173.2, 1332.5 }, N[] = { 6000, 5400 };    mix_test("Ra-226 + Co-60 (6k)", E, N, 2, "Co-60", NULL); }
    { const double E[] = { 59.54 }, N[] = { 30000 };                  mix_test("Ra-226 + Am-241 (30k)", E, N, 1, "Am-241", NULL); }
    // Na-22 сильнее линий Bi-214 1238/1281 радия (слабый — не разделить на NaI, см. isoid.c)
    { const double E[] = { 511.0, 1274.5 }, N[] = { 50000, 14000 };  mix_test("Ra-226 + Na-22 (511+1274)", E, N, 2, "Na-22", NULL); }
    { const double E[] = { 661.66, 1173.2, 1332.5 }, N[] = { 30000, 8000, 7200 };
      mix_test("Ra-226 + Cs-137 + Co-60", E, N, 3, "Cs-137", "Co-60"); }
    // [6] dski_gs5050 — калибровка файла съехала ~ -8 % (K-40 на 1340, Tl-208 на 2417): без
    // ожиданий, пример того, что без верной калибровки определение не работает.
    CHECK(has(&r[0], "Cs-137") && manmade(&r[0]) == 1);
    CHECK(has(&r[2], "Th-232") && manmade(&r[2]) == 0);   // медальон (калибровка файла ~ -3 % вверху)
    CHECK(has(&r[4], "Cs-137") && manmade(&r[4]) == 1);   // черника: слабый Cs-137 на фоне
    CHECK(has(&r[5], "Cs-137") && manmade(&r[5]) == 1);
    CHECK(has(&r[1], "K-40") && manmade(&r[1]) == 0);
    CHECK(has(&r[3], "Ra-226") && manmade(&r[3]) == 0);
    CHECK(has(&r[10], "Ra-226") && manmade(&r[10]) == 0);
    CHECK(has(&r[11], "Ra-226") && manmade(&r[11]) == 0);
    for (int i = 7; i <= 9; i++) CHECK(manmade(&r[i]) == 0);   // фоны — только природное
    // прибор пользователя: слабый Cs-137 + Co-60, шкала вверху на -2 % (Zn-65 1115 ловил 1147)
    CHECK(has(&r[12], "Cs-137") && has(&r[12], "Co-60") && manmade(&r[12]) == 2);
    // то же, 525 с: рентген Pb ~79 кэВ от источников — второй проход давал «плечи» 66/95 кэВ
    // (ложные U-238 63, Sm-153 103)
    CHECK(has(&r[13], "Cs-137") && has(&r[13], "Co-60") && manmade(&r[13]) == 2 && !has(&r[13], "U-238"));
    // 36 с, шкала -3 %: Co-60 1332 на 1294 давал Na-22 1274 — без пика 511 Na-22 не принимается
    CHECK(!has(&r[14], "Na-22"));
    // урановое стекло + Co-60 рядом, гранит (Th 583/2614 без видимой 238): U-238, U-235, Co-60; не Bi-207
    CHECK(has(&r[15], "U-238") && has(&r[15], "U-235") && has(&r[15], "Co-60") && !has(&r[15], "Bi-207"));
    // фон в бетонном помещении: K-40, Th; 903 кэВ = Ac-228 911/969 — не Y-88 898
    CHECK(has(&r[16], "K-40") && manmade(&r[16]) == 0);
    // #ISO-5: фон помещения пользователя 195-1024 с (ловушка 2026-10-18): ложные Y-88 (Ac-228 911,
    // нет 1836), Sm-153 (рентген 97 кэВ), Bi-207 — ни одного техногенного
    for (int i = 17; i <= 20; i++) CHECK(manmade(&r[i]) == 0);
    if (g_failures) { printf("ISOID: %d FAILED\n", g_failures); return 1; }
    printf("ISOID: ALL PASSED\n");
    return 0;
}
