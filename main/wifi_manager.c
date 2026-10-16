#include "atomspectra.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_timer.h"       // #WIFI-2: таймер повторов подключения
#include "nvs_flash.h"
#include "esp_http_server.h"
#include "mdns.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "lwip/sockets.h"
#include <string.h>

static const char *TAG = "wifi";

static EventGroupHandle_t s_wifi_events;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_TRY_FAIL_BIT  BIT1   // #WIFI-1: проверяемая сеть отказала (отключение во время пробы)

// #WIFI-1: идёт проверка новой сети с экрана — обработчик отключений не считает
// попытки и не уводит плату в полевой AP; причину отказа запоминает.
static volatile bool s_trying = false;
static volatile uint8_t s_try_reason = 0;

static int s_retry_count = 0;
#define MAX_RETRY 10

// #WIFI-2 (запрос 2026-10-17, «перезапуск напрягает»): автоперехода в полевой AP больше нет
// (был #FIELD-2a: нет IP за 90 с или 10 отказов -> перезагрузка в AP). Indoor ищет домашнюю
// сеть сам: MAX_RETRY попыток подряд, дальше одна попытка раз в RETRY_SEC. Outdoor — только
// вручную (экран Conf / веб «Система»), там у платы есть экран, и в поле его хватает.
#define RETRY_SEC 30

// #FIELD-1: текущий сетевой режим. Дефолт STA, устанавливается в развилке init.
static net_run_mode_t s_mode = NET_MODE_STA;

// #FIELD-1/#SEC-2: параметры полевого AP (дефолты — из ТЗ разд. 9).
#define AP_SSID_DEFAULT "AtomSpectra-Outdoor"
#define AP_PASS_DEFAULT "atomspectra"
static char s_ap_ssid[WIFI_SSID_MAX] = AP_SSID_DEFAULT;
static char s_ap_pass[WIFI_PASS_MAX] = AP_PASS_DEFAULT;
static bool s_ap_pass_default = true;   // #SEC-2: пароль AP не менялся
static bool s_ap_forced = false;        // #FIELD-6: field_ap липкий (ap_mode=1) vs fallback

static esp_timer_handle_t s_retry_timer = NULL;   // #WIFI-2: редкие попытки после MAX_RETRY


/* ---- DNS captive portal ---- */

static void dns_captive_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "DNS socket failed");
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = INADDR_ANY
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "DNS bind failed");
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    uint8_t rx[512], tx[512];
    while (1) {
        struct sockaddr_in client;
        socklen_t clen = sizeof(client);
        int len = recvfrom(sock, rx, sizeof(rx), 0,
                           (struct sockaddr *)&client, &clen);
        if (len < 12) continue;
        // P2-2: ниже к ответу дописываются 16 байт A-записи (off = len + 16).
        // Без этой проверки запрос длиной > sizeof(tx)-16 переполнил бы tx[].
        if (len > (int)sizeof(tx) - 16) continue;

        memcpy(tx, rx, len);
        tx[2] = 0x80 | (rx[2] & 0x79);
        tx[3] = 0x80;
        tx[6] = 0; tx[7] = 1;

        int off = len;
        tx[off++] = 0xC0; tx[off++] = 0x0C;
        tx[off++] = 0x00; tx[off++] = 0x01;
        tx[off++] = 0x00; tx[off++] = 0x01;
        tx[off++] = 0x00; tx[off++] = 0x00;
        tx[off++] = 0x00; tx[off++] = 0x3C;
        tx[off++] = 0x00; tx[off++] = 0x04;
        tx[off++] = 192;  tx[off++] = 168;
        tx[off++] = 4;    tx[off++] = 1;

        sendto(sock, tx, off, 0,
               (struct sockaddr *)&client, clen);
    }
}

/* ---- Captive setup portal HTTP (свежая плата, NET_MODE_SETUP) ---- */

