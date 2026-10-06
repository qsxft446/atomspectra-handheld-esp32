cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(atomspectra_gw)

# #TFT-17: пул LVGL (CONFIG_LV_MEM_SIZE_KILOBYTES = 32 КБ) — в PSRAM, а не
# статическим массивом во ВНУТРЕННЕЙ RAM. По карте компоновки work_mem_int
# занимал 0x8000 в .bss внутренней DRAM, и вместе с остальным дисплеем оставлял
# системе ~4.3 КБ: WiFi берёт буферы приёма (1600 Б) по требованию из этой же
# памяти, и TCP задыхался при живом пинге (замер soak R1, 2026-09-28).
# lv_mem.c сначала проверяет LV_MEM_POOL_ALLOC (защищён #ifndef в
# lv_conf_internal.h) и, если он задан, статический массив не создаёт вовсе.
# Задаём только для компонента LVGL — чужой код в managed_components не правим.
# Макрос обязан быть ОБЪЕКТНЫМ: макросы-функции CMake из COMPILE_DEFINITIONS
# молча выбрасывает (сборка при этом зелёная, а пул остаётся во внутренней RAM).
# Проверка результата — work_mem_int НЕ должен встречаться в build/*.map.
idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
target_compile_options(${lvgl_lib} PRIVATE -include ${CMAKE_SOURCE_DIR}/main/lv_mem_pool_psram.h)
target_compile_definitions(${lvgl_lib} PRIVATE LV_MEM_POOL_ALLOC=lv_mem_pool_psram_alloc)
