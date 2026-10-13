// #ACAL-1 (запрос 2026-10-14): автокалибровка по Ra-226 — поиск пиков, опознание
// линий, полином. Перенос анализа, сделанного вручную по спектрам прибора
// пользователя (164 и 926 с, ~7000 CPS):
//  * пики — «цилиндр» (отклик минус среднее на ±FWHM) по сглаженному спектру,
//    FWHM в каналах ~ 2.8*sqrt(ch) (сцинтиллятор: FWHM/E ~ 1/sqrt(E));
//  * опознание НЕ по старой калибровке (она может съехать как угодно), а по
//    рисунку: опорная пара — два сильных пика с отношением каналов ~1.7
//    (Pb-214 351.9 / Bi-214 609.3); из гипотез выбирается та, при которой на
//    предсказанных местах находится больше всего остальных линий ряда;
//  * далее линии по одной, модель уточняется после каждой; выше ~1.3 МэВ у
//    прибора пользователя шкала сжата (ФЭУ), поэтому окно поиска верхних линий
//    сдвинуто вниз по каналам;
//  * центр пика — вершина гауссианы (парабола по ln над 55 % максимума);
//  * полином 4-й степени, если найдена Bi-214 1764.5 и линий >= 8, иначе парабола.
//    Bi-214 2204 не используется: на сжатом участке она тянула середину (ручной разбор).
// Бленды: рентген Pb (Ka1/Ka2/Kb) ~79 кэВ, Bi-214 1377.7/1401.5/1408.0 — средневзвешенно
// по выходам 1391.2 кэВ.
#include "autocal.h"
#include <math.h>
#include <string.h>

typedef struct { double kev; const char *name; } line_t;

// Порядок поиска после опорной пары: сначала уверенные линии рядом с ней.
enum { L_PBX, L_186, L_242, L_295, L_352, L_609, L_768, L_934, L_1120, L_1238, L_1391, L_1764, L_COUNT };
static const line_t RA226[L_COUNT] = {
    {   79.0,  "Pb X-ray"   },
    {  186.21, "Ra-226 186" },
    {  241.99, "Pb-214 242" },
    {  295.22, "Pb-214 295" },
    {  351.93, "Pb-214 352" },
    {  609.32, "Bi-214 609" },
    {  768.36, "Bi-214 768" },
    {  934.06, "Bi-214 934" },
    { 1120.29, "Bi-214 1120" },
    { 1238.11, "Bi-214 1238" },
    { 1391.2,  "Bi-214 1391*" },   // бленд 1377.7/1401.5/1408.0
    { 1764.49, "Bi-214 1764" },
};
static const int SEARCH_ORDER[] = { L_295, L_242, L_186, L_768, L_934, L_1120, L_PBX, L_1238, L_1391, L_1764 };

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

// Центр пика около c0: линейный фон по краям окна ±1.3 FWHM, парабола по ln(нетто)
// для точек выше 55 % максимума. Возвращает false, если пика нет.
static bool centroid(const uint32_t *y, int n, double c0, double *mu, double *net_out)
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
    // центровка сглаживания (сдвиг на SW/2)
    for (int i = 0; i < n - SW / 2; i++) s[i] = s[i + SW / 2];
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
        for (int j = i - h; j <= i + h && top; j++) if (j != i && (z[j] > z[i] || (z[j] == z[i] && j < i))) top = false;
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
static int pick(const cand_t *c, int nc, double lo, double hi, const int *used_by, const assign_t *as, int na, int line)
{
    int best = -1;
    for (int k = 0; k < nc; k++) {
        if (c[k].ch < lo || c[k].ch > hi || used_by[k] >= 0) continue;
        bool order_ok = true;
        for (int j = 0; j < na && order_ok; j++) {
            double eo = RA226[as[j].line].kev, e = RA226[line].kev;
            int co = c[as[j].cand].ch;
            if ((e < eo && c[k].ch >= co) || (e > eo && c[k].ch <= co)) order_ok = false;
        }
        if (!order_ok) continue;
        if (best < 0 || c[k].z > c[best].z) best = k;
    }
    return best;
}

// Модель по текущим назначениям: прямая (2 точки) или парабола; верхние линии
// (выше 1238) в модель для поиска не входят, пока не найдены сами.
static bool model(const cand_t *c, const assign_t *as, int na, double *coef, int *order)
{
    double x[L_COUNT], e[L_COUNT];
    int k = 0;
    for (int j = 0; j < na; j++) { x[k] = c[as[j].cand].ch; e[k] = RA226[as[j].line].kev; k++; }
    int deg = k >= 6 ? 2 : 1;
    if (k >= 8 && na && RA226[as[na - 1].line].kev > 1300) deg = 3;
    if (!polyfit(x, e, k, deg, coef)) return false;
    *order = deg;
    return true;
}

