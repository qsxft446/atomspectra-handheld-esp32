// #ACAL-1 (запрос 2026-10-14): автокалибровка по источнику — поиск пиков, опознание
// линий, полином. Перенос анализа, сделанного вручную по спектрам Ra-226 прибора
// пользователя (164 и 926 с, ~7000 CPS):
//  * пики — «цилиндр» (отклик минус среднее на ±FWHM) по сглаженному спектру,
//    FWHM в каналах ~ 2.8*sqrt(ch) (сцинтиллятор: FWHM/E ~ 1/sqrt(E));
//  * опознание НЕ по старой калибровке (она может съехать как угодно), а по
//    рисунку: опорная пара — два сильных пика с характерным отношением каналов
//    (Ra-226: Pb-214 351.9 / Bi-214 609.3 ~1.7); из гипотез выбирается та, при
//    которой на предсказанных местах находится больше всего остальных линий;
//  * далее линии по одной, модель уточняется после каждой; выше ~1.3 МэВ у
//    прибора пользователя шкала сжата (ФЭУ), поэтому окно поиска верхних линий
//    сдвинуто вниз по каналам;
//  * центр пика — вершина гауссианы (парабола по ln над 55 % максимума);
//  * полином 4-й степени, если найдена верхняя опора и линий достаточно, иначе
//    парабола до 1.3 МэВ.
// #ACAL-6 (запрос 2026-10-14): таблицы линий для Ra-226, Th-232, Na-22, Cs-137+K-40.
// У Na-22 (511/1275) и Cs-137+K-40 (рентген Ba ~33/662/1461) линий 2-3 — полином по
// ним не построить, и форму шкалы (сжатие вверху) они не определяют. Режим MAP:
// канал старой шкалы = a*ch + b по МНК (старая калибровка даёт, в каком канале
// «должна» стоять линия), новая калибровка = E_old(a*ch + b) — тоже полином той же
// степени. Так две-три линии поправляют дрейф усиления, не ломая форму, снятую по
// Ra-226/Th-232. Без старой калибровки — прямая (2 линии) или парабола (3).
// Бленды: рентген Pb (Ka1/Ka2/Kb) ~79 кэВ, Bi-214 1377.7/1401.5/1408.0 — 1391.2,
// рентген Ba (Ka1/Ka2/Kb) ~32.9 кэВ — средневзвешенно по выходам.
#include "autocal.h"
#include <math.h>
#include <string.h>

// Окно поиска линии вокруг предсказанного канала.
enum {
    W_N = 0,    // ±5 %
    W_WIDE,     // ±20 % — рентгеновские бленды (энергия центроида условная)
    W_HI,       // 0.90..1.04 — начало сжатия шкалы
    W_TOP,      // 0.82..1.05 — сильное сжатие (1.6-1.8 МэВ)
    W_TOP2,     // 0.60..1.05 — ещё выше (2.6 МэВ)
};
typedef struct { double kev; const char *name; unsigned char win; } line_t;

typedef struct {
    const char   *name;
    const line_t *lines;         // по возрастанию энергии
    int           n_lines;
    int           pair_lo, pair_hi;
    double        ratio_min, ratio_max;   // отношение каналов опорной пары
    const int    *order;         // порядок поиска остальных линий
    int           n_order;
    int           top;           // верхняя опора (-1 — нет)
    int           ref609;        // линия для net609 (-1 — нет)
    int           poly4_min;     // линий для полинома 4-й степени (с верхней опорой)
    int           enough_min;    // линий для «статистики хватает»
    bool          enough_top;    // для «хватает» нужна верхняя опора
    int           max_s;         // предел набора, с
} source_t;

// --- Ra-226 (ряд: Pb-214, Bi-214) ---
static const line_t RA_LINES[] = {
    {   79.0,  "Pb X-ray",    W_WIDE },
    {  186.21, "Ra-226 186",  W_N },
    {  241.99, "Pb-214 242",  W_N },
    {  295.22, "Pb-214 295",  W_N },
    {  351.93, "Pb-214 352",  W_N },
    {  609.32, "Bi-214 609",  W_N },
    {  768.36, "Bi-214 768",  W_N },
    {  934.06, "Bi-214 934",  W_N },
    { 1120.29, "Bi-214 1120", W_N },
    { 1238.11, "Bi-214 1238", W_N },
    { 1391.2,  "Bi-214 1391*", W_HI },   // бленд 1377.7/1401.5/1408.0
    { 1764.49, "Bi-214 1764", W_TOP },
};
static const int RA_ORDER[] = { 3, 2, 1, 6, 7, 8, 0, 9, 10, 11 };

