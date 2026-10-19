// #SD-1: см. sd_card.h. Второе устройство на шине дисплея: esp_lcd и sdspi —
// отдельные устройства одного SPI2_HOST, spi_master сам разводит их по CS.
#include "sd_card.h"
#include "board_spi.h"
#include "tft_display.h"    // #SD-2: tft_display_spi_lock() — развязка шины с экраном

#include <string.h>
#include "driver/spi_common.h"
#include "driver/sdspi_host.h"
#include "driver/gpio.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "diskio_sdmmc.h"   // #FS-1: ff_diskio_get_pdrv_card()
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "sd";

#ifndef SD_PIN_CS
#define SD_PIN_CS       10      // свободен после удаления W5500 (был его CS)
#endif
// Частота карты. Шина общая с панелью (10 МГц) и идёт через преобразователь
// уровней модуля — берём столько же, а не максимальные для SPI-режима 20 МГц.
#define SD_FREQ_KHZ     10000
// 1 — после монтирования фоновая проверка: запись/чтение/сверка файла на карте
// под живой нагрузкой дисплея на той же шине (замер скорости в лог).
#define SD_SELFTEST     0   // проверено 2026-09-28: 1 МБ без ошибок, 228/387 КБ/с при работающем экране

static sdmmc_card_t *s_card;

// #SD-2: ВСЕ команды карты — под замком экрана. Замер 2026-09-28: без него
// запись на карту во время передачи кадра дисплея роняла плату по кругу —
// assert spi_hal_setup_trans (spi_ll_get_running_cmd(hw) == 0): polling-
// транзакция sdspi стартовала поверх идущей DMA-передачи кадра esp_lcd.
// Через do_transaction идут все команды, включая монтирование.
// #HANG-1: кто сейчас в транзакции карты и на каком шаге (1 — ждёт замок шины, 2 — обмен с картой).
static void *volatile    s_txn_task;
static volatile int64_t  s_txn_since;
static volatile int      s_txn_stage;
static volatile uint32_t s_txn_errs;
static esp_err_t sd_do_transaction(int slot, sdmmc_command_t *cmd)
{
    s_txn_task = xTaskGetCurrentTaskHandle();
    s_txn_since = esp_timer_get_time();
    s_txn_stage = 1;
    tft_display_spi_lock();
    s_txn_stage = 2;
    esp_err_t err = sdspi_host_do_transaction(slot, cmd);
    tft_display_spi_unlock();
    s_txn_stage = 0;
    if (err != ESP_OK) s_txn_errs++;
    return err;
}

int sd_card_txn_state(void **task, int64_t *since, uint32_t *errs)
{
    *task = s_txn_task;
    *since = s_txn_since;
    *errs = s_txn_errs;
    return s_txn_stage;
}
#if SD_SELFTEST
static void sd_selftest_task(void *arg);
#endif
static char s_name[8];

