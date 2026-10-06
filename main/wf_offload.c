#include "wf_offload.h"
#include "spectrogram.h"
#include "atomspectra.h"          // wifi_is_connected()
#include "esp_http_client.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "mbedtls/base64.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <time.h>
#include <inttypes.h>

static const char *TAG = "wf_ofl";

#define NVS_NS              "wfofl"
#define BACKOFF_BASE_MS     5000      // первый повтор после неудачи
#define BACKOFF_MAX_MS      300000    // потолок backoff — 5 мин
#define WIFI_WAIT_MS        5000      // нет WiFi — подождать и проверить снова
#define IDLE_EMPTY_MS       15000     // нечего слать — опрос каталога раз в 15 с
#define IDLE_DISABLED_MS    60000     // выключено — спать (просыпается на смене конфига)
#define HTTP_TIMEOUT_MS     20000

// Все три защищены s_lock.
static SemaphoreHandle_t  s_lock;
static wf_offload_cfg_t    s_cfg;
static wf_offload_stat_t   s_stat;
static TaskHandle_t        s_task;

#define CFGLOCK()   do { if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY); } while (0)
#define CFGUNLOCK() do { if (s_lock) xSemaphoreGive(s_lock); } while (0)

// ----------------------------------------------------------------------------
//  NVS (namespace "wfofl") — write-on-change (#NVS-1: не трогаем флэш зря)
// ----------------------------------------------------------------------------

static void nvs_load(wf_offload_cfg_t *c)
{
    memset(c, 0, sizeof(*c));
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;   // namespace ещё нет → defaults
    uint8_t en = 0;
    if (nvs_get_u8(h, "en", &en) == ESP_OK) c->enabled = en ? true : false;
    size_t l;
    l = sizeof(c->url);  nvs_get_str(h, "url",  c->url,  &l);   // missing → буфер остаётся ""
    l = sizeof(c->user); nvs_get_str(h, "user", c->user, &l);
    l = sizeof(c->pass); nvs_get_str(h, "pass", c->pass, &l);
    nvs_close(h);
}

// Возвращает 0 ok / -1 ошибка. Пишет ТОЛЬКО если конфиг реально изменился.
static int nvs_save(const wf_offload_cfg_t *c)
{
    wf_offload_cfg_t cur;
    nvs_load(&cur);
    if (memcmp(&cur, c, sizeof(cur)) == 0) {
        ESP_LOGD(TAG, "nvs_save: no change, skip (NVS wear guard)");
        return 0;                                              // #NVS-1: ничего не изменилось
    }
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return -1;
    esp_err_t e = ESP_OK;
    e |= nvs_set_u8 (h, "en",   c->enabled ? 1 : 0);
    e |= nvs_set_str(h, "url",  c->url);
    e |= nvs_set_str(h, "user", c->user);
    e |= nvs_set_str(h, "pass", c->pass);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    if (e != ESP_OK) { ESP_LOGW(TAG, "nvs_save failed (0x%x)", (int)e); return -1; }
    return 0;
}

// ----------------------------------------------------------------------------
//  Хелперы
// ----------------------------------------------------------------------------

// Регистронезависимый поиск подстроки (для Народмон-бана).
static bool contains_ci(const char *hay, const char *needle_lc)
{
    size_t nl = strlen(needle_lc);
    if (!nl) return false;
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        while (i < nl && p[i] && (char)tolower((unsigned char)p[i]) == needle_lc[i]) i++;
        if (i == nl) return true;
    }
    return false;
}

// "Basic base64(user:pass)" → hdr. user/pass ≤47 символов → влезает в hdr[160].
static void build_basic_auth(const char *user, const char *pass, char *hdr, size_t cap)
{
    char up[100];
    int n = snprintf(up, sizeof(up), "%s:%s", user, pass);
    if (n < 0) n = 0;
    if (n > (int)sizeof(up) - 1) n = (int)sizeof(up) - 1;
    unsigned char b64[160];
    size_t olen = 0;
    if (mbedtls_base64_encode(b64, sizeof(b64), &olen, (const unsigned char *)up, n) != 0)
        olen = 0;
    snprintf(hdr, cap, "Basic %.*s", (int)olen, (const char *)b64);
}

// ----------------------------------------------------------------------------
//  HTTP POST одного сегмента (стриминг тела из файла, без буфера на весь .aswf)
//  Возврат: HTTP-код (>0) при завершённом запросе; <0 — локальный сбой:
//    -10 fopen, -11 init, -12 open, -13 oom, -14 write.
// ----------------------------------------------------------------------------

static int post_segment(const wf_offload_cfg_t *c, const char *name,
                        const char *path, long size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -10;

    esp_http_client_config_t hc = {
        .url        = c->url,
        .method     = HTTP_METHOD_POST,
        .timeout_ms = HTTP_TIMEOUT_MS,
    };
    esp_http_client_handle_t cl = esp_http_client_init(&hc);
    if (!cl) { fclose(f); return -11; }

    esp_http_client_set_header(cl, "Content-Type", "application/octet-stream");
    esp_http_client_set_header(cl, "X-Filename", name);
    char authhdr[160];
    if (c->user[0]) {
        build_basic_auth(c->user, c->pass, authhdr, sizeof(authhdr));
        esp_http_client_set_header(cl, "Authorization", authhdr);
    }

    int result;
    if (esp_http_client_open(cl, (int)size) != ESP_OK) { result = -12; goto done; }

    {
        char *buf = malloc(4096);
        if (!buf) { result = -13; goto done; }
        size_t rd;
        bool ok = true;
        while ((rd = fread(buf, 1, 4096, f)) > 0) {
            int w = esp_http_client_write(cl, buf, rd);
            if (w < 0 || (size_t)w != rd) { ok = false; break; }
        }
        free(buf);
        if (!ok) { result = -14; goto done; }
        esp_http_client_fetch_headers(cl);
        result = esp_http_client_get_status_code(cl);
    }

done:
    esp_http_client_close(cl);
    esp_http_client_cleanup(cl);
    fclose(f);
    return result;
}

