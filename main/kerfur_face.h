// #KF-1: пасхалка — мордочка Kerfur (Voices of the Void) на весь экран, выражение по
// уровню сигнализации CPS. Чистый C: маска «светодиодной матрицы» KF_W x KF_H
// (клетка экрана KF_CELL px), без LVGL — проверяется на хосте (tests/host/test_kerfur.c).
#pragma once
#include <stdint.h>

#define KF_W     96      // клеток по горизонтали (480 / 5)
#define KF_H     64      // по вертикали (320 / 5)
#define KF_CELL  5       // px на клетку: точка 4x4 + зазор 1

enum {
    KF_NORMAL = 0,   // как в игре: глаза-кольца, рот «ω»
    KF_BLINK,        // моргание: глаза — черта
    KF_BLISS,        // «блаженство»: глаза-шевроны «> <», открытый рот (как в игре при поглаживании)
    KF_HMM,          // уровень 1: прищур, рот чертой
    KF_WOW,          // уровень 2: широкие глаза, рот «о»
    KF_ANGRY,        // уровень 3: злые брови, рот зигзагом
    KF_PANIC,        // уровень 4: глаза-крестики, открытый рот, «!!»
    KF_COUNT
};

// mask: KF_W*KF_H байт, 1 — точка горит. Строка за строкой, сверху вниз.
// ws — рабочая память kerfur_face_ws_size() байт (на плате — PSRAM: внутренняя RAM
// на счету, #TFT-17; статический буфер в .bss отнимал ~1.6 КБ).
#include <stddef.h>
size_t kerfur_face_ws_size(void);
void kerfur_face_render(int expr, uint8_t *mask, void *ws);