// --- Th-232 (ряд: Ac-228, Pb-212, Bi-212, Tl-208) ---
static const line_t TH_LINES[] = {
    {  238.63, "Pb-212 239",  W_N },
    {  338.32, "Ac-228 338",  W_N },
    {  510.77, "Tl-208 511",  W_N },
    {  583.19, "Tl-208 583",  W_N },
    {  727.33, "Bi-212 727",  W_N },
    {  860.56, "Tl-208 861",  W_N },
    {  911.20, "Ac-228 911",  W_N },
    {  968.97, "Ac-228 969",  W_N },
    { 1588.19, "Ac-228 1588", W_TOP },
    { 2614.51, "Tl-208 2615", W_TOP2 },
};
static const int TH_ORDER[] = { 1, 2, 4, 6, 7, 5, 8, 9 };

// --- Na-22 ---
static const line_t NA_LINES[] = {
    {  511.00, "Annihil. 511", W_N },
    { 1274.54, "Na-22 1275",   W_N },
};

// --- Cs-137 + K-40 (KCl / калийное удобрение) ---
static const line_t CSK_LINES[] = {
    {   32.9,  "Ba X-ray",    W_WIDE },   // Ka1 32.19 / Ka2 31.82 / Kb 36.4
    {  661.66, "Cs-137 662",  W_N },
    { 1460.82, "K-40 1461",   W_N },
};
static const int CSK_ORDER[] = { 0 };

#define NL(a) ((int)(sizeof(a) / sizeof((a)[0])))
static const source_t SRC[AUTOCAL_SRC_COUNT] = {
    [AUTOCAL_SRC_RA226]     = { "Ra-226", RA_LINES, NL(RA_LINES), 4, 5, 1.55, 1.90, RA_ORDER, NL(RA_ORDER),
                                11, 5, 8, 9, true, 300 },
    [AUTOCAL_SRC_TH232]     = { "Th-232", TH_LINES, NL(TH_LINES), 0, 3, 2.15, 2.75, TH_ORDER, NL(TH_ORDER),
                                9, -1, 7, 6, true, 300 },
    [AUTOCAL_SRC_NA22]      = { "Na-22", NA_LINES, NL(NA_LINES), 0, 1, 2.15, 2.85, NULL, 0,
                                -1, -1, 99, 2, false, 300 },
    [AUTOCAL_SRC_CS137_K40] = { "Cs-137+K-40", CSK_LINES, NL(CSK_LINES), 1, 2, 1.85, 2.50, CSK_ORDER, NL(CSK_ORDER),
                                -1, -1, 99, 2, false, 600 },
};

const char *autocal_source_name(int s) { return s >= 0 && s < AUTOCAL_SRC_COUNT ? SRC[s].name : "?"; }
int autocal_source_max_s(int s) { return s >= 0 && s < AUTOCAL_SRC_COUNT ? SRC[s].max_s : 300; }

#define MAX_CAND  96

typedef struct { int ch; float z, area; } cand_t;

static double fwhm_ch(double ch) { double f = 2.8 * sqrt(ch > 1 ? ch : 1); return f < 6 ? 6 : f; }

double autocal_eval(const double *c, int order, double ch)
{
    double e = 0, p = 1;
    for (int i = 0; i <= order; i++) { e += c[i] * p; p *= ch; }
    return e;
}

