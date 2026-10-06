#pragma once

#include <stdbool.h>

// #FW-2/#FW-3: поведение прошивки при старте платы (persist в NVS, namespace "boot").
// Все флаги по умолчанию false — из коробки ничего не запускается и не чистится
// (решение оператора 2026-06-29). Пользователь включает галочки один раз в Web UI
// (страница «Система» → «Поведение при старте платы»); значения переживают
// ребут / OTA / safe-mode.
// #FW-42: длина буфера префикса имён экспортов (23 значащих символа + '\0').
#define BOOT_NAME_PREFIX_CAP 24

typedef struct {
    bool autostart_spectrum;    // на первом USB-коннекте после ребута послать -sta (набор спектра)
    bool autostart_waterfall;   // на первом USB-коннекте начать запись водопада (spectrogram_start + -sta)
    bool clear_spectrum;        // при старте очистить накопленный спектр (ESP spectrum_reset + -rst прибору)
    bool clear_waterfall;       // при старте очистить водопад (spectrogram_clear)
    bool calib_always;          // #CAL-1: перед стартом набора всегда читать калибровку с прибора
    // #FW-42: префикс имени во всех экспортах (spectrum/waterfall/backup), чтобы не
    // путались файлы от разных детекторов (пример: "MY1"→"MY1spectrum.csv"). Пустая
    // строка = историческое поведение (без префикса). Санитизируется до [A-Za-z0-9_-].
    char name_prefix[BOOT_NAME_PREFIX_CAP];
} boot_config_t;

// Читает конфиг из NVS. Любой отсутствующий ключ → false / "". Безопасно вызывать
// после nvs_flash_init() (выполняется в wifi_manager_init на boot).
void boot_config_load(boot_config_t *out);

// Пишет конфиг в NVS (namespace "boot"). name_prefix санитизируется тут (в NVS
// попадают только символы [A-Za-z0-9_-], усечение до BOOT_NAME_PREFIX_CAP-1).
// Возвращает 0 при успехе, -1 при ошибке.
int  boot_config_save(const boot_config_t *in);

// #MON-2: часовой пояс отображения (смещение от UTC, мин) для экрана платы.
// Источник — настройка «Система → Часовой пояс» браузера (localStorage aswf-tz),
// её приносит POST /api/time (common-time.js). Хранится в NVS "boot"/"tz_min"
// отдельно от boot_config_t: пишется только при изменении, читается из кэша.
// Нет ключа — 0 (UTC), пока не откроется любая страница веба.
#include <stdint.h>
#define BOOT_TZ_MIN_LO  (-720)
#define BOOT_TZ_MIN_HI    840
int16_t boot_config_tz_min(void);
// #CAL-1: «всегда читать калибровку с прибора при старте набора» (кэш, NVS boot/calib_al)
bool    boot_config_calib_always(void);
void    boot_config_set_tz_min(int16_t m);   // вне диапазона — игнор