static esp_err_t handle_setup_root(httpd_req_t *req)
{
    extern const uint8_t setup_html_start[] asm("_binary_setup_html_start");
    extern const uint8_t setup_html_end[]   asm("_binary_setup_html_end");
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, (const char *)setup_html_start,
                    setup_html_end - setup_html_start);
    return ESP_OK;
}

static esp_err_t handle_setup_scan(httpd_req_t *req)
{
    wifi_scan_config_t scan_cfg = { .show_hidden = false };
    esp_wifi_scan_start(&scan_cfg, true);

    uint16_t num = 0;
    esp_wifi_scan_get_ap_num(&num);
    if (num > 20) num = 20;

    wifi_ap_record_t *records = calloc(num, sizeof(wifi_ap_record_t));
    if (!records) {
        httpd_resp_sendstr(req, "[]");
        return ESP_OK;
    }
    esp_wifi_scan_get_ap_records(&num, records);

    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < num; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "ssid", (char *)records[i].ssid);
        cJSON_AddNumberToObject(item, "rssi", records[i].rssi);
        cJSON_AddNumberToObject(item, "auth", records[i].authmode);
        cJSON_AddItemToArray(arr, item);
    }
    free(records);

    char *json = cJSON_PrintUnformatted(arr);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    free(json);
    cJSON_Delete(arr);
    return ESP_OK;
}

static esp_err_t handle_setup_connect(httpd_req_t *req)
{
    char body[256] = {0};
    int recv_len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (recv_len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty");
        return ESP_FAIL;
    }
    body[recv_len] = '\0';

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad JSON");
        return ESP_FAIL;
    }

    const char *ssid = cJSON_GetStringValue(
        cJSON_GetObjectItem(root, "ssid"));
    const char *pass = cJSON_GetStringValue(
        cJSON_GetObjectItem(root, "pass"));

    if (!ssid || strlen(ssid) == 0) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No SSID");
        return ESP_FAIL;
    }

    nvs_handle_t nvs;
    if (nvs_open("wifi", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_str(nvs, "ssid", ssid);
        // P2-6 / #SEC-1: пароль WiFi пишется в NVS открытым текстом. Любой с
        // физическим доступом к плате может вычитать flash
        // (esptool read_flash 0x9000 0x6000 nvs.bin) и достать пароль строкой —
        // шифрования нет. Это осознанный компромисс для модели "доверенный
        // домашний LAN". Единственная корректная защита — включить шифрование
        // flash (NVS-encryption работает поверх него): заготовка
        // CONFIG_SECURE_FLASH_ENC_ENABLED + CONFIG_NVS_ENCRYPTION лежит
        // закомментированной в sdkconfig.defaults. ⚠ Необратимо (прожигает eFuse),
        // поэтому по умолчанию выкл — это gate оператора, здесь не трогаем.
        // Подробности и последствия — INSTALL.md, раздел "9. Безопасность".
        nvs_set_str(nvs, "pass", pass ? pass : "");
        nvs_commit(nvs);
        nvs_close(nvs);
        ESP_LOGI(TAG, "WiFi config saved: SSID=%s", ssid);
    }

    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}

// #FIELD-2c: из setup-портала — «Работать без роутера (полевой режим)».
// Ставит forced-флаг Outdoor и ребутит: свежая плата сразу в поле без домашней сети.
static esp_err_t handle_setup_field(httpd_req_t *req)
{
    nvs_handle_t nvs;
    if (nvs_open("wifi", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u8(nvs, "ap_mode", 1);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    ESP_LOGI(TAG, "FIELD-2c: field mode requested from setup -> reboot");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}

static esp_err_t handle_setup_redirect(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_sendstr(req, "");
    return ESP_OK;
}

static void start_captive_portal(void)
{
    s_mode = NET_MODE_SETUP;

    esp_netif_create_default_wifi_ap();

    wifi_config_t ap_config = {
        .ap = {
            .ssid = "AtomSpectra-Setup",
            .channel = 1,
            .authmode = WIFI_AUTH_OPEN,
            .max_connection = 4,
        }
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Setup AP: SSID=AtomSpectra-Setup, IP=192.168.4.1");

    xTaskCreate(dns_captive_task, "dns_captive", 4096, NULL, 5, NULL);

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 8;
    config.uri_match_fn = httpd_uri_match_wildcard;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) == ESP_OK) {
        const httpd_uri_t uris[] = {
            {"/",              HTTP_GET,  handle_setup_root,     NULL},
            {"/api/scan",      HTTP_GET,  handle_setup_scan,     NULL},
            {"/api/connect",   HTTP_POST, handle_setup_connect,  NULL},
            {"/api/field-mode",HTTP_POST, handle_setup_field,    NULL},  // #FIELD-2c
            {"/*",             HTTP_GET,  handle_setup_redirect, NULL},
        };
        for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++)
            httpd_register_uri_handler(server, &uris[i]);
        ESP_LOGI(TAG, "Captive portal started");
    }
}

