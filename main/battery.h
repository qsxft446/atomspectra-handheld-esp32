#pragma once
// #PWR-1: модуль бесперебойного питания ClimateGuard CG_Charger_BQ-2v0 (TI BQ25895) —
// опциональная плата, подключаемая к ESP32-S3 по I2C. Даёт заряд Li-ion 1S,
// бесперебойный выход и телеметрию заряда/напряжения АКБ для значка батареи
// в шапке Web UI + подробную панель на странице «Система». Если плата физически
// не подключена — probe() на старте не проходит, present=false, остальная
// прошивка не видит разницы.
//
// #PWR-2 (2v0): тот же чип и та же карта регистров, что и в cg-bq-1v0 — I2C-логика
// не меняется. У 2v0 добавлены выведенные на разъём пины OTG/!CE/INT/NTC, но все
// они снабжены собственными подтяжками на плате (OTG=1 boost enable, !CE=0 charge
// enable по умолчанию) — для чтения телеметрии их подключать не обязательно,
// достаточно SDA/SCL/GND + питание. NTC-термистор опционален (см. п.7 датащита);
// если не подключён — TSPCT читается, но не отражает реальную температуру.

#include <stdint.h>
#include <stdbool.h>

// #DISP-1: пины I2C-шины (общая с SSD1306) — см. board_i2c.h. Battery.c её не
// поднимает сам, а получает готовую через board_i2c_get_bus().

// Расшифровка battery_status_t.vbus_stat (REG0B[7:5], см. п.11 датащита)
#define BATT_VBUS_NONE          0   // No Input
#define BATT_VBUS_USB_SDP       1   // USB Host SDP (500mA)
#define BATT_VBUS_USB_CDP       2   // USB CDP (1.5A)
#define BATT_VBUS_USB_DCP       3   // USB DCP (3.25A)
#define BATT_VBUS_MAXCHARGE     4   // Adjustable High Voltage DCP (1.5A)
#define BATT_VBUS_UNKNOWN       5   // Unknown Adapter (500mA)
#define BATT_VBUS_NONSTANDARD   6   // Non-Standard Adapter
#define BATT_VBUS_OTG           7   // OTG (питание идёт из АКБ наружу)

// Расшифровка battery_status_t.fault_chrg (REG0C[5:4])
#define BATT_CHRGFAULT_NORMAL       0
#define BATT_CHRGFAULT_INPUT        1   // VBUS>VACOV или VBAT<VBUS<VVBUSMIN
#define BATT_CHRGFAULT_THERMAL      2   // Тепловое отключение
#define BATT_CHRGFAULT_SAFETY_TMR   3   // Истёк таймер безопасности заряда

typedef struct {
    bool    present;        // BQ25895 отвечает на I2C (плата подключена)

    bool    vbus_present;   // VBUS_GD (REG11 бит7) — внешнее питание физически подано
    bool    power_good;     // PG_STAT (REG0B бит2) — система реально запитана от входа
    uint8_t vbus_stat;      // тип источника питания, см. BATT_VBUS_*

    bool    charging;       // идёт заряд АКБ (pre-charge или fast-charge)
    bool    chg_done;       // заряд завершён (termination done)

    float   voltage;        // напряжение АКБ, В (ADC BATV)
    int     percent;        // грубая оценка заряда 0-100% по напряжению (у BQ25895 нет fuel gauge)
    float   vbus_voltage;   // напряжение на входе VBUS, В (ADC VBUSV); 0, если vbus_present=false
    float   vsys_voltage;   // системное напряжение платы (ADC SYSV), В
    float   ntc_pct;        // TS как % от REGN (только если подключён внешний NTC, п.7 датащита); -1 = не читалось

    bool    fault_watchdog; // истёк I2C watchdog-таймер
    bool    fault_boost;    // ошибка режима OTG/boost
    uint8_t fault_chrg;     // см. BATT_CHRGFAULT_*
    bool    fault_bat;      // BATOVP — перенапряжение АКБ
    uint8_t fault_ntc;      // код ошибки NTC (0 = норма), см. REG0C[2:0] датащита

    // #PWR-9 (отладка питания прибора от АКБ, 2026-09-30): сырые регистры 0x00..0x14
    // и счётчики неисправностей с момента старта. REG0C читается раз в 2 с и
    // сбрасывается чтением — короткий сбой в снапшоте fault_* легко пропустить.
    uint8_t  regs[21];
    uint32_t boost_faults;   // сколько опросов видели BOOST_FAULT
    uint32_t chrg_faults;    // ... CHRG_FAULT != 0
    uint32_t wd_faults;      // ... WATCHDOG_FAULT
    int     iinlim_ma;      // #PWR-6: текущий предел входного тока IINLIM (REG00[5:0]), мА.
                             // Может быть переписан самим чипом при автоопределении источника
                             // (см. datasheet REG00) или алгоритмом ICO — читаем каждый цикл,
                             // а не только то, что сами когда-то записали.
} battery_status_t;

// Поднимает шину I2C и фоновую задачу опроса BQ25895. Безопасно вызывать даже
// если плата CG_Charger_BQ-2v0 физически не подключена — probe провалится тихо,
// battery_get_status() будет всегда возвращать present=false.
void battery_init(void);

// Потокобезопасный снапшот последнего опроса (обновляется каждые ~2 с).
void battery_get_status(battery_status_t *out);

// #PWR-6: ручная установка лимита входного тока IINLIM (REG00[5:0]) поверх
// автоопределения BQ25895. Шаг регистра — 50мА, офсет 100мА (см. datasheet
// REG00) — ma округляется вниз до ближайшего валидного шага и зажимается в
// [100..3250]. Потокобезопасно (защищено тем же I2C-мьютексом, что и фоновый
// опрос). Возвращает фактически применённое значение в мА (после округления),
// или -1, если плата не подключена / запись не удалась — так вызывающий код
// сразу знает точный результат, не дожидаясь следующего цикла опроса (~2с).
int battery_set_iinlim(uint16_t ma);