// МНК-полином степени deg по точкам (x в тысячах каналов — для обусловленности).
static bool polyfit(const double *x, const double *y, int n, int deg, double *coef)
{
    int m = deg + 1;
    double A[AUTOCAL_MAX_COEFFS][AUTOCAL_MAX_COEFFS + 1];
    if (n < m) return false;
    for (int r = 0; r < m; r++) {
        for (int c = 0; c <= m; c++) A[r][c] = 0;
        for (int k = 0; k < n; k++) {
            double xs = x[k] / 1000.0, pr = pow(xs, r);
            for (int c = 0; c < m; c++) A[r][c] += pr * pow(xs, c);
            A[r][m] += pr * y[k];
        }
    }
    for (int c = 0; c < m; c++) {
        int p = c;
        for (int r = c + 1; r < m; r++) if (fabs(A[r][c]) > fabs(A[p][c])) p = r;
        if (fabs(A[p][c]) < 1e-12) return false;
        if (p != c) for (int k = 0; k <= m; k++) { double t = A[c][k]; A[c][k] = A[p][k]; A[p][k] = t; }
        for (int r = 0; r < m; r++) {
            if (r == c) continue;
            double f = A[r][c] / A[c][c];
            for (int k = c; k <= m; k++) A[r][k] -= f * A[c][k];
        }
    }
    for (int i = 0; i < AUTOCAL_MAX_COEFFS; i++) coef[i] = 0;
    for (int i = 0; i < m; i++) coef[i] = A[i][m] / A[i][i] / pow(1000.0, i);
    return true;
}

// Канал, где модель даёт энергию e (бисекция; модель монотонна на [lo, hi]).
static double inv_eval(const double *c, int order, double e, double lo, double hi)
{
    for (int i = 0; i < 50; i++) {
        double m = (lo + hi) / 2;
        if (autocal_eval(c, order, m) < e) lo = m; else hi = m;
    }
    return (lo + hi) / 2;
}

static bool centroid_once(const uint32_t *y, int n, double c0, double *mu, double *net_out, double *fwhm_out);

// Центр пика — парабола по ln, один проход. #ACAL-6 (2026-10-14): пробовал (а) повтор с
// перецентрированием окна и (б) первый момент для слабых пиков — на РЕАЛЬНОМ спектре
// Ra-226 оба хуже (слившиеся линии тянут центр: rms 3.7 -> 6.2 кэВ, K-40 +14 каналов,
// при усилении x0.9 подгонка не сходилась). Оставлен один проход.
static bool centroid(const uint32_t *y, int n, double c0, double *mu, double *net_out, double *fwhm_out)
{
    return centroid_once(y, n, c0, mu, net_out, fwhm_out);
}

// Один проход: линейный фон по краям окна ±1.3 FWHM, парабола по ln(нетто) для точек
// выше 55 % максимума. Возвращает false, если пика нет.
static bool centroid_once(const uint32_t *y, int n, double c0, double *mu, double *net_out, double *fwhm_out)
{
    double fw = fwhm_ch(c0);
    int a = (int)(c0 - 1.3 * fw), b = (int)(c0 + 1.3 * fw);
    if (a < 8 || b > n - 9) return false;
    double L = 0, R = 0;
    for (int i = -7; i <= 7; i++) { L += y[a + i]; R += y[b + i]; }
    L /= 15; R /= 15;
    int im = -1;
    double vm = 0, net = 0;
    for (int i = a; i <= b; i++) {
        double s = 0; int k = 0;
        for (int j = i - 5; j <= i + 5; j++) { if (j < a || j > b) continue; s += y[j] - (L + (R - L) * (j - a) / (b - a)); k++; }
        s /= k;
        if (s > vm) { vm = s; im = i; }
        double v = y[i] - (L + (R - L) * (i - a) / (b - a));
        if (v > 0) net += v;
    }
    if (im < 0 || vm <= 0) return false;
    double S[3][4] = {{0}};
    int used = 0;
    for (int i = a; i <= b; i++) {
        double v = y[i] - (L + (R - L) * (i - a) / (b - a));
        if (v <= 0.55 * vm || fabs((double)(i - im)) >= fw) continue;
        double x = i - im, w = v, lv = log(v), p[3] = { 1, x, x * x };
        for (int r = 0; r < 3; r++) { S[r][3] += w * p[r] * lv; for (int c = 0; c < 3; c++) S[r][c] += w * p[r] * p[c]; }
        used++;
    }
    if (used < 5) return false;
    for (int c = 0; c < 3; c++) {
        int p = c;
        for (int r = c + 1; r < 3; r++) if (fabs(S[r][c]) > fabs(S[p][c])) p = r;
        if (fabs(S[p][c]) < 1e-12) return false;
        if (p != c) for (int k = 0; k < 4; k++) { double t = S[c][k]; S[c][k] = S[p][k]; S[p][k] = t; }
        for (int r = 0; r < 3; r++) {
            if (r == c) continue;
            double f = S[r][c] / S[c][c];
            for (int k = c; k < 4; k++) S[r][k] -= f * S[c][k];
        }
    }
    double c1 = S[1][3] / S[1][1], c2 = S[2][3] / S[2][2];
    if (c2 >= 0) return false;                // не купол
    double d = -c1 / (2 * c2);
    if (fabs(d) > fw / 2) return false;       // вершина ушла из пика
    *mu = im + d;
    *net_out = net;
    *fwhm_out = 2.3548 * sqrt(-1.0 / (2 * c2));   // ширина пика по той же параболе
    return true;
}

