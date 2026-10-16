// #KF-1: host-тест мордочек Kerfur — печать масок текстом (глазами сверить с макетом)
// и простые проверки: всё внутри экрана, левая/правая половины симметричны,
// выражения различаются. cl /W4 /I ..\..\main test_kerfur.c ..\..\main\kerfur_face.c
#include <stdio.h>
#include <string.h>
#include "kerfur_face.h"

int g_failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_failures++; } } while (0)

static const char *NAMES[KF_COUNT] = { "NORMAL", "BLINK", "BLISS", "HMM (1)", "WOW (2)", "ANGRY (3)", "PANIC (4)", "GRUMPY" };

int main(int argc, char **argv)
{
    static uint8_t m[KF_COUNT][KF_W * KF_H];
    (void)argv;
    int show = argc > 1;
    for (int e = 0; e < KF_COUNT; e++) {
        static unsigned char ws[4096];
        CHECK(kerfur_face_ws_size() <= sizeof ws);
        kerfur_face_render(e, m[e], ws);
        int on = 0, asym = 0;
        for (int y = 0; y < KF_H; y++)
            for (int x = 0; x < KF_W; x++) {
                on += m[e][y * KF_W + x];
                if (m[e][y * KF_W + x] != m[e][y * KF_W + (KF_W - 1 - x)]) asym++;
            }
        printf("%-10s lit %4d cells, asymmetric %d\n", NAMES[e], on, asym);
        CHECK(on > 150 && on < KF_W * KF_H / 3);
        // «!!» у паники смещены; остальное симметрично с точностью до растра
        if (e != KF_PANIC) CHECK(asym < on / 10);
        if (show) {
            for (int y = 0; y < KF_H; y += 2) {   // через строку — пропорции терминала
                for (int x = 0; x < KF_W; x++) putchar(m[e][y * KF_W + x] || m[e][(y + 1) * KF_W + x] ? '#' : '.');
                putchar('\n');
            }
        }
    }
    for (int a = 0; a < KF_COUNT; a++)
        for (int b = a + 1; b < KF_COUNT; b++) CHECK(memcmp(m[a], m[b], sizeof m[a]) != 0);
    if (g_failures) { printf("KERFUR: %d FAILED\n", g_failures); return 1; }
    printf("KERFUR: ALL PASSED\n");
    return 0;
}