/* ---- mDNS (atomspectra.local) ---- */

static void start_mdns(void)
{
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mDNS init failed: %s", esp_err_to_name(err));
        return;
    }
    mdns_hostname_set("atomspectra");
    mdns_instance_name_set("AtomSpectra Gateway");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    ESP_LOGI(TAG, "mDNS started: http://atomspectra.local/");
}

/* ---- Полевой AP (Outdoor, NET_MODE_FIELD_AP) ---- */

// #FIELD-1/#SEC-2: подтянуть SSID/пароль AP из NVS (иначе — документированные дефолты).
static void load_ap_config(void)
{
    nvs_handle_t nvs;
    if (nvs_open("wifi", NVS_READONLY, &nvs) != ESP_OK) return;
    size_t len = sizeof(s_ap_ssid);
    nvs_get_str(nvs, "ap_ssid", s_ap_ssid, &len);   // not-found → дефолт цел
    len = sizeof(s_ap_pass);
    if (nvs_get_str(nvs, "ap_pass", s_ap_pass, &len) == ESP_OK)
        s_ap_pass_default = false;                   // пароль был задан оператором
    nvs_close(nvs);
}

// #FIELD-1: полевой AP-режим. AP-only (не APSTA), полный Web UI поднимает main.c.
// DNS-hijack + mDNS здесь; captive-пробы и REST — в web_server (#FIELD-4).
static void start_field_ap(void)
{
    s_mode = NET_MODE_FIELD_AP;
    load_ap_config();

    esp_netif_create_default_wifi_ap();

    wifi_config_t ap = {
        .ap = {
            .channel        = 1,   // #FIELD-1/A10: мягкое требование (AP-only гарантирует)
            .max_connection = 4,
            .authmode = (strlen(s_ap_pass) >= 8) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN,
        }
    };
    strlcpy((char *)ap.ap.ssid, s_ap_ssid, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen(s_ap_ssid);
    strlcpy((char *)ap.ap.password, s_ap_pass, sizeof(ap.ap.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "FIELD-1: Outdoor AP up: SSID=%s auth=%s IP=192.168.4.1",
             s_ap_ssid, (ap.ap.authmode == WIFI_AUTH_OPEN) ? "OPEN" : "WPA2");

    xTaskCreate(dns_captive_task, "dns_captive", 4096, NULL, 5, NULL);
    start_mdns();
}

/* ---- #WIFI-2: повтор подключения к домашней сети раз в RETRY_SEC ---- */

static void retry_timer_cb(void *arg)
{
    (void)arg;
    if (!s_trying && !wifi_is_connected()) esp_wifi_connect();   // отказ снова заведёт таймер
}

/* ---- STA mode ---- */

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
        if (s_trying) {   // #WIFI-1: проба новой сети — решает wifi_manager_try_sta()
            s_try_reason = ((wifi_event_sta_disconnected_t *)data)->reason;
            xEventGroupSetBits(s_wifi_events, WIFI_TRY_FAIL_BIT);
            return;
        }
        if (s_retry_count < MAX_RETRY) {
            esp_wifi_connect();
            s_retry_count++;
            ESP_LOGW(TAG, "Reconnecting (%d/%d)...", s_retry_count, MAX_RETRY);
        } else if (s_retry_timer && !esp_timer_is_active(s_retry_timer)) {
            // #WIFI-2: без перезагрузки — следующая попытка через RETRY_SEC
            if (s_retry_count == MAX_RETRY) {
                ESP_LOGW(TAG, "WiFi: home network not found, retry every %d s", RETRY_SEC);
                s_retry_count++;   // сообщение — один раз за серию
            }
            esp_timer_start_once(s_retry_timer, (uint64_t)RETRY_SEC * 1000000);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Connected, IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_count = 0;
        if (s_retry_timer) esp_timer_stop(s_retry_timer);
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

void wifi_manager_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    s_wifi_events = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // #FIELD-2: режимные флаги NVS
    uint8_t ap_mode = 0, ap_fb_once = 0;
    nvs_handle_t nvs;
    if (nvs_open("wifi", NVS_READONLY, &nvs) == ESP_OK) {
        nvs_get_u8(nvs, "ap_mode", &ap_mode);
        nvs_get_u8(nvs, "ap_fb_once", &ap_fb_once);
        nvs_close(nvs);
    }

    // #FIELD-2a: одноразовый fallback после неудачной STA — войти в поле, сбросить флаг.
    // Не липкий: следующий ребут снова пробует STA (FIELD-3). С #WIFI-2 флаг не ставится —
    // ветка осталась для флага, записанного прежней прошивкой.
    if (ap_fb_once) {
        if (nvs_open("wifi", NVS_READWRITE, &nvs) == ESP_OK) {
            nvs_erase_key(nvs, "ap_fb_once");
            nvs_commit(nvs);
            nvs_close(nvs);
        }
        ESP_LOGW(TAG, "FIELD-2a: entering field AP (one-shot fallback)");
        s_ap_forced = false;   // #FIELD-6: это fallback, не липкий forced
        start_field_ap();
        return;
    }

    // #FIELD-2b: forced Outdoor (липкий; снимается переключателем Indoor/Outdoor в UI).
    if (ap_mode) {
        ESP_LOGI(TAG, "FIELD-2b: forced Outdoor (ap_mode=1)");
        s_ap_forced = true;    // #FIELD-6: липкий forced (снимается переключателем)
        start_field_ap();
        return;
    }

    // STA-конфиг из NVS
    wifi_config_t wifi_config = {0};
    if (nvs_open("wifi", NVS_READONLY, &nvs) == ESP_OK) {
        size_t len = sizeof(wifi_config.sta.ssid);
        nvs_get_str(nvs, "ssid", (char *)wifi_config.sta.ssid, &len);
        len = sizeof(wifi_config.sta.password);
        nvs_get_str(nvs, "pass", (char *)wifi_config.sta.password, &len);
        nvs_close(nvs);
        ESP_LOGI(TAG, "WiFi from NVS: SSID=%s", wifi_config.sta.ssid);
    }

    if (wifi_config.sta.ssid[0] == 0) {
        ESP_LOGW(TAG, "No WiFi config, starting setup captive portal");
        start_captive_portal();
        return;
    }

    // #FIELD-1: Indoor (STA); #WIFI-2 — без ухода в AP, повторы раз в RETRY_SEC.
    esp_event_handler_instance_t inst_any, inst_got_ip;
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
        &wifi_event_handler, NULL, &inst_any);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
        &wifi_event_handler, NULL, &inst_got_ip);

    esp_netif_create_default_wifi_sta();
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    start_mdns();
    s_mode = NET_MODE_STA;

    const esp_timer_create_args_t targs = {
        .callback = retry_timer_cb,
        .name = "wifi_retry",
    };
    esp_timer_create(&targs, &s_retry_timer);   // заводит обработчик отключений

    ESP_LOGI(TAG, "WiFi STA starting, SSID=%s", wifi_config.sta.ssid);

}

