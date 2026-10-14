// #KF-1 (запрос 2026-10-14): мордочки Kerfur для пасхального режима экрана.
// Облик — по вики Voices of the Void и снимку пользователя: чёрный экран, мордочка из
// голубых «светодиодов»: глаза-кольца, брови-овалы, нос, рот «ω», по две черты усов;
// «блаженство» — глаза-шевроны и открытый рот. Выражения уровней 1-4 придуманы в том
// же стиле (макет согласован с пользователем). Координаты фигур — в пикселях экрана
// 480x320 (как в макете), растеризация — по центрам клеток KF_CELL.
#include "kerfur_face.h"
#include <math.h>
#include <string.h>

#define MAXSH 48

typedef enum { SH_RING, SH_SEG, SH_ELL, SH_TRI, SH_DISK } shape_kind_t;
typedef struct {
    shape_kind_t k;
    float a, b, c, d, e, f, t;   // смысл — по виду фигуры
} shape_t;

typedef struct { shape_t s[MAXSH]; int n; } face_t;

static void add(face_t *F, shape_kind_t k, float a, float b, float c, float d, float e, float f, float t)
{
    if (F->n < MAXSH) F->s[F->n++] = (shape_t){ k, a, b, c, d, e, f, t };
}
// кольцо: центр, радиус, толщина
static void ring(face_t *F, float x, float y, float r, float t) { add(F, SH_RING, x, y, r, 0, 0, 0, t); }
// отрезок с круглыми концами
static void seg(face_t *F, float x0, float y0, float x1, float y1, float t) { add(F, SH_SEG, x0, y0, x1, y1, 0, 0, t); }
// закрашенный эллипс
static void ell(face_t *F, float x, float y, float rx, float ry) { add(F, SH_ELL, x, y, rx, ry, 0, 0, 0); }
static void disk(face_t *F, float x, float y, float r) { add(F, SH_DISK, x, y, r, 0, 0, 0, 0); }
// закрашенный треугольник
static void tri(face_t *F, float x0, float y0, float x1, float y1, float x2, float y2)
{
    add(F, SH_TRI, x0, y0, x1, y1, x2, y2, 0);
}
// квадратичная кривая Безье — цепочкой отрезков
static void quad(face_t *F, float x0, float y0, float cx, float cy, float x1, float y1, float t)
{
    float px = x0, py = y0;
    for (int i = 1; i <= 8; i++) {
        float u = i / 8.0f, v = 1 - u;
        float x = v * v * x0 + 2 * v * u * cx + u * u * x1;
        float y = v * v * y0 + 2 * v * u * cy + u * u * y1;
        seg(F, px, py, x, y, t);
        px = x; py = y;
    }
}

static void whiskers(face_t *F, float y)
{
    seg(F, 60, y, 100, y, 11);       seg(F, 64, y + 28, 100, y + 28, 11);
    seg(F, 380, y, 420, y, 11);      seg(F, 380, y + 28, 416, y + 28, 11);
}