static int assign_from_pair(const cand_t *c, int nc, int a, int b, assign_t *as, int *used_by)
{
    for (int k = 0; k < nc; k++) used_by[k] = -1;
    int na = 0;
    as[na++] = (assign_t){ L_352, a }; used_by[a] = L_352;
    as[na++] = (assign_t){ L_609, b }; used_by[b] = L_609;
    for (size_t s = 0; s < sizeof SEARCH_ORDER / sizeof SEARCH_ORDER[0]; s++) {
        int line = SEARCH_ORDER[s];
        double coef[AUTOCAL_MAX_COEFFS];
        int order;
        if (!model(c, as, na, coef, &order)) break;
        double e = RA226[line].kev;
        double maxch = c[nc - 1].ch * 1.2 + 100;
        double pred = inv_eval(coef, order, e, 0, maxch);
        double lo, hi, fw = fwhm_ch(pred);
        if (line == L_PBX)        { lo = pred * 0.80; hi = pred * 1.20; }
        else if (line == L_1764)  { lo = pred * 0.82; hi = pred * 1.05; }   // сжатие вверху
        else if (line == L_1391)  { lo = pred * 0.90; hi = pred * 1.04; }
        else                      { lo = pred * 0.95; hi = pred * 1.05; }
        if (hi - lo < fw) { lo = pred - fw / 2; hi = pred + fw / 2; }
        int k = pick(c, nc, lo, hi, used_by, as, na, line);
        if (k < 0) continue;
        used_by[k] = line;
        as[na++] = (assign_t){ line, k };
    }
    return na;
}

int autocal_ra226(const uint32_t *bins, int n, float *ws, autocal_result_t *out)
{
    memset(out, 0, sizeof *out);
    uint64_t total = 0;
    for (int i = 0; i < n; i++) total += bins[i];
    if (n < 1024 || total < 20000) return out->err = AUTOCAL_E_NODATA;

    cand_t cand[MAX_CAND];
    int nc = find_candidates(bins, n, ws, cand);
    if (nc < 2) return out->err = AUTOCAL_E_NOPAIR;

    // Опорная пара: среди 8 самых крупных пиков — пары с отношением каналов 1.55..1.90;
    // побеждает гипотеза с наибольшим числом опознанных линий (при равенстве — площадь).
    int order_idx[MAX_CAND];
    for (int i = 0; i < nc; i++) order_idx[i] = i;
    for (int i = 0; i < nc; i++)
        for (int j = i + 1; j < nc; j++)
            if (cand[order_idx[j]].area > cand[order_idx[i]].area) { int t = order_idx[i]; order_idx[i] = order_idx[j]; order_idx[j] = t; }
    int top = nc < 8 ? nc : 8;
    assign_t best_as[L_COUNT], as[L_COUNT];
    int used_by[MAX_CAND], best_na = 0;
    double best_area = 0;
    for (int i = 0; i < top; i++)
        for (int j = 0; j < top; j++) {
            int a = order_idx[i], b = order_idx[j];
            if (cand[a].ch <= 0 || cand[b].ch <= cand[a].ch) continue;
            double r = (double)cand[b].ch / cand[a].ch;
            if (r < 1.55 || r > 1.90) continue;
            int na = assign_from_pair(cand, nc, a, b, as, used_by);
            double area = cand[a].area + cand[b].area;
            if (na > best_na || (na == best_na && area > best_area)) {
                best_na = na; best_area = area;
                memcpy(best_as, as, sizeof as);
            }
        }
    if (best_na < 2) return out->err = AUTOCAL_E_NOPAIR;

    // Точные центры и итоговый набор (по возрастанию энергии).
    for (int line = 0; line < L_COUNT; line++) {
        for (int j = 0; j < best_na; j++) {
            if (best_as[j].line != line) continue;
            double mu, net;
            if (!centroid(bins, n, cand[best_as[j].cand].ch, &mu, &net)) break;
            int k = out->n++;
            out->ch[k] = mu; out->kev[k] = RA226[line].kev; out->name[k] = RA226[line].name; out->net[k] = net;
            if (line == L_609) out->net609 = net;
            if (line == L_1764) { out->has_top = true; out->net_top = net; }
        }
    }
    if (out->n < 4) return out->err = AUTOCAL_E_FEW;

    int deg = (out->has_top && out->n >= 8) ? 4 : 2;
    for (;;) {
        double x[AUTOCAL_MAX_LINES], e[AUTOCAL_MAX_LINES];
        int k = 0;
        for (int i = 0; i < out->n; i++) {
            if (deg < 4 && out->kev[i] > 1300) continue;   // парабола — только до излома
            x[k] = out->ch[i]; e[k] = out->kev[i]; k++;
        }
        bool ok = polyfit(x, e, k, deg, out->coeffs);
        double ss = 0, mx = 0;
        int cnt = 0;
        double chmax = out->ch[out->n - 1] * 1.15;
        for (int i = 0; ok && i < out->n; i++) {
            out->fit_kev[i] = autocal_eval(out->coeffs, deg, out->ch[i]);
            if (deg < 4 && out->kev[i] > 1300) continue;
            double r = out->fit_kev[i] - out->kev[i];
            ss += r * r; cnt++;
            if (fabs(r) > mx) mx = fabs(r);
        }
        // монотонность от 0 до чуть выше последней найденной линии
        for (double c = 0; ok && c < chmax; c += 8)
            if (autocal_eval(out->coeffs, deg, c + 8) <= autocal_eval(out->coeffs, deg, c)) ok = false;
        if (ok && mx <= 15.0) {
            out->order = deg;
            out->rms_kev = sqrt(ss / cnt);
            out->max_abs_kev = mx;
            return out->err = AUTOCAL_OK;
        }
        if (deg == 4) { deg = 2; out->has_top = false; continue; }
        return out->err = AUTOCAL_E_FIT;
    }
}

bool autocal_enough(const autocal_result_t *r)
{
    return r->err == AUTOCAL_OK && r->order == 4 && r->n >= 9 && r->net609 >= 20000 && r->net_top >= 3000;
}
