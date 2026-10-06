// #OTA-1: см. ota_update.h.
#include "ota_update.h"
#include "web_waterfall.h"   // web_csrf_check()
#include "spectrogram.h"     // spectrogram_prepare_reboot()
#include "atomspectra.h"     // spectrum_autosave()

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "esp_ota_ops.h"
#include "esp_app_format.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ota";

#define OTA_BUF          4096
#define OTA_MAX_TIMEOUTS 5      // подряд таймаутов приёма (по ~3 с) — клиент пропал; дольше держать
                                // веб-сервер нельзя: он один на все страницы

// Структурная проверка образа в потоке (из апстрима, ota_image_check.h): заголовок
// esp_image_header_t (24 Б: magic 0xE9, число сегментов, chip_id) и сегменты
// (8 Б: адрес + длина) должны быть пройдены целиком — длина тела от клиента
// целостности не доказывает.
typedef struct {
    int      state;            // 0 заголовок, 1 заголовок сегмента, 2 данные сегмента, 3 готово, -1 ошибка
    uint8_t  hdr[24];
    uint8_t  hdr_len;
    uint8_t  seg[8];
    uint8_t  seg_len;
    uint8_t  seg_count, seg_done;
    uint32_t seg_left;
} img_walk_t;

static bool walk_feed(img_walk_t *w, const uint8_t *d, size_t n)
{
    size_t i = 0;
    while (i < n) {
        if (w->state == 0) {
            size_t take = n - i < (size_t)(24 - w->hdr_len) ? n - i : (size_t)(24 - w->hdr_len);
            memcpy(w->hdr + w->hdr_len, d + i, take);
            w->hdr_len += take; i += take;
            if (w->hdr_len == 24) {
                uint16_t chip = w->hdr[12] | (w->hdr[13] << 8);
                if (w->hdr[0] != ESP_IMAGE_HEADER_MAGIC || chip != ESP_CHIP_ID_ESP32S3 || w->hdr[1] == 0) {
                    w->state = -1;
                    return false;
                }
                w->seg_count = w->hdr[1];
                w->state = 1;
            }
        } else if (w->state == 1) {
            size_t take = n - i < (size_t)(8 - w->seg_len) ? n - i : (size_t)(8 - w->seg_len);
            memcpy(w->seg + w->seg_len, d + i, take);
            w->seg_len += take; i += take;
            if (w->seg_len == 8) {
                w->seg_len = 0;
                w->seg_left = w->seg[4] | (w->seg[5] << 8) | (w->seg[6] << 16) | ((uint32_t)w->seg[7] << 24);
                w->state = 2;
                if (w->seg_left == 0) { w->seg_done++; w->state = w->seg_done == w->seg_count ? 3 : 1; }
            }
        } else if (w->state == 2) {
            size_t take = n - i < w->seg_left ? n - i : w->seg_left;
            i += take; w->seg_left -= take;
            if (w->seg_left == 0) { w->seg_done++; w->state = w->seg_done == w->seg_count ? 3 : 1; }
        } else if (w->state == 3) {
            i = n;   // хвост: контрольная сумма и SHA-256 — проверит esp_ota_end()
        } else {
            return false;
        }
    }
    return true;
}

bool ota_update_supported(void)
{
    return esp_ota_get_next_update_partition(NULL) != NULL;
}

const char *ota_update_running_label(void)
{
    const esp_partition_t *p = esp_ota_get_running_partition();
    return p ? p->label : "?";
}

// Дочитать остаток тела, чтобы браузер получил ответ с причиной, а не обрыв
// соединения посреди отправки.
static void drain(httpd_req_t *req, int left)
{
    char tmp[512];
    int timeouts = 0;
    while (left > 0 && timeouts < OTA_MAX_TIMEOUTS) {
        int rd = httpd_req_recv(req, tmp, left < (int)sizeof tmp ? left : (int)sizeof tmp);
        if (rd == HTTPD_SOCK_ERR_TIMEOUT) { timeouts++; continue; }
        if (rd <= 0) break;
        timeouts = 0;
        left -= rd;
    }
}

static esp_err_t fail(httpd_req_t *req, esp_ota_handle_t h, const char *status, const char *msg)
{
    if (h) esp_ota_abort(h);
    ESP_LOGW(TAG, "#OTA-1: отказ: %s", msg);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    char b[160];
    snprintf(b, sizeof b, "{\"ok\":false,\"err\":\"%s\"}", msg);
    httpd_resp_sendstr(req, b);
    return ESP_OK;   // ответ отдан; соединение пусть закроет клиент
}