// Кандидаты в пики: локальные максимумы значимости «цилиндра».
static int find_candidates(const uint32_t *y, int n, float *ws, cand_t *cand)
{
    float *s = ws, *z = ws + n;
    const int SW = 7;   // сглаживание
    double acc = 0;
    for (int i = 0; i < n; i++) {
        acc += y[i];
        if (i >= SW) acc -= y[i - SW];
        s[i] = (float)(acc / (i + 1 < SW ? i + 1 : SW));
    }
    for (int i = 0; i < n - SW / 2; i++) s[i] = s[i + SW / 2];   // центровка сглаживания
    for (int i = 0; i < n; i++) {
        int w = (int)fwhm_ch(i);
        if (i - w < 0 || i + w >= n) { z[i] = 0; continue; }
        double side = 0.5 * (s[i - w] + s[i + w]);
        double net = s[i] - side;
        double noise = sqrt((s[i] + 0.5 * side) / SW + 1.0);
        z[i] = (float)(net > 0 ? net / noise : 0);
    }
    int nc = 0;
    for (int i = 40; i < n - 40 && nc < MAX_CAND; i++) {
        if (z[i] < 6) continue;
        int h = (int)(fwhm_ch(i) / 2);
        bool top = true;
        for (int j = i - h; j <= i + h && top; j++)
            if (j >= 0 && j < n && j != i && (z[j] > z[i] || (z[j] == z[i] && j < i))) top = false;
        if (!top) continue;
        int w = (int)fwhm_ch(i);
        cand[nc].ch = i;
        cand[nc].z = z[i];
        cand[nc].area = (float)((s[i] - 0.5 * (s[i - w] + s[i + w])) * w);
        nc++;
    }
    return nc;
}

typedef struct { int line; int cand; } assign_t;

// Подходящий кандидат для линии в окне [lo, hi] каналов: самый значимый, не занятый,
// и так, чтобы порядок каналов совпадал с порядком энергий.
static int pick(const source_t *S, const cand_t *c, int nc, double lo, double hi, const int *used_by,
                const assign_t *as, int na, int line)
{
    int best = -1;
    for (int k = 0; k < nc; k++) {
        if (c[k].ch < lo || c[k].ch > hi || used_by[k] >= 0) continue;
        bool order_ok = true;
        for (int j = 0; j < na && order_ok; j++) {
            double eo = S->lines[as[j].line].kev, e = S->lines[line].kev;
            int co = c[as[j].cand].ch;
            if ((e < eo && c[k].ch >= co) || (e > eo && c[k].ch <= co)) order_ok = false;
        }
        if (!order_ok) continue;
        if (best < 0 || c[k].z > c[best].z) best = k;
    }
    return best;
}

// Модель по текущим назначениям: прямая или парабола; кубика, когда уже есть верх.
static bool model(const source_t *S, const cand_t *c, const assign_t *as, int na, double *coef, int *order)
{
    double x[AUTOCAL_MAX_LINES], e[AUTOCAL_MAX_LINES];
    int k = 0;
    for (int j = 0; j < na; j++) { x[k] = c[as[j].cand].ch; e[k] = S->lines[as[j].line].kev; k++; }
    int deg = k >= 6 ? 2 : 1;
    if (k >= 8 && na && S->lines[as[na - 1].line].kev > 1300) deg = 3;
    if (!polyfit(x, e, k, deg, coef)) return false;
    *order = deg;
    return true;
}

