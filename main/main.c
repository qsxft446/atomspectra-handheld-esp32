#include "atomspectra.h"
#include "spectrogram.h"
#include "cps_alarm.h"
#include "iso_live.h"   // #ISO-1
#include "boot_config.h"
#include "wf_offload.h"   // #REC-11-A2: автономная выгрузка сегментов водопада
#include "monitor.h"      // #MON-1: серия CPS-мониторинга на плате
#include "battery.h"      // #PWR-1: опрос CG_Charger_BQ-2v0 (BQ25895) для значка батареи в Web UI
#include "net_time.h"     // #FIELD-5: источник времени (SNTP/браузер/ручной)
#include "rgb_led.h"      // #LED-1: бортовой RGB-светодиод — индикация статуса спектрометра
#include "rtc_ds3231.h"   // #RTC-1: часы DS3231 — время платы без сети
#include "sd_card.h"      // #SD-1: слот microSD на модуле дисплея
#include "sys_load.h"     // #SYS-1: загрузка ядер ESP32 в лог и на экран
#include "log_capture.h"  // #LOG-1: лог ESP32 на странице «Сервис»
#include "esp_ota_ops.h"   // #OTA-1: подтвердить новую прошивку после успешного старта
#include "tft_display.h"  // #TFT-1: ST7796U 480x320 + тач FT6336U — водопад/спектр вместо SSD1306
#include "esp_log.h"
#include "esp_sntp.h"
#include "esp_timer.h"      // #BOOT-1
#include "esp_system.h"
#include "esp_attr.h"
#include "esp_rom_sys.h"
#include <string.h>
#include <inttypes.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"   // #FW-13 фикс №2: ожидание коммита свипа перед autosave

static const char *TAG = "main";

// #BOOT-1 (2026-10-18, «иногда при включении нет экрана и WiFi, отловить сложно»): сторож старта.
// Этапы app_main() отмечаются; не дошли до конца за BOOT_WD_S — перезагрузка, имя этапа — в RTC-память
// (переживает программный сброс), следующий старт пишет его в лог (видно на странице «Сервис»).
#define BOOT_WD_S    45
#define BOOT_MAGIC   0xB0071D1Eu
static RTC_NOINIT_ATTR uint32_t s_hang_magic;
static RTC_NOINIT_ATTR char     s_hang_stage[24];
static const char *volatile     s_stage = "start";
static esp_timer_handle_t       s_boot_wd;

static void boot_wd_cb(void *arg)
{
    (void)arg;
    strlcpy(s_hang_stage, s_stage, sizeof s_hang_stage);
    s_hang_magic = BOOT_MAGIC;
    esp_rom_printf("#BOOT-1: init stuck at '%s' for %d s - restart\n", s_hang_stage, BOOT_WD_S);
    esp_restart();
}

static void boot_wd_arm(void)
{
    if (s_hang_magic == BOOT_MAGIC && esp_reset_reason() == ESP_RST_SW) {
        s_hang_stage[sizeof s_hang_stage - 1] = '\0';
        ESP_LOGE(TAG, "#BOOT-1: previous start hung at '%s' — restarted by the start watchdog", s_hang_stage);
    }
    s_hang_magic = 0;
    const esp_timer_create_args_t a = { .callback = boot_wd_cb, .name = "boot_wd" };
    if (esp_timer_create(&a, &s_boot_wd) == ESP_OK) esp_timer_start_once(s_boot_wd, BOOT_WD_S * 1000000ULL);
}

static void boot_wd_done(void)
{
    if (s_boot_wd) esp_timer_stop(s_boot_wd);
}

// #FW-23: usb_host_cdc_init() (boot-автозапуск водопада) стартует раньше SNTP —
// started_at мог зафиксироваться near-epoch. На первый успешный sync — пересчитать.
static void time_sync_cb(struct timeval *tv)
{
    (void)tv;
    net_time_mark_sntp();      // #FIELD-5 (A2): зафиксировать факт реальной SNTP-синхронизации
    rtc_ds3231_save_now();     // #RTC-1: точное время — в DS3231
    spectrogram_time_synced();
}

static void init_sntp(void)
{
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_set_time_sync_notification_cb(time_sync_cb);
    esp_sntp_init();
}