void sd_card_init(void)
{
    // #SD-5: карта монтируется ДО дисплея (водопад и спектры пишутся только на
    // неё, водопад восстанавливает запись при старте) — шину поднимаем здесь.
    spi_bus_config_t bus = {
        .mosi_io_num = BOARD_SPI_MOSI,
        .miso_io_num = BOARD_SPI_MISO,
        .sclk_io_num = BOARD_SPI_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = BOARD_SPI_MAX_TRANSFER,   // #SD-5: как у дисплея
    };
    // #SD-5: CS панели — в 1, пока её не поднял tft_display_init().
    gpio_set_direction(BOARD_SPI_LCD_CS, GPIO_MODE_OUTPUT);
    gpio_set_level(BOARD_SPI_LCD_CS, 1);
    // «Уже поднята» — штатный случай (шину поднял дисплей), но драйвер пишет
    // его как ошибку. На время вызова глушим его тег, чтобы не пугать лог.
    esp_log_level_set("spi", ESP_LOG_NONE);
    esp_err_t err = spi_bus_initialize(BOARD_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
    esp_log_level_set("spi", ESP_LOG_INFO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "шина SPI не поднята (%s) — SD не используется", esp_err_to_name(err));
        return;
    }
    // Карта в SPI-режиме требует подтяжку MISO: пока она не выбрана, линия висит.
    gpio_set_pull_mode(BOARD_SPI_MISO, GPIO_PULLUP_ONLY);

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = BOARD_SPI_HOST;
    host.max_freq_khz = SD_FREQ_KHZ;
    host.do_transaction = sd_do_transaction;   // #SD-2
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = SD_PIN_CS;
    slot.host_id = BOARD_SPI_HOST;
    esp_vfs_fat_sdmmc_mount_config_t mcfg = {
        .format_if_mount_failed = false,   // чужую карту не стираем
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
    };
    // #SD-6: до 3 попыток. Замер 2026-09-30: после сброса платы посреди записи
    // карта иногда не отвечала на первый опрос (ESP_ERR_TIMEOUT) — с #SD-5 она
    // монтируется уже на ~1.3 с после старта, раньше до неё доходили на ~3 с.
    for (int attempt = 1; ; attempt++) {
        err = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot, &mcfg, &s_card);
        if (err == ESP_OK || err == ESP_FAIL || attempt >= 3) break;   // ESP_FAIL — не FAT, повтор не поможет
        ESP_LOGW(TAG, "SD: попытка %d не удалась (%s), повтор", attempt, esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    if (err != ESP_OK) {
        s_card = NULL;
        if (err == ESP_FAIL)
            ESP_LOGW(TAG, "карта есть, но FAT не монтируется — отформатируйте её в FAT32 на ПК");
        else
            ESP_LOGI(TAG, "SD-карта не найдена (%s)", esp_err_to_name(err));
        return;
    }
    memcpy(s_name, s_card->cid.name, sizeof s_card->cid.name);
    s_name[sizeof s_name - 1] = '\0';
    uint64_t total = 0, free_b = 0;
    sd_card_space(&total, &free_b);
    ESP_LOGI(TAG, "SD-карта %s смонтирована в %s: %llu МБ, свободно %llu МБ, %u кГц",
             s_name, SD_MOUNT_POINT, (unsigned long long)(total >> 20),
             (unsigned long long)(free_b >> 20), (unsigned)s_card->max_freq_khz);
#if SD_SELFTEST
    xTaskCreate(sd_selftest_task, "sd_test", 4096, NULL, 2, NULL);
#endif
}

#if SD_SELFTEST
// Запись 1 МБ кусками по 4 КБ, чтение, сверка, удаление. В фоне через 5 с
// после старта — когда дисплей уже гоняет кадры по той же шине.
static void sd_selftest_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(5000));
    const size_t CH = 4096, N = 256;
    uint8_t *buf = heap_caps_malloc(CH, MALLOC_CAP_SPIRAM);
    const char *path = SD_MOUNT_POINT "/SDTEST.BIN";
    bool ok = buf != NULL;
    int64_t t0 = esp_timer_get_time(), tw = 0;
    FILE *f = ok ? fopen(path, "wb") : NULL;
    if (!f) ok = false;
    for (size_t i = 0; ok && i < N; i++) {
        for (size_t k = 0; k < CH; k++) buf[k] = (uint8_t)(i * 31 + k * 7);
        if (fwrite(buf, 1, CH, f) != CH) ok = false;
    }
    if (f) { fclose(f); tw = esp_timer_get_time() - t0; }
    size_t bad = 0;
    int64_t t1 = esp_timer_get_time(), tr = 0;
    f = ok ? fopen(path, "rb") : NULL;
    if (ok && !f) ok = false;
    for (size_t i = 0; ok && i < N; i++) {
        if (fread(buf, 1, CH, f) != CH) { ok = false; break; }
        for (size_t k = 0; k < CH; k++) if (buf[k] != (uint8_t)(i * 31 + k * 7)) bad++;
    }
    if (f) { fclose(f); tr = esp_timer_get_time() - t1; }
    remove(path);
    free(buf);
    if (ok && !bad)
        ESP_LOGI(TAG, "проверка SD: 1 МБ записан и прочитан без ошибок; запись %.0f КБ/с, чтение %.0f КБ/с",
                 1024.0 * 1e6 / tw, 1024.0 * 1e6 / tr);
    else
        ESP_LOGE(TAG, "проверка SD: СБОЙ (ok=%d, неверных байт %u)", ok, (unsigned)bad);
    vTaskDelete(NULL);
}
#endif

bool sd_card_present(void) { return s_card != NULL; }

bool sd_card_space(uint64_t *total, uint64_t *free_bytes)
{
    if (!s_card) return false;
    return esp_vfs_fat_info(SD_MOUNT_POINT, total, free_bytes) == ESP_OK;
}

const char *sd_card_name(void) { return s_card ? s_name : ""; }

bool sd_card_ff_path(const char *vfs_path, char *out, size_t cap)
{
    size_t mp = strlen(SD_MOUNT_POINT);
    if (!s_card || strncmp(vfs_path, SD_MOUNT_POINT, mp) != 0) return false;
    BYTE pdrv = ff_diskio_get_pdrv_card(s_card);
    if (pdrv == 0xFF) return false;
    int n = snprintf(out, cap, "%u:%s", (unsigned)pdrv, vfs_path[mp] ? vfs_path + mp : "/");
    return n > 0 && (size_t)n < cap;
}