static int assign_from_pair(const source_t *S, const cand_t *c, int nc, int a, int b, assign_t *as, int *used_by)
{
    for (int k = 0; k < nc; k++) used_by[k] = -1;
    int na = 0;
    as[na++] = (assign_t){ S->pair_lo, a }; used_by[a] = S->pair_lo;
    as[na++] = (assign_t){ S->pair_hi, b }; used_by[b] = S->pair_hi;
    for (int s = 0; s < S->n_order; s++) {
        int line = S->order[s];
        double coef[AUTOCAL_MAX_COEFFS];
        int order;
        if (!model(S, c, as, na, coef, &order)) break;
        double e = S->lines[line].kev;
        double maxch = c[nc - 1].ch * 1.6 + 200;
        double pred = inv_eval(coef, order, e, 0, maxch);
        double lo, hi, fw = fwhm_ch(pred);
        switch (S->lines[line].win) {
        case W_WIDE: lo = pred * 0.80; hi = pred * 1.20; break;
        case W_HI:   lo = pred * 0.90; hi = pred * 1.04; break;
        case W_TOP:  lo = pred * 0.82; hi = pred * 1.05; break;
        case W_TOP2: lo = pred * 0.60; hi = pred * 1.05; break;
        default:     lo = pred * 0.95; hi = pred * 1.05; break;
        }
        if (hi - lo < fw) { lo = pred - fw / 2; hi = pred + fw / 2; }
        int k = pick(S, c, nc, lo, hi, used_by, as, na, line);
        if (k < 0) continue;
        used_by[k] = line;
        as[na++] = (assign_t){ line, k };
    }
    return na;
}

// Невязки, монотонность, статистика. deg — степень, skip_hi — пропускать линии выше
// 1.3 МэВ в невязках (парабола до излома). false — подгонка негодна.
static bool finalize(autocal_result_t *out, int deg, bool skip_hi)
{
    double ss = 0, mx = 0;
    int cnt = 0;
    double chmax = out->ch[out->n - 1] * 1.15;
    for (int i = 0; i < out->n; i++) {
        out->fit_kev[i] = autocal_eval(out->coeffs, deg, out->ch[i]);
        if (skip_hi && out->kev[i] > 1300) continue;
        double r = out->fit_kev[i] - out->kev[i];
        ss += r * r; cnt++;
        if (fabs(r) > mx) mx = fabs(r);
    }
    for (double c = 0; c < chmax; c += 8)   // монотонность от 0 до чуть выше последней линии
        if (autocal_eval(out->coeffs, deg, c + 8) <= autocal_eval(out->coeffs, deg, c)) return false;
    if (mx > 15.0 || cnt == 0) return false;
    out->order = deg;
    out->rms_kev = sqrt(ss / cnt);
    out->max_abs_kev = mx;
    // #ACAL-4: погрешность центра: (FWHM/2.355)/sqrt(нетто) x2, FWHM в кэВ по наклону
    out->max_sigma_kev = 0;
    for (int i = 0; i < out->n; i++) {
        double fwc = out->sigma_kev[i];
        double fwk = autocal_eval(out->coeffs, deg, out->ch[i] + fwc / 2) -
                     autocal_eval(out->coeffs, deg, out->ch[i] - fwc / 2);
        double s = fwk / 2.3548 / sqrt(out->net[i] > 1 ? out->net[i] : 1) * 2;
        out->sigma_kev[i] = s;
        if (s > out->max_sigma_kev) out->max_sigma_kev = s;
    }
    return true;
}

