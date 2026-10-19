#pragma once
// #SD-1: слот microSD на модуле дисплея lcdwiki 3.5" ST7796 (SPI, общая шина
// board_spi.h, SD_CS=GPIO10). FAT монтируется в /sdcard. Карта опциональна:
// нет карты / не читается — present=false, остальная прошивка не меняется.
// Карту НЕ форматируем сами (format_if_mount_failed=false) — на ней могут быть
// чужие данные. Горячая замена не поддерживается: карта ищется при старте.
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define SD_MOUNT_POINT "/sdcard"

// Вызывать из app_main() ПОСЛЕ tft_display_init() (шина SPI уже поднята;
// если дисплей не поднялся — модуль поднимет шину сам).
void sd_card_init(void);

bool sd_card_present(void);   // карта смонтирована
// #HANG-1: задача в транзакции карты, с какого момента (мкс), ошибок транзакций с запуска;
// шаг: 0 — нет транзакции, 1 — ждёт замок шины SPI (= замок экрана), 2 — обмен с картой.
int sd_card_txn_state(void **task, int64_t *since, uint32_t *errs);
// Объём и свободное место, байт. false — карты нет.
bool sd_card_space(uint64_t *total, uint64_t *free_bytes);
const char *sd_card_name(void);   // имя карты из CID ("" — нет карты)
// #FS-1: путь FatFs ("N:/wf") для пути VFS ("/sdcard/wf") — для f_readdir, который
// отдаёт размер и дату без stat() на каждый файл. false — карты нет или путь не на ней.
bool sd_card_ff_path(const char *vfs_path, char *out, size_t cap);
