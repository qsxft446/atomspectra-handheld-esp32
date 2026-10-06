// #PWR-1/#PWR-2: опрос ClimateGuard CG_Charger_BQ-2v0 (TI BQ25895) по I2C для
// значка батареи в шапке Web UI и подробной панели на странице «Система».
// Регистры и адрес — см. cg_charger_bq_2v0_datasheet (REG0B/REG0C/REG0E/REG0F/
// REG10/REG11/REG14, дефолтный адрес 0x6A). Карта регистров идентична cg-bq-1v0.
#include "battery.h"
#include "board_i2c.h"     // #DISP-1: общая I2C-шина (шарится с oled_display.c)
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "nvs.h"           // #PWR-7: сохранение выбранного IINLIM между перезагрузками ESP32
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "battery";

#define BQ25895_I2C_ADDR   0x6A
#define REG_00_INPUT       0x00   // bit7 EN_HIZ, bit6 EN_ILIM, bits5:0 IINLIM[5:0] (offset 100мА, шаг 50мА)
#define REG_02_CONV        0x02   // bit7 CONV_START, bit6 CONV_RATE
#define REG_03_CTRL        0x03   // bit6 WD_RST — "кормим" I2C watchdog (REG07 WATCHDOG[1:0], default 40с)
#define REG_0B_STATUS      0x0B   // VBUS_STAT/CHRG_STAT/PG_STAT/VSYS_STAT
#define REG_0C_FAULT       0x0C   // WATCHDOG_FAULT/BOOST_FAULT/CHRG_FAULT/BAT_FAULT/NTC_FAULT
#define REG_0E_BATV        0x0E   // ADC-конверсия напряжения АКБ
#define REG_0F_SYSV        0x0F   // ADC-конверсия системного напряжения VSYS
#define REG_10_TSPCT       0x10   // ADC-конверсия TS (NTC) как % REGN
#define REG_11_VBUSV       0x11   // bit7 VBUS_GD, [6:0] ADC VBUS
#define REG_14_PN          0x14   // PN[2:0] — идентификация чипа (BQ25895=111)

static i2c_master_dev_handle_t s_dev;
static SemaphoreHandle_t       s_mutex;      // защищает публикуемый снапшот s_status
static SemaphoreHandle_t       s_i2c_mutex;  // #PWR-6: защищает саму шину I2C — poll_task (фон) и
                                              // battery_set_iinlim() (вызывается из HTTP-обработчика,
                                              // другая задача) обращаются к одному i2c_master_dev_handle_t
static battery_status_t        s_status = { .present = false };

// #PWR-7: желаемый (запомненный) ток заряда — переживает reboot ESP32 (NVS) и
// переприменяется автоматически при (пере)инициализации модуля и при переходе
// в состояние заряда (см. poll_task) — т.к. сам чип может переписать IINLIM
// при автоопределении источника питания.
#define BATT_NVS_NS "battery"
static int s_desired_iinlim_ma = 500;   // дефолт, пока не загружен из NVS / не установлен пользователем

static void load_iinlim_from_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(BATT_NVS_NS, NVS_READONLY, &h) != ESP_OK) return;   // неймспейса ещё нет — оставляем дефолт 500
    uint16_t v = 0;
    if (nvs_get_u16(h, "iinlim_ma", &v) == ESP_OK && v >= 100 && v <= 3250) {
        s_desired_iinlim_ma = v;
    }
    nvs_close(h);
}