// #ACAL-6: MAP — канал старой шкалы = a*ch + b (МНК), новая калибровка E_old(a*ch + b).
static bool map_fit(autocal_result_t *out, const double *old_c, int old_order)
{
    if (!old_c || old_order < 1 || old_order >= AUTOCAL_MAX_COEFFS || out->n < 2) return false;
    double prev = autocal_eval(old_c, old_order, 0);
    for (int c = 16; c < 8192; c += 16) {   // старая калибровка должна быть монотонной
        double e = autocal_eval(old_c, old_order, c);
        if (e <= prev) return false;
        prev = e;
    }
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (int i = 0; i < out->n; i++) {
        double x = out->ch[i], y = inv_eval(old_c, old_order, out->kev[i], 0, 8191);
        sx += x; sy += y; sxx += x * x; sxy += x * y;
    }
    double n = out->n, den = n * sxx - sx * sx;
    if (fabs(den) < 1e-9) return false;
    double a = (n * sxy - sx * sy) / den, b = (sy - a * sx) / n;
    if (a < 0.5 || a > 2.0) return false;    // усиление не может уйти вдвое
    // E_new(ch) = sum_k old_k (a ch + b)^k — бином
    double c[AUTOCAL_MAX_COEFFS] = { 0 };
    for (int k = 0; k <= old_order; k++) {
        double binom = 1;
        for (int j = 0; j <= k; j++) {
            if (j > 0) binom = binom * (k - j + 1) / j;
            c[j] += old_c[k] * binom * pow(a, j) * pow(b, k - j);
        }
    }
    memcpy(out->coeffs, c, sizeof c);
    out->map_a = a;
    out->map_b = b;
    out->mode = AUTOCAL_MODE_MAP;
    return finalize(out, old_order, false);
}

static int run_core(int source, const uint32_t *bins, int n, float *ws,
                    const double *old_c, int old_order, autocal_result_t *out)
{
    memset(out, 0, sizeof *out);
    out->source = source;
    if (source < 0 || source >= AUTOCAL_SRC_COUNT) return out->err = AUTOCAL_E_NODATA;
    const source_t *S = &SRC[source];
    uint64_t total = 0;
    for (int i = 0; i < n; i++) total += bins[i];
    if (n < 1024 || total < 20000) return out->err = AUTOCAL_E_NODATA;

    cand_t cand[MAX_CAND];
    int nc = find_candidates(bins, n, ws, cand);
    if (nc < 2) return out->err = AUTOCAL_E_NOPAIR;

    // Опорная пара: среди 8 самых крупных пиков — пары с отношением каналов источника;
    // побеждает гипотеза с наибольшим числом опознанных линий (при равенстве — площадь).
    int order_idx[MAX_CAND];
    for (int i = 0; i < nc; i++) order_idx[i] = i;
    for (int i = 0; i < nc; i++)
        for (int j = i + 1; j < nc; j++)
            if (cand[order_idx[j]].area > cand[order_idx[i]].area) { int t = order_idx[i]; order_idx[i] = order_idx[j]; order_idx[j] = t; }
    int top = nc < 8 ? nc : 8;
    assign_t best_as[AUTOCAL_MAX_LINES], as[AUTOCAL_MAX_LINES];
    int used_by[MAX_CAND], best_na = 0;
    double best_area = 0;
    for (int i = 0; i < top; i++)
        for (int j = 0; j < top; j++) {
            int a = order_idx[i], b = order_idx[j];
            if (cand[a].ch <= 0 || cand[b].ch <= cand[a].ch) continue;
            double r = (double)cand[b].ch / cand[a].ch;
            if (r < S->ratio_min || r > S->ratio_max) continue;
            int na = assign_from_pair(S, cand, nc, a, b, as, used_by);
            double area = cand[a].area + cand[b].area;
            if (na > best_na || (na == best_na && area > best_area)) {
                best_na = na; best_area = area;
                memcpy(best_as, as, sizeof as);
            }
        }
    if (best_na < 2) return out->err = AUTOCAL_E_NOPAIR;

    // Точные центры и итоговый набор (по возрастанию энергии).
    for (int line = 0; line < S->n_lines; line++) {
        for (int j = 0; j < best_na; j++) {
            if (best_as[j].line != line) continue;
            double mu, net, fwc;
            if (!centroid(bins, n, cand[best_as[j].cand].ch, &mu, &net, &fwc)) break;
            int k = out->n++;
            out->ch[k] = mu; out->kev[k] = S->lines[line].kev; out->name[k] = S->lines[line].name; out->net[k] = net;
            out->sigma_kev[k] = fwc;   // пока FWHM в каналах; в кэВ — после подгонки
            if (line == S->ref609) out->net609 = net;
            if (line == S->top) { out->has_top = true; out->net_top = net; }
        }
    }
    if (out->n < 2) return out->err = AUTOCAL_E_FEW;

    // Мало линий (2-3): поправка текущей калибровки (MAP), без неё — прямая / парабола.
    if (out->n <= 3) {
        if (map_fit(out, old_c, old_order)) return out->err = AUTOCAL_OK;
        out->mode = AUTOCAL_MODE_POLY;
        double x[AUTOCAL_MAX_LINES], e[AUTOCAL_MAX_LINES];
        for (int i = 0; i < out->n; i++) { x[i] = out->ch[i]; e[i] = out->kev[i]; }
        int deg = out->n - 1;
        if (polyfit(x, e, out->n, deg, out->coeffs) && finalize(out, deg, false)) return out->err = AUTOCAL_OK;
        return out->err = AUTOCAL_E_FIT;
    }

    out->mode = AUTOCAL_MODE_POLY;
    int deg = (out->has_top && out->n >= S->poly4_min) ? 4 : 2;
    for (;;) {
        double x[AUTOCAL_MAX_LINES], e[AUTOCAL_MAX_LINES];
        int k = 0;
        for (int i = 0; i < out->n; i++) {
            if (deg < 4 && out->kev[i] > 1300) continue;   // парабола — только до излома
            x[k] = out->ch[i]; e[k] = out->kev[i]; k++;
        }
        if (polyfit(x, e, k, deg, out->coeffs) && finalize(out, deg, deg < 4)) return out->err = AUTOCAL_OK;
        if (deg == 4) { deg = 2; out->has_top = false; continue; }
        return out->err = AUTOCAL_E_FIT;
    }
}

