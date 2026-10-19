// #HANG-1 (запрос 2026-10-19, «плата виснет при записи водопада: экран и тач замирают, веб не
// открывается, пинг есть»): детектор зависания. Ключевые задачи отмечаются (hang_watch_beat); задача,
// молчащая дольше HANG_STALL_S, — зависание: отчёт (кто молчит, состояния задач, держатели замков
// спектра/водопада/экрана, память, питание) пишется в RTC-память, плата перезагружается. Следующий
// старт выводит отчёт в лог (страница «Сервис») и дописывает в /sdcard/hang.log.
#pragma once
#include <stdbool.h>

typedef enum { HB_LVGL, HB_TFT, HB_WF, HB_WF_FS, HB_HTTPD, HB_MAIN, HB_N } hang_hb_t;

void hang_watch_boot_report(void);   // в начале app_main(): отчёт прошлого запуска -> в лог
void hang_watch_save_sd(void);       // после sd_card_init(): отчёт -> /sdcard/hang.log
void hang_watch_start(void);         // после «All subsystems initialized»
void hang_watch_beat(hang_hb_t id);  // отметка задачи (дёшево, из любой задачи)
