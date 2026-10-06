#pragma once
// Минимальный тест-харнесс для host-тестов shproto (без ESP-IDF).
// CHECK() накапливает провалы в глобальном счётчике g_failures, который
// определён в test_shproto.c и виден остальным файлам набора через extern.
#include <stdio.h>

extern int g_failures;

#define CHECK(cond) do {                                              \
    if (!(cond)) {                                                    \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
        g_failures++;                                                 \
    }                                                                 \
} while (0)