// #ACAL-6: у Ra-226 и Th-232 похожие соотношения линий (242/609 против 239/583): процедура
// тория на спектре радия «находила» 7 линий и давала неверную калибровку (host-тест на
// реальном спектре), и наоборот. После успеха сравниваем долю найденных линий с другим
// многолинейным источником; заметно лучше у другого — отказ с подсказкой. Na-22 и Cs+K
// в сравнении не участвуют: из 2-3 линий любой спектр с подходящей парой «найдёт» все.
int autocal_run(int source, const uint32_t *bins, int n, float *ws,
                const double *old_c, int old_order, autocal_result_t *out)
{
    int err = run_core(source, bins, n, ws, old_c, old_order, out);
    if (err != AUTOCAL_OK) return err;
    static const int MULTI[] = { AUTOCAL_SRC_RA226, AUTOCAL_SRC_TH232 };
    double f_self = (double)out->n / SRC[source].n_lines;
    for (size_t i = 0; i < sizeof MULTI / sizeof MULTI[0]; i++) {
        int c = MULTI[i];
        if (c == source) continue;
        autocal_result_t other;
        if (run_core(c, bins, n, ws, NULL, 0, &other) != AUTOCAL_OK) continue;
        double f_other = (double)other.n / SRC[c].n_lines;
        if (f_other >= f_self + 0.15) {
            out->wrong_src = c;
            return out->err = AUTOCAL_E_WRONGSRC;
        }
    }
    return AUTOCAL_OK;
}

int autocal_ra226(const uint32_t *bins, int n, float *ws, autocal_result_t *out)
{
    return autocal_run(AUTOCAL_SRC_RA226, bins, n, ws, NULL, 0, out);
}

bool autocal_enough(const autocal_result_t *r)
{
    // #ACAL-4: по точности всех линий; #ACAL-6: требования — по источнику.
    if (r->err != AUTOCAL_OK || r->source < 0 || r->source >= AUTOCAL_SRC_COUNT) return false;
    const source_t *S = &SRC[r->source];
    if (r->n < S->enough_min || (S->enough_top && !r->has_top)) return false;
    if (S->poly4_min < 99 && r->order != 4) return false;   // Ra/Th: нужен полный полином
    return r->max_sigma_kev <= AUTOCAL_SIGMA_MAX;
}