static void build(int expr, face_t *F)
{
    F->n = 0;
    switch (expr) {
    case KF_NORMAL:
        ring(F, 165, 140, 48, 20); ring(F, 315, 140, 48, 20);
        ell(F, 150, 66, 18, 9); ell(F, 330, 66, 18, 9);
        tri(F, 240, 172, 252, 184, 228, 184); tri(F, 228, 184, 252, 184, 240, 196);   // нос-ромб
        quad(F, 200, 206, 220, 228, 240, 206, 12); quad(F, 240, 206, 260, 228, 280, 206, 12);
        whiskers(F, 186);
        break;
    case KF_BLINK:
        seg(F, 125, 146, 205, 146, 16); seg(F, 275, 146, 355, 146, 16);
        ell(F, 150, 70, 18, 9); ell(F, 330, 70, 18, 9);
        tri(F, 240, 172, 252, 184, 228, 184); tri(F, 228, 184, 252, 184, 240, 196);
        quad(F, 200, 206, 220, 228, 240, 206, 12); quad(F, 240, 206, 260, 228, 280, 206, 12);
        whiskers(F, 186);
        break;
    case KF_BLISS:   // как на снимке из игры: «> <», брови-точки, нос, открытый рот
        seg(F, 110, 100, 202, 142, 28); seg(F, 202, 142, 110, 184, 28);
        seg(F, 370, 100, 278, 142, 28); seg(F, 278, 142, 370, 184, 28);
        ell(F, 200, 80, 15, 9); ell(F, 280, 80, 15, 9);
        tri(F, 228, 180, 252, 180, 240, 194);
        quad(F, 214, 214, 208, 248, 240, 248, 12); quad(F, 240, 248, 272, 248, 266, 214, 12);
        quad(F, 214, 214, 227, 226, 240, 214, 12); quad(F, 240, 214, 253, 226, 266, 214, 12);
        whiskers(F, 196);
        break;
    case KF_HMM:     // прищур: нижняя половина кольца + черта сверху
        for (int i = 0; i <= 10; i++) {
            float a0 = 3.14159f * i / 10, a1 = 3.14159f * (i + 1) / 10;
            if (i == 10) break;
            seg(F, 165 + 46 * cosf(a0), 140 + 46 * sinf(a0), 165 + 46 * cosf(a1), 140 + 46 * sinf(a1), 18);
            seg(F, 315 + 46 * cosf(a0), 140 + 46 * sinf(a0), 315 + 46 * cosf(a1), 140 + 46 * sinf(a1), 18);
        }
        seg(F, 119, 140, 211, 140, 18); seg(F, 269, 140, 361, 140, 18);
        seg(F, 125, 92, 177, 102, 12); seg(F, 355, 92, 303, 102, 12);
        tri(F, 240, 170, 252, 182, 228, 182); tri(F, 228, 182, 252, 182, 240, 194);
        seg(F, 205, 210, 275, 210, 11);
        whiskers(F, 182);
        break;
    case KF_WOW:     // широкие глаза со зрачком, рот «о», брови вверх
        ring(F, 165, 140, 54, 18); ring(F, 315, 140, 54, 18);
        disk(F, 165, 140, 13); disk(F, 315, 140, 13);
        seg(F, 118, 72, 168, 56, 12); seg(F, 362, 72, 312, 56, 12);
        ring(F, 240, 226, 17, 11);
        whiskers(F, 190);
        break;
    case KF_ANGRY:   // злые брови, глаза поменьше, рот зигзагом
        ring(F, 165, 150, 40, 18); ring(F, 315, 150, 40, 18);
        seg(F, 115, 80, 193, 110, 14); seg(F, 365, 80, 287, 110, 14);
        for (int i = 0; i < 6; i++)   // зигзаг из 6 отрезков, вершина ровно по центру (x 240)
            seg(F, 192.0f + 16 * i, (i % 2) ? 202.0f : 216.0f, 208.0f + 16 * i, (i % 2) ? 216.0f : 202.0f, 11);
        whiskers(F, 186);
        break;
    case KF_PANIC:   // глаза-крестики, открытый рот, «!!»
        seg(F, 130, 100, 200, 170, 18); seg(F, 200, 100, 130, 170, 18);
        seg(F, 280, 100, 350, 170, 18); seg(F, 350, 100, 280, 170, 18);
        quad(F, 195, 205, 240, 275, 285, 205, 12); seg(F, 195, 205, 285, 205, 12);
        seg(F, 225, 22, 225, 50, 10); disk(F, 225, 64, 6);
        seg(F, 255, 22, 255, 50, 10); disk(F, 255, 64, 6);
        whiskers(F, 186);
        break;
    default: break;
    }
}

static float dist_seg(float px, float py, const shape_t *s)
{
    float vx = s->c - s->a, vy = s->d - s->b, wx = px - s->a, wy = py - s->b;
    float l2 = vx * vx + vy * vy, u = l2 > 0 ? (wx * vx + wy * vy) / l2 : 0;
    if (u < 0) u = 0;
    if (u > 1) u = 1;
    float dx = wx - u * vx, dy = wy - u * vy;
    return sqrtf(dx * dx + dy * dy);
}

static float sgn(float x0, float y0, float x1, float y1, float x2, float y2)
{
    return (x0 - x2) * (y1 - y2) - (x1 - x2) * (y0 - y2);
}

static int inside(const shape_t *s, float x, float y)
{
    switch (s->k) {
    case SH_RING: { float d = hypotf(x - s->a, y - s->b) - s->c; return fabsf(d) <= s->t / 2; }
    case SH_SEG:  return dist_seg(x, y, s) <= s->t / 2;
    case SH_ELL:  { float u = (x - s->a) / s->c, v = (y - s->b) / s->d; return u * u + v * v <= 1; }
    case SH_DISK: return hypotf(x - s->a, y - s->b) <= s->c;
    case SH_TRI: {
        float d1 = sgn(x, y, s->a, s->b, s->c, s->d), d2 = sgn(x, y, s->c, s->d, s->e, s->f),
              d3 = sgn(x, y, s->e, s->f, s->a, s->b);
        int neg = d1 < 0 || d2 < 0 || d3 < 0, pos = d1 > 0 || d2 > 0 || d3 > 0;
        return !(neg && pos);
    }
    }
    return 0;
}

size_t kerfur_face_ws_size(void) { return sizeof(face_t); }

void kerfur_face_render(int expr, uint8_t *mask, void *ws)
{
    face_t *Fp = (face_t *)ws;   // ~1.6 КБ — от вызывающего (PSRAM), не на стеке задачи LVGL
#define F (*Fp)
    build(expr, &F);
    for (int gy = 0; gy < KF_H; gy++)
        for (int gx = 0; gx < KF_W; gx++) {
            float x = gx * KF_CELL + KF_CELL / 2.0f, y = gy * KF_CELL + KF_CELL / 2.0f;
            uint8_t on = 0;
            for (int i = 0; i < F.n && !on; i++) on = (uint8_t)inside(&F.s[i], x, y);
            mask[gy * KF_W + gx] = on;
        }
#undef F
}
