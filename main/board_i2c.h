#pragma once
// #DISP-1: общая I2C-шина платы. Раньше её поднимал только battery.c под себя;
// вынесена сюда, т.к. SSD1306 (oled_display.c) садится на ту же физическую
// шину другим адресом (0x3C) — двух i2c_new_master_bus() на одном порту делать
// нельзя, ESP-IDF отдаёт ESP_ERR_INVALID_STATE на второй вызов.

#include "driver/i2c_master.h"

// Свободные GPIO на большинстве плат ESP32-S3-N16R8 (не заняты октальной PSRAM
// [GPIO33-37], SPI flash [GPIO26-32] или USB OTG [GPIO19/20]). Если на вашей
// плате эти пины заняты другой периферией — поменяйте здесь и пересоберите.
#ifndef BOARD_I2C_SDA_GPIO
#define BOARD_I2C_SDA_GPIO   8
#endif
#ifndef BOARD_I2C_SCL_GPIO
#define BOARD_I2C_SCL_GPIO   9
#endif

// Возвращает общую шину I2C, создавая её при первом вызове (потокобезопасно).
// NULL, если инициализация не удалась (например, пины заняты чем-то ещё) —
// вызывающий должен считать соответствующую периферию отсутствующей и не падать.
i2c_master_bus_handle_t board_i2c_get_bus(void);
