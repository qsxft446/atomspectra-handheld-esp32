#pragma once
// #SD-3: запись на SD-карту модуля дисплея (sd_card.h) — мониторинг.
// #SD-7 (решение пользователя 2026-10-06): CSV-копия каждого сохранённого спектра
// (/sdcard/spectra/...csv) больше не пишется — с #SD-4 сам spec_NNNN.bin лежит на
// карте, а CSV из него даёт «Export CSV» на странице «Файлы». Старые CSV на карте
// не трогаем.
//   /sdcard/monitor/<префикс>cps_monitor_ГГГГММДД_ччммсс.csv — сессия
//     мониторинга: каждый закрытый интервал дописывается сразу; колонки и
//     формат — как «Выгрузить CSV» в web/monitor.html; итог сессии — на Стопе.
// Время в строках и именах — в поясе отображения из веба (boot_config_tz_min).
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// Мониторинг. Зовёт tft_display.c из tft_task — там уже собираются интервалы
// общей сессии (#MON-2) той же логикой, что в вебе.
void sd_log_mon_new_session(bool running, uint16_t interval_s);   // сессия сменилась
void sd_log_mon_interval(uint32_t dev_start, uint32_t dev_end, uint32_t dur,
                         uint32_t counts, uint32_t up_end);          // закрыт интервал
void sd_log_mon_stop(void);                                           // Стоп: итог, закрыть
bool sd_log_mon_active(void);                                         // файл сессии открыт
// #FS-1: имя (без каталога) открытого сейчас файла мониторинга; false — не открыт.
// Браузер файлов не даёт его удалить (FATFS_FS_LOCK=0 — FatFs сам не защищает).
bool sd_log_mon_file(char *out, size_t cap);