bool wifi_is_connected(void)
{
    return (xEventGroupGetBits(s_wifi_events) & WIFI_CONNECTED_BIT) != 0;
}

net_run_mode_t wifi_manager_mode(void)
{
    return s_mode;
}

bool wifi_manager_is_ap_mode(void)
{
    return s_mode != NET_MODE_STA;   // FIELD_AP или SETUP
}

int wifi_manager_ap_clients(void)
{
    if (s_mode == NET_MODE_STA) return -1;
    wifi_sta_list_t list;
    if (esp_wifi_ap_get_sta_list(&list) == ESP_OK) return list.num;
    return 0;
}

const char *wifi_manager_ap_ssid(void)
{
    if (s_mode == NET_MODE_FIELD_AP) return s_ap_ssid;
    if (s_mode == NET_MODE_SETUP)    return "AtomSpectra-Setup";
    return "";
}

bool wifi_manager_ap_forced(void)
{
    return s_ap_forced;
}

// #FIELD-6 / #TFT-29: режим сети на следующий старт — общий для веба
// (/api/net-mode) и экрана платы (вкладка Conf). outdoor -> NVS ap_mode=1
// (липкий forced AP), indoor -> снять ap_mode. Применяется перезагрузкой —
// её делает вызывающий, после ответа клиенту.
esp_err_t wifi_manager_set_boot_mode(bool outdoor)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("wifi", NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    if (outdoor) err = nvs_set_u8(nvs, "ap_mode", 1);
    else         nvs_erase_key(nvs, "ap_mode");   // -> Indoor (not-found ок)
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

bool wifi_manager_ap_pass_is_default(void)
{
    return s_ap_pass_default;
}

// ---------------- #WIFI-1: настройка WiFi с экрана платы ----------------

int wifi_manager_scan(wifi_net_t *out, int max)
{
    if (!out || max <= 0) return -1;
    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_wifi_get_mode(&mode);
    bool ap_only = (mode == WIFI_MODE_AP);
    // В режимах точки доступа сканировать можно только в APSTA — на время поиска.
    if (ap_only && esp_wifi_set_mode(WIFI_MODE_APSTA) != ESP_OK) return -1;
    wifi_scan_config_t sc = { .show_hidden = false };
    esp_err_t e = esp_wifi_scan_start(&sc, true);   // блокирующий, ~2-3 с
    int n = -1;
    if (e == ESP_OK) {
        uint16_t num = 0;
        esp_wifi_scan_get_ap_num(&num);
        wifi_ap_record_t *rec = num ? calloc(num, sizeof(*rec)) : NULL;
        if (num && rec && esp_wifi_scan_get_ap_records(&num, rec) == ESP_OK) {
            n = 0;
            for (int i = 0; i < num && n < max; i++) {   // записи уже по убыванию сигнала
                if (!rec[i].ssid[0]) continue;
                bool dup = false;   // одна сеть на нескольких точках — показать лучшую
                for (int k = 0; k < n && !dup; k++)
                    dup = strcmp(out[k].ssid, (const char *)rec[i].ssid) == 0;
                if (dup) continue;
                strlcpy(out[n].ssid, (const char *)rec[i].ssid, sizeof(out[n].ssid));
                out[n].rssi = rec[i].rssi;
                out[n].secure = rec[i].authmode != WIFI_AUTH_OPEN;
                n++;
            }
        } else if (num == 0) {
            n = 0;
        }
        free(rec);
    }
    if (ap_only) esp_wifi_set_mode(WIFI_MODE_AP);
    return n;
}

bool wifi_manager_sta_info(char *ssid, size_t cap, int *rssi)
{
    if (s_mode != NET_MODE_STA || !wifi_is_connected()) return false;
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return false;
    if (ssid) strlcpy(ssid, (const char *)ap.ssid, cap);
    if (rssi) *rssi = ap.rssi;
    return true;
}

static esp_err_t save_sta(const char *ssid, const char *pass)
{
    nvs_handle_t nvs;
    esp_err_t e = nvs_open("wifi", NVS_READWRITE, &nvs);
    if (e != ESP_OK) return e;
    // Пароль — открытым текстом, как и из портала настройки (см. #SEC-1 выше).
    e = nvs_set_str(nvs, "ssid", ssid);
    if (e == ESP_OK) e = nvs_set_str(nvs, "pass", pass ? pass : "");
    nvs_erase_key(nvs, "ap_mode");      // -> Indoor
    nvs_erase_key(nvs, "ap_fb_once");
    if (e == ESP_OK) e = nvs_commit(nvs);
    nvs_close(nvs);
    if (e == ESP_OK) ESP_LOGI(TAG, "#WIFI-1: WiFi config saved: SSID=%s", ssid);
    return e;
}

int wifi_manager_try_sta(const char *ssid, const char *pass, int timeout_ms)
{
    if (!ssid || !ssid[0] || strlen(ssid) >= WIFI_SSID_MAX || (pass && strlen(pass) >= WIFI_PASS_MAX))
        return WIFI_TRY_BAD_ARG;
    if (s_mode != NET_MODE_STA) {
        // Точка доступа: STA-интерфейса и обработчиков нет — сохранить, Indoor, перезагрузка.
        // Не та сеть/пароль — плата останется в Indoor и будет искать сеть (#WIFI-2);
        // обратно в точку доступа — Conf -> Outdoor.
        return save_sta(ssid, pass) == ESP_OK ? WIFI_TRY_SAVED_REBOOT : WIFI_TRY_FAILED;
    }
    wifi_config_t old = {0};
    esp_wifi_get_config(WIFI_IF_STA, &old);
    if (s_retry_timer) esp_timer_stop(s_retry_timer);

    s_trying = true;
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(300));   // наше отключение пришло событием — не считать отказом
    xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT | WIFI_TRY_FAIL_BIT);
    s_try_reason = 0;

    wifi_config_t nc = {0};
    strlcpy((char *)nc.sta.ssid, ssid, sizeof(nc.sta.ssid));
    strlcpy((char *)nc.sta.password, pass ? pass : "", sizeof(nc.sta.password));
    esp_wifi_set_config(WIFI_IF_STA, &nc);
    ESP_LOGI(TAG, "#WIFI-1: trying SSID=%s", ssid);

    int64_t until = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    bool ok = false;
    int attempts = 0;
    while (!ok && esp_timer_get_time() < until && attempts < 3) {
        attempts++;
        xEventGroupClearBits(s_wifi_events, WIFI_TRY_FAIL_BIT);
        esp_wifi_connect();
        int left_ms = (int)((until - esp_timer_get_time()) / 1000);
        if (left_ms < 100) left_ms = 100;
        EventBits_t b = xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT | WIFI_TRY_FAIL_BIT,
                                            pdFALSE, pdFALSE, pdMS_TO_TICKS(left_ms));
        ok = (b & WIFI_CONNECTED_BIT) != 0;
        if (!ok && (b & WIFI_TRY_FAIL_BIT)) vTaskDelay(pdMS_TO_TICKS(500));
    }
    uint8_t reason = s_try_reason;
    if (ok) {
        s_trying = false;
        s_retry_count = 0;
        save_sta(ssid, pass);
        return WIFI_TRY_OK;
    }
    // Не вышло — вернуть прежнюю сеть; её штатные повторы и уход в AP — как раньше.
    ESP_LOGW(TAG, "#WIFI-1: SSID=%s failed (reason %u) — back to SSID=%s", ssid, reason, old.sta.ssid);
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(300));
    s_trying = false;
    s_retry_count = 0;
    esp_wifi_set_config(WIFI_IF_STA, &old);
    esp_wifi_connect();   // отказы прежней сети — штатные повторы (#WIFI-2)
    switch (reason) {
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_MIC_FAILURE:
        return WIFI_TRY_BAD_PASS;
    case WIFI_REASON_NO_AP_FOUND:
        return WIFI_TRY_NO_AP;
    default:
        return WIFI_TRY_FAILED;
    }
}