static volatile bool s_busy;

static esp_err_t h_ota(httpd_req_t *req)
{
    if (!web_csrf_check(req)) return ESP_FAIL;
    int total = req->content_len;
    if (s_busy) { drain(req, total); return fail(req, 0, "409 Conflict", "busy"); }
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) { drain(req, total); return fail(req, 0, "400 Bad Request", "no OTA partition (flash once over USB)"); }
    if (total <= 0) return fail(req, 0, "400 Bad Request", "empty body");
    if ((size_t)total > part->size) { drain(req, total); return fail(req, 0, "400 Bad Request", "image larger than partition"); }

    s_busy = true;
    esp_ota_handle_t h = 0;
    // Стирать по мере записи: стирание всего слота (3 МБ) одним вызовом держало
    // флеш десятки секунд, сеть не отвечала, и Windows рвал соединение (замер:
    // обрыв через 19.5 с). Хвост прежнего образа за концом нового не мешает —
    // esp_ota_end() проверяет образ по его собственной длине.
    esp_err_t e = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &h);
    if (e != ESP_OK) { s_busy = false; return fail(req, 0, "500 Internal Server Error", "ota_begin"); }
    uint8_t *buf = heap_caps_malloc(OTA_BUF, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buf) { s_busy = false; return fail(req, h, "500 Internal Server Error", "oom"); }
    ESP_LOGW(TAG, "#OTA-1: приём %d байт в %s", total, part->label);

    img_walk_t w = {0};
    int left = total, got = 0, timeouts = 0;
    const char *err = NULL;
    while (left > 0) {
        int rd = httpd_req_recv(req, (char *)buf, left < OTA_BUF ? left : OTA_BUF);
        if (rd == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++timeouts >= OTA_MAX_TIMEOUTS) { err = "receive timeout"; break; }
            continue;
        }
        if (rd <= 0) { err = "receive error"; break; }
        timeouts = 0;
        left -= rd;   // принятое уже не дочитывать в drain()
        if (!walk_feed(&w, buf, rd)) { err = "not an ESP32-S3 firmware image"; break; }
        if (esp_ota_write(h, buf, rd) != ESP_OK) { err = "flash write"; break; }
        got += rd;
    }
    free(buf);
    if (!err && w.state != 3) err = "image incomplete (truncated)";
    if (err) {
        esp_ota_abort(h);
        drain(req, left);
        s_busy = false;
        return fail(req, 0, "400 Bad Request", err);
    }
    e = esp_ota_end(h);   // проверка образа (SHA-256)
    if (e != ESP_OK) { s_busy = false; return fail(req, 0, "400 Bad Request", "image check failed"); }
    e = esp_ota_set_boot_partition(part);
    if (e != ESP_OK) { s_busy = false; return fail(req, 0, "500 Internal Server Error", "set_boot_partition"); }

    const esp_app_desc_t *d = NULL;
    esp_app_desc_t desc;
    if (esp_ota_get_partition_description(part, &desc) == ESP_OK) d = &desc;
    char resp[160];
    snprintf(resp, sizeof resp, "{\"ok\":true,\"bytes\":%d,\"partition\":\"%.16s\",\"version\":\"%.32s\"}",
             got, part->label, d ? d->version : "");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    ESP_LOGW(TAG, "#OTA-1: записано %d байт в %s (%s) — перезагрузка", got, part->label, d ? d->version : "?");
    spectrum_autosave();            // набранный спектр — на карту
    spectrogram_prepare_reboot();   // #OTA-3: закрыть сегмент водопада
    vTaskDelay(pdMS_TO_TICKS(800)); // дать ответу уйти в эфир
    esp_restart();
    return ESP_OK;
}

void ota_update_register(httpd_handle_t server)
{
    static const httpd_uri_t u = { .uri = "/api/ota", .method = HTTP_POST, .handler = h_ota };
    httpd_register_uri_handler(server, &u);
    ESP_LOGI(TAG, "#OTA-1: работаем с раздела %s, обновление по WiFi %s",
             ota_update_running_label(), ota_update_supported() ? "доступно" : "НЕДОСТУПНО (нет OTA-разделов)");
}