static void save_iinlim_to_nvs(int ma)
{
    nvs_handle_t h;
    if (nvs_open(BATT_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u16(h, "iinlim_ma", (uint16_t)ma);
    nvs_commit(h);
    nvs_close(h);
}

static bool reg_read(uint8_t reg, uint8_t *val)
{
    if (!s_dev) return false;
    bool locked = s_i2c_mutex && xSemaphoreTake(s_i2c_mutex, pdMS_TO_TICKS(500)) == pdTRUE;
    // #PWR-3 (debug fix): i2c_master_transmit_receive() ожидает xfer_timeout_ms
    // в МИЛЛИСЕКУНДАХ напрямую (не тики FreeRTOS) — pdMS_TO_TICKS(100) здесь
    // тихо ужимал реальный таймаут до ~10мс (при дефолтном tick rate 100Гц),
    // из-за чего единичная задержка на шине читалась как "модуля нет".
    esp_err_t err = i2c_master_transmit_receive(s_dev, &reg, 1, val, 1, 100);
    if (locked) xSemaphoreGive(s_i2c_mutex);
    return err == ESP_OK;
}

static bool reg_write(uint8_t reg, uint8_t val)
{
    if (!s_dev) return false;
    bool locked = s_i2c_mutex && xSemaphoreTake(s_i2c_mutex, pdMS_TO_TICKS(500)) == pdTRUE;
    uint8_t buf[2] = { reg, val };
    esp_err_t err = i2c_master_transmit(s_dev, buf, 2, 100);
    if (locked) xSemaphoreGive(s_i2c_mutex);
    return err == ESP_OK;
}

// Грубая оценка % заряда 1S Li-ion по напряжению (у BQ25895 нет fuel-gauge/coulomb
// counter — только ADC напряжения). Кривая — линейная аппроксимация разряда
// 3.30-4.20В; ниже 3.30В почти разряжен, дальше жать нечего.
static int voltage_to_percent(float v)
{
    const float vmin = 3.30f, vmax = 4.20f;
    if (v <= vmin) return 0;
    if (v >= vmax) return 100;
    return (int)((v - vmin) / (vmax - vmin) * 100.0f + 0.5f);
}

static bool probe_once(void)
{
    uint8_t pn = 0;
    if (!reg_read(REG_14_PN, &pn)) return false;
    // PN[2:0] = биты 5:3 REG14; для BQ25895 = 111
    uint8_t pn_bits = (pn >> 3) & 0x07;
    return pn_bits == 0x07;
}

// #PWR-4 (debug fix): у BQ25895 есть встроенный I2C watchdog (REG07 WATCHDOG[1:0],
// по умолчанию 40с) — если хост не пишет в чип достаточно часто, чип решает, что
// прошивка зависла, и откатывает параметры заряда/АЦП на заводские значения
// (в т.ч. гасит наш continuous-режим АЦП, REG02 CONV_RATE). Раньше мы писали в
// чип только один раз при старте (включение АЦП) и дальше только читали — через
// 40с watchdog неизбежно истекал (REG0C WATCHDOG_FAULT=1) и опрос "протухал".
// Кормим его явным битом WD_RST (REG03 бит6, сам обнуляется после сброса
// таймера) на каждом цикле опроса (2с — большой запас до 40с). Read-modify-write,
// чтобы не задеть OTG_CONFIG/CHG_CONFIG, которые тоже живут в REG03.
static void kick_watchdog(void)
{
    uint8_t r03 = 0;
    if (reg_read(REG_03_CTRL, &r03)) {
        reg_write(REG_03_CTRL, (uint8_t)(r03 | 0x40));
    }
}

// #PWR-6/#PWR-7: низкоуровневая запись IINLIM в REG00 (без обращения к NVS и
// без обновления s_desired_iinlim_ma) — используется и публичным
// battery_set_iinlim() (по запросу пользователя, с сохранением в NVS), и
// внутренним автоприменением сохранённого значения (инициализация модуля,
// переход в заряд — там NVS трогать не нужно, значение и так оттуда взято).
// Возвращает применённое (округлённое до шага регистра) значение в мА, либо
// -1 при ошибке.
static int write_iinlim_raw(int ma)
{
    if (ma < 100) ma = 100;
    if (ma > 3250) ma = 3250;   // максимум по датащиту (REG00 IINLIM[5:0]=111111)
    // код = (ma-100)/50, округление вниз до ближайшего валидного шага
    uint8_t code = (uint8_t)((ma - 100) / 50);
    if (code > 0x3F) code = 0x3F;
    int applied_ma = 100 + (int)code * 50;

    uint8_t r00 = 0;
    if (!reg_read(REG_00_INPUT, &r00)) return -1;
    // Меняем только IINLIM[5:0], сохраняя EN_HIZ (бит7) и EN_ILIM (бит6) как есть.
    uint8_t new_r00 = (uint8_t)((r00 & 0xC0) | code);
    if (!reg_write(REG_00_INPUT, new_r00)) return -1;
    return applied_ma;
}

static void poll_task(void *arg)
{
    (void)arg;
    bool init_done_conv = false;
    // #PWR-5 (debug fix): BATV имеет разрешение всего 20мВ/бит (см. REG0E) —
    // если реальное напряжение сидит у границы кода АЦП, шум (ripple от DC-DC,
    // броски тока при Wi-Fi TX) заставляет одиночный отсчёт "прыгать" между
    // соседними кодами (напр. 3.98/4.00/4.02). Внешний вольтметр это сглаживает
    // сам, наш опрос — нет. Сглаживаем экспоненциальным средним по нескольким
    // циклам опроса, чтобы не показывать этот шум как реальные скачки заряда.
    float ema_v = -1.0f;
    // #PWR-7: следим за переходом "не заряжается" → "заряжается", чтобы
    // переприменить сохранённый IINLIM (чип мог сам переписать регистр при
    // автоопределении источника после переподключения питания).
    bool prev_charging = false;

    for (;;) {
        bool present = probe_once();
        bool just_enabled_conv = false;

        if (present) {
            kick_watchdog();
        }

        if (present && !init_done_conv) {
            // #PWR-1: включаем непрерывную АЦП-конверсию (REG02: CONV_START=1,
            // CONV_RATE=1 → 1с период), сохраняя остальные биты read-modify-write.
            uint8_t r02 = 0;
            if (reg_read(REG_02_CONV, &r02)) {
                reg_write(REG_02_CONV, (uint8_t)(r02 | 0xC0));
                init_done_conv = true;
                just_enabled_conv = true;
                // #PWR-7: при (пере)инициализации модуля отправляем сохранённое
                // в NVS значение IINLIM — переживает reboot ESP32.
                write_iinlim_raw(s_desired_iinlim_ma);
            }
        }
        if (!present) {
            init_done_conv = false;
            ema_v = -1.0f;   // плата отключилась/пропала — начнём фильтр заново при повторном обнаружении
            prev_charging = false;   // #PWR-7: следующее обнаружение = новая "инициализация"
        }

        // #PWR-3 (debug fix): в цикле, где мы только что включили конверсию,
        // BATV/VBUSV/SYSV/TSPCT ещё могут быть дефолтными (нулевыми) — первая
        // конверсия завершится только через ~1с. Без этой проверки значок на
        // секунду-две мигал бы 0%/красным сразу после обнаружения платы или
        // hot-plug. Публикуем present=true (чтобы чип/панель появились), но
        // измерения — только со следующего цикла (через 2с), когда ADC точно
        // готов.
        if (just_enabled_conv) {
            battery_status_t warm = { 0 };
            warm.present = true;
            warm.ntc_pct = -1.0f;
            if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
                s_status = warm;
                xSemaphoreGive(s_mutex);
            }
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        battery_status_t st = { 0 };
        st.present = present;
        st.ntc_pct = -1.0f;

        if (present) {
            uint8_t r0b = 0, r0c = 0, r0e = 0, r0f = 0, r10 = 0, r11 = 0, r00 = 0;
            bool ok_status = reg_read(REG_0B_STATUS, &r0b);
            bool ok_fault  = reg_read(REG_0C_FAULT, &r0c);
            bool ok_batv   = reg_read(REG_0E_BATV, &r0e);
            bool ok_sysv   = reg_read(REG_0F_SYSV, &r0f);
            bool ok_tspct  = reg_read(REG_10_TSPCT, &r10);
            bool ok_vbus   = reg_read(REG_11_VBUSV, &r11);
            bool ok_iinlim = reg_read(REG_00_INPUT, &r00);

            if (ok_status) {
                uint8_t chrg_stat = (r0b >> 3) & 0x03;   // 00 not charging, 01 pre-charge, 10 fast, 11 done
                st.charging   = (chrg_stat == 0x01 || chrg_stat == 0x02);
                st.chg_done   = (chrg_stat == 0x03);
                st.power_good = (r0b & 0x04) != 0;       // PG_STAT
                st.vbus_stat  = (r0b >> 5) & 0x07;       // VBUS_STAT[2:0]

                // #PWR-7: переход "работа от АКБ" → "заряд идёт" (напр. воткнули
                // внешнее питание) — переприменяем сохранённый IINLIM, т.к. чип
                // мог сам переписать регистр при повторном автоопределении.
                if (st.charging && !prev_charging) {
                    write_iinlim_raw(s_desired_iinlim_ma);
                }
                prev_charging = st.charging;
            }
            // #PWR-9: дамп регистров (0x0C — уже прочитанное значение: повторное
            // чтение сбросило бы защёлку) и счётчики неисправностей с момента старта.
            static uint32_t n_boost, n_chrg, n_wd;
            for (uint8_t r = 0; r <= 0x14; r++) {
                if (r == REG_0C_FAULT) { st.regs[r] = r0c; continue; }
                uint8_t v = 0;
                reg_read(r, &v);
                st.regs[r] = v;
            }
            if (ok_fault) {
                if (r0c & 0x40) { n_boost++; ESP_LOGW(TAG, "BOOST_FAULT (REG0C=0x%02x, VBUS_STAT=%u, REG03=0x%02x, REG0A=0x%02x)",
                                                      r0c, (unsigned)((r0b >> 5) & 7), st.regs[0x03], st.regs[0x0A]); }
                if (r0c & 0x30) n_chrg++;
                if (r0c & 0x80) n_wd++;
            }
            st.boost_faults = n_boost;
            st.chrg_faults  = n_chrg;
            st.wd_faults    = n_wd;
            if (ok_fault) {
                st.fault_watchdog = (r0c & 0x80) != 0;
                st.fault_boost    = (r0c & 0x40) != 0;
                st.fault_chrg     = (r0c >> 4) & 0x03;
                st.fault_bat      = (r0c & 0x08) != 0;
                st.fault_ntc      = r0c & 0x07;
            }
            if (ok_vbus) {
                st.vbus_present = (r11 & 0x80) != 0;    // VBUS_GD
                st.vbus_voltage = 2.6f + (float)(r11 & 0x7F) * 0.1f;
            }
            if (ok_batv) {
                // BATV[6:0]: offset 2.304V, шаг 20мВ на бит (см. datasheet REG0E)
                float raw_v = 2.304f + (float)(r0e & 0x7F) * 0.02f;
                // #PWR-5: сглаживаем шум квантования АЦП (см. комментарий у ema_v выше)
                ema_v = (ema_v < 0.0f) ? raw_v : (ema_v * 0.75f + raw_v * 0.25f);
                st.voltage = ema_v;
                st.percent = voltage_to_percent(st.voltage);
            }
            if (ok_sysv) {
                st.vsys_voltage = 2.304f + (float)(r0f & 0x7F) * 0.02f;
            }
            if (ok_tspct) {
                // TSPCT[6:0]: offset 21%, шаг 0.465%/бит; валидно только с внешним NTC
                st.ntc_pct = 21.0f + (float)(r10 & 0x7F) * 0.465f;
            }
            if (ok_iinlim) {
                // #PWR-6: IINLIM[5:0], offset 100мА, шаг 50мА/бит (см. datasheet REG00)
                st.iinlim_ma = 100 + (int)(r00 & 0x3F) * 50;
            }
            if (!ok_status && !ok_batv && !ok_vbus) {
                // #PWR-1: несколько подряд неудачных чтений — плата, вероятно,
                // отключилась между probe() и опросом регистров (шатающийся
                // разъём); не показываем стухшие данные.
                st.present = false;
            }
        }

        if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
            s_status = st;
            xSemaphoreGive(s_mutex);
        }

        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

void battery_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    s_i2c_mutex = xSemaphoreCreateMutex();
    load_iinlim_from_nvs();   // #PWR-7: до подъёма I2C — не блокирует, просто читает NVS

    i2c_master_bus_handle_t bus = board_i2c_get_bus();
    if (!bus) {
        ESP_LOGW(TAG, "shared I2C bus unavailable — battery icon disabled");
        return;
    }

    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BQ25895_I2C_ADDR,
        .scl_speed_hz = 100000,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &dev_config, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "i2c_master_bus_add_device failed: %s", esp_err_to_name(err));
        return;
    }

    if (probe_once()) {
        ESP_LOGI(TAG, "CG_Charger_BQ-2v0 (BQ25895) detected on shared I2C bus");
    } else {
        ESP_LOGI(TAG, "CG_Charger_BQ-2v0 not detected on shared I2C bus — battery icon will stay hidden. "
                      "Fine if the module isn't installed; check wiring/pull-ups otherwise.");
    }

    xTaskCreate(poll_task, "battery_poll", 3072, NULL, tskIDLE_PRIORITY + 2, NULL);
}

void battery_get_status(battery_status_t *out)
{
    if (!out) return;
    battery_status_t snap = { 0 };
    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        snap = s_status;
        xSemaphoreGive(s_mutex);
    }
    *out = snap;
}

int battery_set_iinlim(uint16_t ma)
{
    if (!s_dev) return -1;
    int applied_ma = write_iinlim_raw(ma);
    if (applied_ma < 0) return -1;
    // #PWR-7: запоминаем выбор пользователя — переживёт reboot и повторно
    // применится при инициализации модуля / начале заряда (см. poll_task).
    s_desired_iinlim_ma = applied_ma;
    save_iinlim_to_nvs(applied_ma);
    return applied_ma;
}
