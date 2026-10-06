#pragma once
// #SD-1: общая шина SPI2_HOST модуля дисплея lcdwiki 3.5" ST7796: на ней и
// панель (tft_display.c, LCD_CS=GPIO4), и слот SD-карты модуля (sd_card.c,
// SD_CS=GPIO10). MOSI/SCK/MISO у них общие — это одни и те же выводы 6/7/9
// разъёма модуля. Шину поднимает первый, кому она нужна (обычно
// tft_display_init()); второй вызов spi_bus_initialize() отдаёт
// ESP_ERR_INVALID_STATE — это норма, не ошибка.
#ifndef BOARD_SPI_SCLK
#define BOARD_SPI_SCLK   12
#endif
#ifndef BOARD_SPI_MOSI
#define BOARD_SPI_MOSI   11
#endif
#ifndef BOARD_SPI_MISO
#define BOARD_SPI_MISO   13   // нужен только SD-карте: из панели прошивка не читает
#endif
#define BOARD_SPI_HOST   SPI2_HOST
// #SD-5: карта теперь поднимает шину РАНЬШЕ дисплея (водопад пишется на неё и
// восстанавливается при старте), поэтому параметры шины одинаковы у обоих:
// кто первый — тот и настроил. Размер — полоса LVGL 480 x 20 строк x 2 Б.
#define BOARD_SPI_MAX_TRANSFER  (480 * 20 * 2)
// CS панели: пока дисплей не инициализирован, вывод висит, и панель могла бы
// принять команды карты за свои и отвечать на общем MISO. sd_card_init()
// держит его в 1 до инициализации дисплея.
#define BOARD_SPI_LCD_CS 4