// ----------------------------------------------------------------------------
//  Задача-аплоадер
// ----------------------------------------------------------------------------

static void wait_ms(uint32_t ms)
{
    // Просыпается раньше по wf_offload_kick() (xTaskNotifyGive).
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(ms));
}

static void offload_task(void *arg)
{
    (void)arg;
    uint32_t backoff = BACKOFF_BASE_MS;

    for (;;) {
        wf_offload_cfg_t c;
        CFGLOCK(); c = s_cfg; CFGUNLOCK();

        if (!c.enabled || c.url[0] == 0) { wait_ms(IDLE_DISABLED_MS); continue; }
        if (!wifi_is_connected())        { wait_ms(WIFI_WAIT_MS);     continue; }

        uint32_t idx; char name[32], path[80]; long size = 0;
        if (!spectrogram_offload_claim(&idx, name, sizeof(name),
                                       path, sizeof(path), &size)) {
            wait_ms(IDLE_EMPTY_MS);                 // нечего слать
            continue;
        }

        CFGLOCK(); s_stat.busy = true; CFGUNLOCK();
        int rc = post_segment(&c, name, path, size);
        long now = (long)time(NULL);

        CFGLOCK();
        s_stat.busy = false;
        s_stat.last_status = rc;
        CFGUNLOCK();

        if (rc >= 200 && rc < 300) {
            spectrogram_offload_done(idx);          // удалить с Flash — освободить место
            CFGLOCK(); s_stat.sent_ok++; s_stat.last_ok_at = now; CFGUNLOCK();
            ESP_LOGI(TAG, "%s uploaded (HTTP %d), %ld B", name, rc, size);
            backoff = BACKOFF_BASE_MS;
            // успех — сразу пробуем следующий сегмент (дренаж backlog), без паузы
        } else {
            spectrogram_offload_release(idx);       // файл оставить, повторим позже
            CFGLOCK(); s_stat.failed++; CFGUNLOCK();
            ESP_LOGW(TAG, "%s upload failed (rc=%d), retry in %" PRIu32 " ms",
                     name, rc, backoff);
            wait_ms(backoff);
            backoff = (backoff >= BACKOFF_MAX_MS / 2) ? BACKOFF_MAX_MS : backoff * 2;
        }
    }
}

// ----------------------------------------------------------------------------
//  Публичный API
// ----------------------------------------------------------------------------

void wf_offload_init(void)
{
    if (s_task) return;                              // идемпотентно
    s_lock = xSemaphoreCreateMutex();
    memset(&s_stat, 0, sizeof(s_stat));
    s_stat.last_ok_at = 0;
    nvs_load(&s_cfg);
    ESP_LOGI(TAG, "init: enabled=%d url=%s auth=%s",
             s_cfg.enabled,
             s_cfg.url[0]  ? s_cfg.url  : "(none)",
             s_cfg.user[0] ? "basic"   : "none");
    // Сеть на core 1 (#TCP-2), низкий приоритет — не мешать рекордеру/USB.
    xTaskCreatePinnedToCore(offload_task, "wf_ofl", 6144, NULL, 4, &s_task, 1);
}

void wf_offload_get_cfg(wf_offload_cfg_t *out)
{
    if (!out) return;
    CFGLOCK(); *out = s_cfg; CFGUNLOCK();
}

int wf_offload_set_cfg(const wf_offload_cfg_t *in)
{
    if (!in) return WF_OFFLOAD_ERR_INVALID;

    wf_offload_cfg_t n;
    memset(&n, 0, sizeof(n));                        // нулевой паддинг → корректный memcmp в nvs_save
    n.enabled = in->enabled;
    snprintf(n.url,  sizeof(n.url),  "%s", in->url);
    snprintf(n.user, sizeof(n.user), "%s", in->user);
    snprintf(n.pass, sizeof(n.pass), "%s", in->pass);

    // БАН Народмон (CLAUDE.md, HARD HOLD): не позволяем настроить выгрузку на narodmon.
    if (contains_ci(n.url, "narodmon")) {
        ESP_LOGW(TAG, "set_cfg: refused narodmon url (ban)");
        return WF_OFFLOAD_ERR_BLOCKED;
    }
    // Транспорт A2 — только plain HTTP (LAN). HTTPS = фаза B (#REC-11-B): esp-tls
    // требует много стека и cert-bundle; не пускаем, чтобы не уронить задачу.
    if (n.enabled && strncmp(n.url, "http://", 7) != 0) {
        return WF_OFFLOAD_ERR_INVALID;
    }

    if (nvs_save(&n) != 0) return WF_OFFLOAD_ERR_NVS;

    CFGLOCK(); s_cfg = n; CFGUNLOCK();
    wf_offload_kick();
    return WF_OFFLOAD_OK;
}

void wf_offload_get_stat(wf_offload_stat_t *out)
{
    if (!out) return;
    CFGLOCK(); *out = s_stat; CFGUNLOCK();
}

void wf_offload_kick(void)
{
    if (s_task) xTaskNotifyGive(s_task);
}