void app_main(void)
{
    log_capture_init();   // #LOG-1: первым — чтобы лог старта попал на страницу «Сервис»
    boot_wd_arm();        // #BOOT-1: сторож старта (s_stage — этап для лога зависания)
    ESP_LOGI(TAG, "AtomSpectra Gateway starting...");
    sys_load_init();   // #SYS-1: замер загрузки ядер раз в секунду

    s_stage = "wifi";
    wifi_manager_init();

    // #PWR-1: опрос модуля бесперебойного питания CG_Charger_BQ-2v0 (I2C, BQ25895) —
    // независим от WiFi/USB Host, поэтому поднимается сразу. Если плата физически
    // не подключена, probe тихо не пройдёт и значок батареи в Web UI останется скрыт.
    s_stage = "battery";
    battery_init();

    // #RTC-1: время из DS3231 (та же шина I2C) — до spectrum/spectrogram/USB,
    // чтобы метки старта записи и сэмплов сразу были в настоящем времени.
    s_stage = "rtc";
    rtc_ds3231_init();

    // #SD-5: SD-карта (слот модуля дисплея) — ДО спектра и водопада: сохранения
    // и сегменты водопада пишутся ТОЛЬКО на неё, а водопад при старте
    // восстанавливает запись в её каталог. Шину SPI поднимает карта, дисплей
    // позже садится на неё же.
    s_stage = "sd";
    sd_card_init();

    // #FW-2/#FW-3: настройки поведения при старте платы (NVS, по умолчанию всё OFF).
    // #FW-44: весь USB-стек (boot_config → spectrum → spectrogram → usb_host_cdc)
    // поднят ДО AP-проверки. Раньше свежая плата (wifi не настроен) уходила в
    // немой while(1){vTaskDelay} ещё до usb_host_cdc_init() → USB Host не стартовал,
    // консоль давала лишь 3 boot-строки и молчала навсегда. Теперь USB работает
    // независимо от WiFi — спектрометр определяется и диагностируется в AP-режиме.
    boot_config_t bc;
    boot_config_load(&bc);
    ESP_LOGI(TAG, "boot-config: as_spec=%d as_wf=%d clr_spec=%d clr_wf=%d",
             bc.autostart_spectrum, bc.autostart_waterfall, bc.clear_spectrum, bc.clear_waterfall);

    s_stage = "spectrum";
    spectrum_init();
    spectrum_saved_migrate_to_sd();   // #SD-4: прежние сохранения с флеша -> на карту (флеш не трогаем)
    spectrum_restore_autosave();
    spectrum_load_calibration();
    // #FW-3: очистка накопленного спектра при старте — после restore, до того как
    // спектрограмма снимет baseline. -rst прибору пошлётся на первом USB-коннекте.
    if (bc.clear_spectrum) {
        spectrum_reset();
        ESP_LOGW(TAG, "FW-3: accumulated spectrum cleared on boot");
    }
    s_stage = "spectrogram";
    spectrogram_init();
    // #FW-3: очистка водопада при старте — ДО spectrogram_restore(), иначе restore
    // возобновит прежнюю запись из сохранённого состояния.
    if (bc.clear_waterfall) {
        spectrogram_clear();
        ESP_LOGW(TAG, "FW-3: waterfall cleared on boot");
    }
    spectrogram_restore();   // #REC-6: возобновить запись после ребута/сбоя питания
    // #FW-2: передать флаги автозапуска в USB-модуль ДО его инициализации.
    usb_host_cdc_set_autostart(bc.autostart_spectrum, bc.autostart_waterfall, bc.clear_spectrum);
    s_stage = "usb";
    usb_host_cdc_init();

    // #LED-1: статус спектрометра на бортовом RGB (GPIO48) — работает и в setup-режиме,
    // поэтому поднимаем сразу после USB Host, до ветвления по net_mode.
    s_stage = "rgb_led";
    rgb_led_init();

    // #TFT-1/#TFT-6: ST7796U сам поднимает SPI2_HOST (W5500 из проекта вырезан)
    // и делит I2C-шину с battery_init() (тач FT6336U). Работает и в setup-режиме,
    // статус USB/сети виден сразу.
    s_stage = "tft";
    tft_display_init();

    // #FW-44/#FIELD-1: setup-портал (свежая плата без wifi-конфига) — диагностический
    // цикл, сеть-зависимые подсистемы не поднимаем (setup-httpd держится внутри
    // wifi_manager). USB Host уже поднят выше → логируем реальную диагностику вместо
    // прежней немоты. Выход — только перезагрузка после сохранения wifi/выбора режима.
    net_run_mode_t net_mode = wifi_manager_mode();
    if (net_mode == NET_MODE_SETUP) {
        boot_wd_done();   // #BOOT-1: дальше — вечный диагностический цикл
        ESP_LOGI(TAG, "Setup captive portal active, waiting for WiFi config");
        while (1) {
            const spectrum_data_t *sp = spectrum_get_current();
            ESP_LOGI(TAG, "SETUP: connect SSID=AtomSpectra-Setup -> http://192.168.4.1 | USB:%s spectrometer:%s counts:%" PRIu32,
                usb_host_cdc_is_connected() ? "OK" : "--",
                usb_host_cdc_spectrometer_dead() ? "DEAD"
                    : (usb_host_cdc_is_connected() ? "streaming" : "--"),
                sp->total_counts);
            vTaskDelay(pdMS_TO_TICKS(10000));
        }
    }

    // #FIELD-1: рабочий сетевой стек — общий для Indoor (STA) и Outdoor (полевой AP).
    s_stage = "web";
    web_server_init();
    monitor_init();      // #MON-1: кольцо серии CPS (6 ч в PSRAM) + задача-подписчик коммитов
    cps_alarm_init();    // #ALM-1: пороги CPS — спикер GPIO21 и мигание CPS на экране
    iso_live_init();     // #ISO-1: определение нуклидов во время набора (задача iso, PSRAM)
    s_stage = "bridge";
    tcp_bridge_init();
    if (net_mode == NET_MODE_STA) {
        wf_offload_init();   // #REC-11-A2: аплоадер сегментов — только Indoor (есть сеть/приёмник)
        init_sntp();          // SNTP-время — только Indoor (есть интернет)
    } else {
        // #FIELD-1: Outdoor (полевой AP) — интернета/приёмника нет: SNTP и offload
        // не поднимаем. Время платы приходит от браузера телефона (#FIELD-5,
        // POST /api/time); данные водопада копятся на flash, забираются дома.
        ESP_LOGI(TAG, "FIELD-1: Outdoor AP — SNTP & offload disabled (time via browser)");
    }

    boot_wd_done();   // #BOOT-1
    ESP_LOGI(TAG, "All subsystems initialized");

    // #FW-13 фикс №2: autosave (32.8 КБ LittleFS = freeze кэша обоих ядер) фазово
    // привязывается к коммиту свипа — запись уходит в тихое USB-окно (~0.5 с) сразу
    // после конца burst, а не в случайную фазу, где рвала приём FTDI.
    SemaphoreHandle_t autosave_sig = xSemaphoreCreateBinary();
    if (autosave_sig) spectrum_add_commit_listener(autosave_sig);

    int info_tick = 0, autosave_tick = 0, uptime_ticks = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        // #OTA-1: прошивка, залитая по WiFi, стартует «на испытании»: если она упадёт
        // до этой отметки (30 с работы), загрузчик вернёт прежнюю. Вызов для уже
        // подтверждённой прошивки ничего не делает.
        if (++uptime_ticks == 3) {
            esp_err_t e = esp_ota_mark_app_valid_cancel_rollback();
            if (e != ESP_OK) ESP_LOGW(TAG, "#OTA-1: mark_app_valid: %s", esp_err_to_name(e));
        }
        spectrum_base_tick();   // #SB-1: запись базы спектра после перезапуска прибора
        const spectrum_data_t *sp = spectrum_get_current();
        // #SYS-1: dev_cpu — загрузка процессора спектрометра (из STAT), esp_cpu — ядра 0/1.
        ESP_LOGI(TAG, "USB:%s WiFi:%s TCP:%s counts:%" PRIu32 " dev_cpu:%u%% esp_cpu:%d/%d%%",
            usb_host_cdc_is_connected() ? "OK" : "--",
            wifi_is_connected() ? "OK" : "--",
            tcp_bridge_client_connected() ? "OK" : "--",
            sp->total_counts, (unsigned)sp->cpu_load, sys_load_core(0), sys_load_core(1));

        // #FW-13: период -inf 30 с → 30 мин (оператор 2026-07-03). Единственное
        // динамичное поле ответа — температура; каждый -inf вклинивает 404-байтный
        // текстовый ответ в поток свипов, чаще незачем.
        if (usb_host_cdc_is_connected() && ++info_tick >= 180) {
            info_tick = 0;
            usb_host_send_text_command("-inf");
        }
        if (++autosave_tick >= 6) {
            autosave_tick = 0;
            if (autosave_sig) {
                xSemaphoreTake(autosave_sig, 0);                    // сброс протухшего сигнала
                xSemaphoreTake(autosave_sig, pdMS_TO_TICKS(1500));  // ждём свежий коммит
            }
            spectrum_autosave();
        }
        // #WF-1: отложенная запись калибровки (s_calib_dirty). Внутри сама берёт
        // SPEC_LOCK только на снапшот; flash-запись — вне лока и вне CDC/httpd.
        spectrum_save_calibration();
    }
}