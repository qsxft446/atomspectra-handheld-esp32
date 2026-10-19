#pragma once
// #OTA-1 (запрос 2026-10-06; по образцу апстрима 1.2.26, AWF-4): обновление прошивки
// по WiFi загрузкой файла со страницы «Система» (карточка ESP32-S3).
//   POST /api/ota   тело — atomspectra_gw.bin как есть (application/octet-stream), CSRF.
// Плата пишет образ потоком в неактивный OTA-раздел, проверяет сигнатуру, чип и
// целостность сегментов, затем esp_ota_end() (SHA-256), ставит раздел на загрузку
// и перезагружается. Новая прошивка стартует «на испытании»: если она не проработает
// 30 с (main.c), загрузчик вернёт прежнюю (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE).
// Обновления с GitHub апстрима здесь НЕТ намеренно: та прошивка затёрла бы нашу.
#include "esp_http_server.h"
#include <stdbool.h>

void ota_update_register(httpd_handle_t server);
// Таблица разделов с OTA (ota_0/ota_1) — иначе загрузка по WiFi невозможна.
bool ota_update_supported(void);
bool ota_update_busy(void);   // #HANG-1: идёт приём образа
// Метка раздела, с которого работает текущая прошивка ("ota_0", "factory", …).
const char *ota_update_running_label(void);
