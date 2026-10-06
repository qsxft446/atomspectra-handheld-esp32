#pragma once
// #TFT-17: аллокатор пула LVGL в PSRAM. Принудительно подключается (-include)
// ТОЛЬКО к компоненту LVGL из корневого CMakeLists.txt, там же задаётся
// LV_MEM_POOL_ALLOC=lv_mem_pool_psram_alloc. Отдельный заголовок нужен потому,
// что CMake молча выбрасывает макросы-функции из COMPILE_DEFINITIONS, и
// "LV_MEM_POOL_ALLOC(size)=heap_caps_malloc(...)" до компилятора не доходил.
#include <stddef.h>
#include "esp_heap_caps.h"

static inline void *lv_mem_pool_psram_alloc(size_t size)
{
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
}
