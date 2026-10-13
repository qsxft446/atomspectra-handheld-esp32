#include "atomspectra.h"
#include "shproto.h"
#include "web_waterfall.h"
#include "web_util.h"
#include "boot_config.h"
#include "spectrogram.h"    // #FIELD-5: spectrogram_time_synced() при установке времени
#include "net_time.h"       // #FIELD-5: guard-логика источника времени
#include "monitor.h"        // #MON-1: серия CPS-мониторинга (/api/monitor/series)
#include "battery.h"        // #PWR-1: статус CG_Charger_BQ-2v0 (BQ25895) для значка батареи
#include "tft_display.h"    // #TFT-21: калибровка тача дисплея со страницы «Система»
#include "rtc_ds3231.h"     // #RTC-1: запись времени в DS3231, статус в /api/system
#include "sd_card.h"        // #SD-1: статус SD-карты в /api/system
#include "sys_load.h"       // #SYS-1: загрузка ядер ESP32 в /api/system
#include "web_files.h"      // #FS-1: браузер файлов страницы «Файлы»
#include "ota_update.h"     // #OTA-1: обновление прошивки по WiFi
#include "sd_log.h"         // #SD-3: статус записи мониторинга на карту
#include "esp_netif.h"      // #ETH-38: esp_netif_get_ip_info для реального WiFi IP
#include "esp_heap_caps.h"  // #MON-1: PSRAM-буфер чанка серии
#include "esp_log.h"
#include "esp_http_server.h"
#include "cJSON.h"
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <inttypes.h>
#include <stdlib.h>
#include <time.h>
#include <stddef.h>
#include <math.h>
#include <sys/time.h>       // #FIELD-5: gettimeofday/settimeofday
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_app_desc.h"   // #FW-28: esp_app_get_description() → версия прошивки
#include "esp_wifi.h"
#include "esp_timer.h"
#include "esp_littlefs.h"
#include "cps_alarm.h"         // #ALM-1
#include "esp_random.h"
#include <dirent.h>

static const char *TAG = "web";

// #FIELD-9 (A11): часы платы «синхронизированы», если время не near-epoch (< 2023-11).
// В полевом AP до прихода браузерного времени часы = 1970; экспорт помечает это и НЕ
// падает (localtime_r сам не падает). Пометка добавляется ТОЛЬКО при рассинхроне →
// нормальные записи байт-точны к эталонным форматам (#EXP-1 N42/#CSV-1). Проверка — T14.
#define WF_TIME_SYNCED_EPOCH 1700000000L   // 2023-11-14 UTC
static inline bool time_is_synced(time_t t) { return t >= WF_TIME_SYNCED_EPOCH; }

// CSRF-токен: генерируется при старте, выдаётся по GET /api/csrf-token,
// требуется в заголовке X-CSRF-Token на всех мутирующих POST. Защищает
// открытый-в-LAN Web UI от drive-by cross-origin POST: сторонняя страница
// не может прочитать токен (same-origin policy) → не может подделать запрос.
static char s_csrf[33];

static void csrf_generate(void)
{
    static const char hx[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++)
        s_csrf[i] = hx[esp_random() & 0x0F];
    s_csrf[32] = '\0';
}

// P3-9: сравнение токена за постоянное время — не утекает позиция
// первого несовпавшего байта по времени ответа.
static bool csrf_ct_eq(const char *a, const char *b, size_t n)
{
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++) d |= (uint8_t)a[i] ^ (uint8_t)b[i];
    return d == 0;
}

// Проверяет X-CSRF-Token. При несовпадении сам шлёт 403 и возвращает false.
static bool csrf_check(httpd_req_t *req)
{
    char hdr[40] = {0};
    if (httpd_req_get_hdr_value_str(req, "X-CSRF-Token", hdr, sizeof(hdr)) == ESP_OK
        && s_csrf[0] && strlen(hdr) == 32 && csrf_ct_eq(hdr, s_csrf, 32))
        return true;
    httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Bad or missing CSRF token");
    return false;
}

// Экспорт CSRF-проверки для web_waterfall.c (csrf_check здесь static).
bool web_csrf_check(httpd_req_t *req)
{
    return csrf_check(req);
}

static esp_err_t handle_csrf_token(httpd_req_t *req)
{
    char buf[48];
    int n = snprintf(buf, sizeof(buf), "{\"token\":\"%s\"}", s_csrf);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, n);
    return ESP_OK;
}

static int parse_saved_index(const char *uri)
{
    const char *p = strstr(uri, "/api/saved/");
    if (!p) return -1;
    return atoi(p + 11);
}

static esp_err_t handle_status(httpd_req_t *req)
{
    const spectrum_data_t *sp = spectrum_get_current();
    const device_info_t   *di = spectrum_get_device_info();

    cJSON *root = cJSON_CreateObject();
    if (!root) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom"); return ESP_FAIL; }
    cJSON_AddBoolToObject(root, "analyzer_connected", usb_host_cdc_is_connected());
    cJSON_AddBoolToObject(root, "wifi_connected", wifi_is_connected());
    cJSON_AddNumberToObject(root, "total_counts", sp->total_counts);
    cJSON_AddNumberToObject(root, "cpu_load", sp->cpu_load);
    cJSON_AddBoolToObject(root, "tcp_client", tcp_bridge_client_connected());

    if (di->valid) {
        cJSON_AddNumberToObject(root, "dev", di->dev);
        cJSON_AddNumberToObject(root, "version", di->version);
        cJSON_AddNumberToObject(root, "mode", di->mode);
        cJSON_AddNumberToObject(root, "freq", di->freq);
        cJSON_AddNumberToObject(root, "t1", di->t1);
        cJSON_AddNumberToObject(root, "t2", di->t2);
        cJSON_AddNumberToObject(root, "t3", di->t3);
        cJSON_AddNumberToObject(root, "time", di->time_sec);
        cJSON_AddNumberToObject(root, "noise", di->noise);
        cJSON_AddNumberToObject(root, "max", di->max_integral);
    }

    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    free(json);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t handle_spectrum(httpd_req_t *req)
{
    spectrum_data_t *sp = malloc(sizeof(*sp));
    if (!sp) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    if (!spectrum_get_snapshot(sp)) {
        free(sp);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "No spectrum data yet");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline");
    httpd_resp_send(req, (const char *)sp->bins, SPECTRUM_CHANNELS * sizeof(uint32_t));
    free(sp);
    return ESP_OK;
}

// #DT-4: мёртвое время берём ИЗ ДЕТЕКТОРА. STAT-кадр (cmd 0x04) несёт pulse_width
// [offset 14, u32] = суммарная ширина импульсов (отсчёты АЦП) — реально измеренное прибором
// занятое время (учитывает наложения). dead = pulse_width/F; live = total - dead.
// Фоллбэк (#DT-1, модель total_counts*(RISE+FALL+Srise+Sfall)/F) — для прошивок без pulse_width
// (kADR<18 байт → pulse_width==0) или пока -inf не прочитан / F<=0.
static float compute_live_time(const spectrum_data_t *sp)
{
    const device_info_t *di = spectrum_get_device_info();
    float total = (float)sp->total_time_sec;
    if (!di->valid || di->freq <= 0.0f) return total;
    // #DT-4: метод BecqMoni (эталон Am6er, Utils/LiveTime.cs + AtomSpectraVCPDeviceForm.cs).
    // Мёртвое время на импульс τ = (RISE+FALL+1)/F (RISE/FALL/F из -inf, т.е. из прибора).
    // Мёртвое время за набор = (valid+invalid импульсы)·τ; TotalPulseCount = sum + InvalidPulses.
    float tau = ((float)di->rise + (float)di->fall + 1.0f) / di->freq;
    float dead = (float)(sp->total_counts + sp->lost_impulses) * tau;
    if (dead < 0.0f) dead = 0.0f;
    if (dead > total) dead = total;
    return total - dead;
}

static esp_err_t render_spectrum_json(httpd_req_t *req, const spectrum_data_t *sp)
{
    char *buf = malloc(4096);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "{\"bins\":[");
    int pos = 0;
    for (int i = 0; i < SPECTRUM_CHANNELS; i++) {
        pos += snprintf(buf + pos, 4096 - pos, "%s%" PRIu32, (i > 0) ? "," : "", sp->bins[i]);
        if (pos > 3800) {
            httpd_resp_send_chunk(req, buf, pos);
            pos = 0;
        }
    }
    if (pos > 0) httpd_resp_send_chunk(req, buf, pos);
    uint32_t hist_ok = 0, hist_drop = 0;
    spectrum_get_hist_stats(&hist_ok, &hist_drop);
    int dead = usb_host_cdc_spectrometer_dead() ? 1 : 0;   // #FW-43: «определился, но не запитан»
    int n = snprintf(buf, 4096,
        "],\"total\":%" PRIu32 ",\"cpu\":%u,\"cps\":%" PRIu32 ",\"lost\":%" PRIu32 ",\"time\":%" PRIu32 ",\"live\":%.1f,"
        "\"bridge_drop\":%" PRIu32 ",\"usb_rx_err\":%" PRIu32 ",\"rx_ring_drops\":%" PRIu32 ","
        "\"hist_ok\":%" PRIu32 ",\"hist_drop\":%" PRIu32 ","
        "\"t1\":%.1f,\"t2\":%.1f,\"t3\":%.1f,\"serial\":\"%s\",\"dead\":%d",
        sp->total_counts, sp->cpu_load, sp->cps, sp->lost_impulses,
        sp->total_time_sec, compute_live_time(sp),
        tcp_bridge_dropped_bytes(), usb_host_cdc_rx_errors(), usb_host_cdc_rx_ring_drops(),
        hist_ok, hist_drop,
        sp->temperature[0], sp->temperature[1], sp->temperature[2],
        sp->serial_number[0] ? sp->serial_number : "", dead);
    httpd_resp_send_chunk(req, buf, n);
    // #FS-1: время сохранения — браузеру файлов (дата файла на карте у перенесённых
    // с флеша спектров — время переноса, а не сохранения).
    n = snprintf(buf, 4096, ",\"saved_at\":%lld", (long long)sp->saved_at);
    httpd_resp_send_chunk(req, buf, n);
    if (sp->calib_valid) {
        int p = snprintf(buf, 4096, ",\"calib\":[");
        for (int i = 0; i <= sp->calib_order; i++)
            p += snprintf(buf + p, 4096 - p, "%s%.15g", i ? "," : "", sp->calibration[i]);
        snprintf(buf + p, 4096 - p, "]");
        httpd_resp_sendstr_chunk(req, buf);
    }
    httpd_resp_sendstr_chunk(req, "}");
    httpd_resp_send_chunk(req, NULL, 0);
    free(buf);
    return ESP_OK;
}

static esp_err_t handle_spectrum_json(httpd_req_t *req)
{
    spectrum_data_t *sp = malloc(sizeof(*sp));
    if (!sp) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    if (!spectrum_get_snapshot(sp)) {
        free(sp);
        // #FW-43: дед-флаг доступен даже без валидного снапшота спектра. Дед-состояние
        // (прибор определился, но не запитан) часто = НЕТ данных спектра → снапшот
        // невалиден → сюда. Без заголовка баннер бы не показался (dead:N живёт только в
        // render_spectrum_json на валидном пути). Фронт читает заголовок на каждом 404.
        httpd_resp_set_hdr(req, "X-Spectrometer-Dead", usb_host_cdc_spectrometer_dead() ? "1" : "0");
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "No spectrum data yet");
        return ESP_FAIL;
    }
    esp_err_t ret = render_spectrum_json(req, sp);
    free(sp);
    return ret;
}

static esp_err_t handle_command(httpd_req_t *req)
{
    if (!csrf_check(req)) return ESP_FAIL;
    char body[256] = {0};
    int recv_len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (recv_len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    body[recv_len] = '\0';
    // #ACQ-1/#SB-1/#CAL-1: общий путь текстовых команд — учитывает намерение набора,
    // обнуляет базу при -rst и читает калибровку перед стартом.
    int ret = usb_host_send_text_command(body);
    httpd_resp_sendstr(req, ret == 0 ? "{\"ok\":true}" : "{\"ok\":false}");
    return ESP_OK;
}

// #UI-1: лог текстовых ответов прибора для веб-UI. GET /api/devlog?since=N
// Read-only (CSRF не нужен). Отдаёт {"lines":[{"seq":N,"text":"..."}],"next":M}.
static esp_err_t handle_devlog(httpd_req_t *req)
{
    uint32_t since = 0;
    char q[48], v[16];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
        httpd_query_key_value(q, "since", v, sizeof(v)) == ESP_OK)
        since = strtoul(v, NULL, 10);
    char *buf = malloc(9000);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    usb_host_cdc_devlog_json(since, buf, 9000);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    free(buf);
    return ESP_OK;
}

// #MON-1: серия CPS-мониторинга с платы. GET /api/monitor/series?since=<seq>
// Read-only (CSRF не нужен, как /api/devlog). Ответ:
//   {"epoch":E,"next_seq":N,"first_seq":F,"interval_base":1,
//    "sess":{"rev":R,"run":b,"start":S,"stop":P,"iv":I,"sma":W},
//    "samples":[[end_sec,dur,counts,wall],...]}
// #MON-2: sess — общая сессия мониторинга платы; wall — настенное время сэмпла
// (Unix, с; 0 — часы платы не выставлены), одинаковое для всех клиентов.
// samples — от max(since+1, старейший в кольце), чанк <= 2000 шт; клиент
// дотягивает циклом, пока не догонит next_seq. Стрим httpd_resp_send_chunk
// (паттерн h_export_aswf) — без гигантского JSON-дерева в куче.
#define MON_SERIES_CHUNK 2000   // 2000 × 14 Б = ~28 КБ PSRAM на время запроса

static int monitor_sess_json(const monitor_session_t *ss, char *out, size_t n)
{
    return snprintf(out, n,
        "{\"rev\":%" PRIu32 ",\"run\":%s,\"start\":%" PRIu32 ",\"stop\":%" PRIu32
        ",\"iv\":%u,\"sma\":%u}",
        ss->rev, ss->running ? "true" : "false", ss->start_seq, ss->stop_seq,
        (unsigned)ss->interval_s, (unsigned)ss->sma_win);
}

// #MON-2: POST /api/monitor/session (CSRF) — управление общей сессией.
// Тело: {"action":"start","interval":10} | {"action":"stop"} | {"action":"clear"}
//       | {"sma":10}. Ответ — сессия после изменения (формат "sess" серии).
static esp_err_t handle_monitor_session(httpd_req_t *req)
{
    if (!csrf_check(req)) return ESP_FAIL;
    char body[128];
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no body"); return ESP_FAIL; }
    body[len] = '\0';
    cJSON *root = cJSON_Parse(body);
    if (!root) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json"); return ESP_FAIL; }

    const cJSON *act = cJSON_GetObjectItem(root, "action");
    const cJSON *iv  = cJSON_GetObjectItem(root, "interval");
    const cJSON *sma = cJSON_GetObjectItem(root, "sma");
    bool ok = true;
    if (cJSON_IsNumber(sma)) {   // кламп до приведения к uint16; точный — в monitor.c
        double w = sma->valuedouble;
        monitor_session_set_sma((uint16_t)(w < 0 ? 0 : w > MON_SMA_MAX ? MON_SMA_MAX : w));
    }
    if (cJSON_IsString(act)) {
        if (!strcmp(act->valuestring, "start")) {
            double v = cJSON_IsNumber(iv) ? iv->valuedouble : 0;
            // Интервал хранится в uint16: сверху кламп по формату (18 ч), а не по вебу.
            if (v > UINT16_MAX) v = UINT16_MAX;
            monitor_session_start(v >= MON_INTERVAL_MIN ? (uint16_t)v : 0);
        } else if (!strcmp(act->valuestring, "stop")) {
            monitor_session_stop();
        } else if (!strcmp(act->valuestring, "clear")) {
            monitor_session_clear();
        } else {
            ok = false;
        }
    }
    cJSON_Delete(root);
    if (!ok) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad action"); return ESP_FAIL; }

    monitor_session_t ss;
    monitor_session_get(&ss);
    char out[160];
    int n = monitor_sess_json(&ss, out, sizeof out);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, out, n);
    return ESP_OK;
}

static esp_err_t handle_monitor_series(httpd_req_t *req)
{
    uint32_t since = 0;
    char q[48], v[16];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
        httpd_query_key_value(q, "since", v, sizeof(v)) == ESP_OK)
        since = strtoul(v, NULL, 10);

    size_t cap = MON_SERIES_CHUNK;
    monitor_sample_t *smp = heap_caps_malloc(cap * sizeof(*smp), MALLOC_CAP_SPIRAM);
    if (!smp) { cap = 400; smp = malloc(cap * sizeof(*smp)); }  // fallback: internal, чанк меньше
    char *buf = malloc(2048);
    if (!smp || !buf) {
        free(smp);
        free(buf);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }

    uint32_t first = 0, next = 0, epoch = 0;
    monitor_session_t ss;
    size_t n = monitor_copy_since(since, smp, cap, &first, &next, &epoch, &ss);

    httpd_resp_set_type(req, "application/json");
    int pos = snprintf(buf, 2048,
        "{\"epoch\":%" PRIu32 ",\"next_seq\":%" PRIu32 ",\"first_seq\":%" PRIu32
        ",\"interval_base\":1,\"sd_log\":%s,\"sess\":", epoch, next, first,
        sd_log_mon_active() ? "true" : "false");   // #SD-3: сессия пишется на карту
    pos += monitor_sess_json(&ss, buf + pos, 2048 - pos);
    pos += snprintf(buf + pos, 2048 - pos, ",\"samples\":[");
    for (size_t i = 0; i < n; i++) {
        pos += snprintf(buf + pos, 2048 - pos, "%s[%" PRIu32 ",%u,%" PRIu32 ",%" PRIu32 "]",
                        i ? "," : "", smp[i].end_sec, (unsigned)smp[i].dur, smp[i].counts,
                        monitor_wall_of(smp[i].up_sec));
        if (pos > 1900) { httpd_resp_send_chunk(req, buf, pos); pos = 0; }
    }
    pos += snprintf(buf + pos, 2048 - pos, "]}");
    httpd_resp_send_chunk(req, buf, pos);
    httpd_resp_send_chunk(req, NULL, 0);   // терминатор чанк-стрима
    free(smp);
    free(buf);
    return ESP_OK;
}

static esp_err_t handle_reset(httpd_req_t *req)
{
    if (!csrf_check(req)) return ESP_FAIL;
    usb_host_send_text_command("-rst");
    spectrum_reset();
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

// #FW-2/#FW-3: GET текущих настроек «Поведение при старте платы» (NVS).
static esp_err_t handle_boot_config_get(httpd_req_t *req)
{
    boot_config_t bc;
    boot_config_load(&bc);
    char resp[256];
    // #FW-42: name_prefix санитизирован в NVS ([A-Za-z0-9_-]) → JSON-escape не нужен.
    snprintf(resp, sizeof(resp),
        "{\"autostart_spectrum\":%s,\"autostart_waterfall\":%s,"
        "\"clear_spectrum\":%s,\"clear_waterfall\":%s,\"calib_always\":%s,\"name_prefix\":\"%s\"}",
        bc.autostart_spectrum  ? "true" : "false",
        bc.autostart_waterfall ? "true" : "false",
        bc.clear_spectrum      ? "true" : "false",
        bc.clear_waterfall     ? "true" : "false",
        bc.calib_always        ? "true" : "false",   // #CAL-1
        bc.name_prefix);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

// #ALM-1: сигнализация CPS — включение и 4 порога (NVS "alarm"), level — текущий уровень.
static esp_err_t handle_alarm_get(httpd_req_t *req)
{
    cps_alarm_cfg_t c;
    cps_alarm_diag_t d;
    cps_alarm_get_config(&c);
    cps_alarm_diag(&d);
#define MN(v) ((v) == INT32_MAX ? -1L : (long)(v))
    char resp[400];
    // #ALM-3: diag — фактические длительности ритма, мкс (-1 — не было).
    snprintf(resp, sizeof(resp), "{\"enabled\":%s,\"th\":[%lu,%lu,%lu,%lu],\"level\":%d,"
             "\"diag\":{\"level\":%d,\"beeps\":%lu,\"gaps\":%lu,\"pauses\":%lu,"
             "\"on\":[%ld,%ld],\"gap\":[%ld,%ld],\"pause\":[%ld,%ld]}}",
             c.enabled ? "true" : "false", (unsigned long)c.th[0], (unsigned long)c.th[1],
             (unsigned long)c.th[2], (unsigned long)c.th[3], cps_alarm_level(),
             d.level, (unsigned long)d.beeps, (unsigned long)d.gaps, (unsigned long)d.pauses,
             MN(d.on_min_us), (long)d.on_max_us, MN(d.gap_min_us), (long)d.gap_max_us,
             MN(d.pause_min_us), (long)d.pause_max_us);
#undef MN
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

static esp_err_t handle_alarm_set(httpd_req_t *req)
{
    if (!csrf_check(req)) return ESP_FAIL;
    char body[256] = {0};
    int recv_len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (recv_len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    body[recv_len] = '\0';
    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad JSON");
        return ESP_FAIL;
    }
    cps_alarm_cfg_t c;
    cps_alarm_get_config(&c);   // отсутствующие ключи — как есть
    cJSON *it;
    if ((it = cJSON_GetObjectItem(root, "enabled"))) c.enabled = cJSON_IsTrue(it);
    if ((it = cJSON_GetObjectItem(root, "th")) && cJSON_IsArray(it) && cJSON_GetArraySize(it) == CPS_ALARM_LEVELS) {
        for (int i = 0; i < CPS_ALARM_LEVELS; i++) {
            cJSON *v = cJSON_GetArrayItem(it, i);
            double d = cJSON_IsNumber(v) ? v->valuedouble : -1;
            c.th[i] = d >= 0 && d <= CPS_ALARM_TH_MAX ? (uint32_t)d : 0;   // 0 отвергнет проверка
        }
    }
    cJSON_Delete(root);
    int rc = cps_alarm_set_config(&c);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, rc == 0 ? "{\"ok\":true}" : rc == -1 ? "{\"ok\":false,\"err\":\"order\"}"
                                                               : "{\"ok\":false,\"err\":\"nvs\"}");
    return ESP_OK;
}

// #FW-2/#FW-3: POST настроек. Отсутствующие в теле ключи сохраняют текущее значение.
static esp_err_t handle_boot_config_set(httpd_req_t *req)
{
    if (!csrf_check(req)) return ESP_FAIL;
    char body[256] = {0};
    int recv_len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (recv_len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    body[recv_len] = '\0';
    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad JSON");
        return ESP_FAIL;
    }
    boot_config_t bc;
    boot_config_load(&bc);   // стартуем от текущего, перекрываем переданными ключами
    cJSON *it;
    if ((it = cJSON_GetObjectItem(root, "autostart_spectrum")))  bc.autostart_spectrum  = cJSON_IsTrue(it);
    if ((it = cJSON_GetObjectItem(root, "autostart_waterfall"))) bc.autostart_waterfall = cJSON_IsTrue(it);
    if ((it = cJSON_GetObjectItem(root, "clear_spectrum")))      bc.clear_spectrum      = cJSON_IsTrue(it);
    if ((it = cJSON_GetObjectItem(root, "clear_waterfall")))     bc.clear_waterfall     = cJSON_IsTrue(it);
    if ((it = cJSON_GetObjectItem(root, "calib_always")))        bc.calib_always        = cJSON_IsTrue(it);   // #CAL-1
    // #FW-42: префикс имён экспортов. boot_config_save() санитизирует ([A-Za-z0-9_-], усечение).
    if ((it = cJSON_GetObjectItem(root, "name_prefix")) && cJSON_IsString(it)) {
        strncpy(bc.name_prefix, it->valuestring, sizeof(bc.name_prefix) - 1);
        bc.name_prefix[sizeof(bc.name_prefix) - 1] = '\0';
    }
    cJSON_Delete(root);
    int rc = boot_config_save(&bc);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, rc == 0 ? "{\"ok\":true}" : "{\"ok\":false}");
    return ESP_OK;
}

static esp_err_t handle_save(httpd_req_t *req)
{
    if (!csrf_check(req)) return ESP_FAIL;
    int idx = spectrum_save_to_flash();
    char resp[64];
    if (idx >= 0) {
        // #SD-7: CSV-копии на карту больше нет — один файл spec_NNNN.bin.
        snprintf(resp, sizeof(resp), "{\"ok\":true,\"index\":%d,\"storage\":\"%s\"}", idx,
                 spectrum_saved_on_sd() ? "sd" : "flash");
    }
    else {
        // #FW-24: конкретная причина отказа в UI
        const char *err = (idx == -1) ? "novalid" : (idx == -2) ? "nospace" : (idx == -4) ? "nosd" : "fserr";
        snprintf(resp, sizeof(resp), "{\"ok\":false,\"err\":\"%s\"}", err);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

static esp_err_t handle_list(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "{\"spectra\":[");
    char path[80], item[160];
    int count = 0;
    // Перечисляем реальные файлы каталога, а не пробуем индексы подряд:
    // save() занимает первую дырку, поэтому после удалений индексы не
    // обязательно идут без разрывов — старый "стоп после 20 пропусков" терял
    // спектры с большими индексами. readdir видит все файлы.
    // #FW-24: спектры лежат в подкаталоге SPEC_DIR (/storage/spec) — отделены
    // от calib.bin/current.bin/wf_state.bin в корне mount. readdir здесь исправен
    // (корень тоже листится; пустой список был из-за %04d-бага в rebuild пути ниже).
    const char *sdir = spectrum_saved_dir();   // #SD-4: SD-карта или флеш
    DIR *dir = opendir(sdir);
    struct dirent *de;
    while (dir && (de = readdir(dir)) != NULL) {
        int i = -1;
        if (sscanf(de->d_name, "spec_%d.bin", &i) != 1 || i < 0) continue;
        // Путь восстанавливаем из разобранного индекса (%d ограничен), а не из
        // d_name (%s до 255 байт) — иначе -Werror=format-truncation.
        // #FW-24 КОРЕНЬ БАГА: save пишет spec_%04d.bin (нули), а тут было
        // spec_%d.bin → fopen "spec_0.bin" ≠ "spec_0000.bin" → NULL → continue →
        // КАЖДЫЙ файл пропускался → список всегда пуст. readdir был исправен.
        snprintf(path, sizeof(path), "%s/spec_%04d.bin", sdir, i);
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        uint32_t counts = 0, time_sec = 0;
        time_t saved_at = 0;
        fseek(f, offsetof(spectrum_data_t, total_counts), SEEK_SET);
        fread(&counts, 4, 1, f);
        fread(&time_sec, 4, 1, f);
        fseek(f, offsetof(spectrum_data_t, saved_at), SEEK_SET);
        fread(&saved_at, sizeof(time_t), 1, f);
        fclose(f);
        int n = snprintf(item, sizeof(item), "%s{\"index\":%d,\"counts\":%" PRIu32 ",\"time\":%" PRIu32 ",\"saved_at\":%ld}",
            count > 0 ? "," : "", i, counts, time_sec, (long)saved_at);
        httpd_resp_send_chunk(req, item, n);
        count++;
    }
    if (dir) closedir(dir);
    char tail[32];
    int n = snprintf(tail, sizeof(tail), "],\"count\":%d}", count);
    httpd_resp_send_chunk(req, tail, n);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

#define EMBED_HTML_HANDLER(fn,sym) static esp_err_t fn(httpd_req_t *req){ \
    extern const uint8_t sym##_start[] asm("_binary_" #sym "_start"); \
    extern const uint8_t sym##_end[]   asm("_binary_" #sym "_end"); \
    httpd_resp_set_type(req,"text/html"); \
    httpd_resp_send(req,(const char *)sym##_start, sym##_end - sym##_start); \
    return ESP_OK; }
EMBED_HTML_HANDLER(handle_index,        index_html)
EMBED_HTML_HANDLER(handle_saved_page,   saved_html)
EMBED_HTML_HANDLER(handle_system_page,  system_html)
EMBED_HTML_HANDLER(handle_service_page, service_html)
EMBED_HTML_HANDLER(handle_monitor_page, monitor_html)
EMBED_HTML_HANDLER(handle_captive_page, captive_html)   // #FIELD-10/#FIELD-11: лёгкая captive-landing

// #FIELD-5: общий JS авто-синхронизации времени (application/javascript, не text/html).
static esp_err_t handle_common_time_js(httpd_req_t *req)
{
    extern const uint8_t common_time_js_start[] asm("_binary_common_time_js_start");
    extern const uint8_t common_time_js_end[]   asm("_binary_common_time_js_end");
    httpd_resp_set_type(req, "application/javascript");
    httpd_resp_send(req, (const char *)common_time_js_start, common_time_js_end - common_time_js_start);
    return ESP_OK;
}

static esp_err_t render_spectrum_xml(httpd_req_t *req, const spectrum_data_t *sp, const char *filename)
{
    char *buf = malloc(4096);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/xml");
    char named[48];
    web_build_export_name(named, sizeof(named), filename);   // #FW-42: префикс
    char disp[80];
    snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", named);
    httpd_resp_set_hdr(req, "Content-Disposition", disp);

    httpd_resp_sendstr_chunk(req,
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
        "<ResultDataFile xmlns:xsd=\"http://www.w3.org/2001/XMLSchema\""
        " xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\">\r\n"
        "  <FormatVersion>120920</FormatVersion>\r\n"
        "  <ResultDataList>\r\n"
        "    <ResultData>\r\n");

    time_t end_time = (sp->saved_at > 0) ? sp->saved_at : time(NULL);
    struct tm ts, te;
    time_t t_start = end_time - sp->total_time_sec;
    localtime_r(&t_start, &ts);
    localtime_r(&end_time, &te);

    // #FIELD-9 (A11): near-epoch → XML-комментарий (валиден, парсеры игнорируют); только при рассинхроне
    if (!time_is_synced(end_time))
        httpd_resp_sendstr_chunk(req, "      <!-- TIME NOT SYNCHRONIZED: board clock near-epoch, timestamps unreliable -->\r\n");

    char serial_esc[160];
    web_xml_escape(sp->serial_number[0] ? sp->serial_number : "AtomSpectra",
               serial_esc, sizeof(serial_esc));
    int n = snprintf(buf, 4096,
        "      <SampleInfo>\r\n"
        "        <Name>%s</Name>\r\n"
        "        <Location />\r\n"
        "        <Time>%04d-%02d-%02dT%02d:%02d:%02d</Time>\r\n"
        "        <Weight>1</Weight>\r\n"
        "        <Volume>1</Volume>\r\n"
        "        <Note />\r\n"
        "      </SampleInfo>\r\n"
        "      <DeviceConfigReference>\r\n"
        "        <Name>Atom Spectra</Name>\r\n"
        "        <Guid>00000000-0000-0000-0000-000000000000</Guid>\r\n"
        "      </DeviceConfigReference>\r\n"
        "      <StartTime>%04d-%02d-%02dT%02d:%02d:%02d</StartTime>\r\n"
        "      <EndTime>%04d-%02d-%02dT%02d:%02d:%02d</EndTime>\r\n"
        "      <PresetTime>0</PresetTime>\r\n",
        serial_esc,
        ts.tm_year+1900, ts.tm_mon+1, ts.tm_mday, ts.tm_hour, ts.tm_min, ts.tm_sec,
        ts.tm_year+1900, ts.tm_mon+1, ts.tm_mday, ts.tm_hour, ts.tm_min, ts.tm_sec,
        te.tm_year+1900, te.tm_mon+1, te.tm_mday, te.tm_hour, te.tm_min, te.tm_sec);
    httpd_resp_send_chunk(req, buf, n);

    n = snprintf(buf, 4096,
        "      <EnergySpectrum>\r\n"
        "        <NumberOfChannels>%d</NumberOfChannels>\r\n"
        "        <ChannelPitch>1</ChannelPitch>\r\n",
        SPECTRUM_CHANNELS);
    httpd_resp_send_chunk(req, buf, n);

    if (sp->calib_valid) {
        n = snprintf(buf, 4096,
            "        <EnergyCalibration>\r\n"
            "          <PolynomialOrder>%d</PolynomialOrder>\r\n"
            "          <Coefficients>\r\n",
            sp->calib_order);
        httpd_resp_send_chunk(req, buf, n);
        for (int i = 0; i <= sp->calib_order; i++) {
            n = snprintf(buf, 4096,
                "            <Coefficient>%.15G</Coefficient>\r\n",
                sp->calibration[i]);
            httpd_resp_send_chunk(req, buf, n);
        }
        httpd_resp_sendstr_chunk(req,
            "          </Coefficients>\r\n"
            "        </EnergyCalibration>\r\n");
    }

    float live_time = compute_live_time(sp);

    n = snprintf(buf, 4096,
        "        <ValidPulseCount>%" PRIu32 "</ValidPulseCount>\r\n"
        "        <TotalPulseCount>%" PRIu32 "</TotalPulseCount>\r\n"
        "        <MeasurementTime>%" PRIu32 "</MeasurementTime>\r\n"
        "        <LiveTime>%.1f</LiveTime>\r\n"
        "        <NumberOfSamples>1</NumberOfSamples>\r\n"
        "        <Spectrum>\r\n",
        sp->total_counts,
        sp->total_counts + sp->lost_impulses,
        sp->total_time_sec, live_time);
    httpd_resp_send_chunk(req, buf, n);

    for (int i = 0; i < SPECTRUM_CHANNELS; ) {
        int pos = 0;
        for (int j = 0; j < 80 && i < SPECTRUM_CHANNELS; j++, i++) {
            pos += snprintf(buf + pos, 4096 - pos,
                "          <DataPoint>%" PRIu32 "</DataPoint>\r\n", sp->bins[i]);
        }
        httpd_resp_send_chunk(req, buf, pos);
    }

    httpd_resp_sendstr_chunk(req,
        "        </Spectrum>\r\n"
        "      </EnergySpectrum>\r\n"
        "      <PulseCollection>\r\n"
        "        <Format>Base64 encoded binary</Format>\r\n"
        "        <Pulses />\r\n"
        "      </PulseCollection>\r\n"
        "    </ResultData>\r\n"
        "  </ResultDataList>\r\n"
        "</ResultDataFile>\r\n");

    httpd_resp_send_chunk(req, NULL, 0);
    free(buf);
    return ESP_OK;
}

static esp_err_t render_spectrum_csv(httpd_req_t *req, const spectrum_data_t *sp, const char *filename)
{
    char *buf = malloc(4096);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "text/csv");
    char named[48];
    web_build_export_name(named, sizeof(named), filename);   // #FW-42: префикс
    char disp[80];
    snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", named);
    httpd_resp_set_hdr(req, "Content-Disposition", disp);

    // #CSV-1 (2026-06-28): нативный формат Atom Spectra. Эталон оператора —
    // "spectrum (10).csv": одна строка-заголовок "Channel,Counts (TotalTime=<realtime>.0s)",
    // далее "канал,счёт" с канала 0 (не 1), без пробела после запятой, окончания CRLF,
    // 8192 канала. TotalTime = реальное (измерительное) время total_time_sec, формат %.1f.
    // Перекрывает прежний InterSpec/SpecUtils-заголовок #PR-2 по явному указанию оператора.
    // #FIELD-9 (A11): near-epoch → пометка в скобке TotalTime (одна строка-заголовок цела); только при рассинхроне
    time_t csv_ref = (sp->saved_at > 0) ? sp->saved_at : time(NULL);
    int n = time_is_synced(csv_ref)
        ? snprintf(buf, 4096, "Channel,Counts (TotalTime=%.1fs)\r\n", (double)sp->total_time_sec)
        : snprintf(buf, 4096, "Channel,Counts (TotalTime=%.1fs; TIME NOT SYNCHRONIZED)\r\n", (double)sp->total_time_sec);
    httpd_resp_send_chunk(req, buf, n);

    for (int i = 0; i < SPECTRUM_CHANNELS; ) {
        int pos = 0;
        for (int j = 0; j < 200 && i < SPECTRUM_CHANNELS; j++, i++) {
            pos += snprintf(buf + pos, 4096 - pos, "%d,%" PRIu32 "\r\n", i, sp->bins[i]);
        }
        httpd_resp_send_chunk(req, buf, pos);
    }

    httpd_resp_send_chunk(req, NULL, 0);
    free(buf);
    return ESP_OK;
}

// #EXP-1 (2026-06-28): нативный N42-2011 экспорт. Эталон оператора — "spectrum (10).N42"
// (родная выгрузка Atom Spectra через BecqMoni). Байт-точно: UTF-8 BOM, CRLF, БЕЗ финального
// перевода строки, отступы 2 пробела, блок RadInstrumentInformation дословно, CoefficientValues
// и ChannelData с замыкающим пробелом. Значения — живые из снимка спектра.
static esp_err_t render_spectrum_n42(httpd_req_t *req, const spectrum_data_t *sp, const char *filename)
{
    char *buf = malloc(4096);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/octet-stream");
    char named[48];
    web_build_export_name(named, sizeof(named), filename);   // #FW-42: префикс
    char disp[80];
    snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", named);
    httpd_resp_set_hdr(req, "Content-Disposition", disp);
    uint32_t r0 = esp_random(), r1 = esp_random(), r2 = esp_random(), r3 = esp_random();
    char uuid[40];
    snprintf(uuid, sizeof(uuid), "%08lx-%04lx-%04lx-%04lx-%04lx%08lx",
             (unsigned long)r0,
             (unsigned long)(r1 >> 16), (unsigned long)(r1 & 0xffff),
             (unsigned long)(r2 >> 16), (unsigned long)(r2 & 0xffff),
             (unsigned long)r3);
    int n = snprintf(buf, 4096,
        "\xEF\xBB\xBF"
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
        "<RadInstrumentData xmlns:xsd=\"http://www.w3.org/2001/XMLSchema\""
        " xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\""
        " n42DocUUID=\"%s\" xmlns=\"http://physics.nist.gov/N42/2011/N42\">\r\n",
        uuid);
    httpd_resp_send_chunk(req, buf, n);
    // #FIELD-9 (A11): near-epoch → Remark в начале RadInstrumentData (схема N42-2011 разрешает); только при рассинхроне
    {
        time_t et = (sp->saved_at > 0) ? sp->saved_at : time(NULL);
        if (!time_is_synced(et))
            httpd_resp_sendstr_chunk(req, "  <Remark>TIME NOT SYNCHRONIZED: board clock near-epoch, StartDateTime unreliable</Remark>\r\n");
    }
    httpd_resp_sendstr_chunk(req,
        "  <RadInstrumentInformation id=\"RadInstrument\">\r\n"
        "    <RadInstrumentManufacturerName>KB Radar</RadInstrumentManufacturerName>\r\n"
        "    <RadInstrumentModelName>ATOM Spectra</RadInstrumentModelName>\r\n"
        "    <RadInstrumentClassCode>Radionuclide Identifier</RadInstrumentClassCode>\r\n"
        "    <RadInstrumentVersion>\r\n"
        "      <RadInstrumentComponentName>Hardware</RadInstrumentComponentName>\r\n"
        "      <RadInstrumentComponentVersion />\r\n"
        "    </RadInstrumentVersion>\r\n"
        "    <RadInstrumentVersion>\r\n"
        "      <RadInstrumentComponentName>SoftwareName</RadInstrumentComponentName>\r\n"
        "      <RadInstrumentComponentVersion>BecqMoni</RadInstrumentComponentVersion>\r\n"
        "    </RadInstrumentVersion>\r\n"
        "    <RadInstrumentVersion>\r\n"
        "      <RadInstrumentComponentName>Software</RadInstrumentComponentName>\r\n"
        "      <RadInstrumentComponentVersion>2026.6.15.1</RadInstrumentComponentVersion>\r\n"
        "    </RadInstrumentVersion>\r\n"
        "  </RadInstrumentInformation>\r\n");
    if (sp->calib_valid) {
        httpd_resp_sendstr_chunk(req,
            "  <EnergyCalibration id=\"SpectrumCalibration-0\">\r\n"
            "    <CoefficientValues>");
        int cpos = 0;
        for (int i = 0; i <= sp->calib_order; i++)
            cpos += snprintf(buf + cpos, 4096 - cpos, "%.15G ", sp->calibration[i]);
        httpd_resp_send_chunk(req, buf, cpos);
        httpd_resp_sendstr_chunk(req,
            "</CoefficientValues>\r\n"
            "  </EnergyCalibration>\r\n");
    }
    time_t end_time = (sp->saved_at > 0) ? sp->saved_at : time(NULL);
    struct tm ts;
    time_t t_start = end_time - sp->total_time_sec;
    localtime_r(&t_start, &ts);
    n = snprintf(buf, 4096,
        "  <RadMeasurement id=\"SpectrumMeasurement-0\">\r\n"
        "    <MeasurementClassCode>Foreground</MeasurementClassCode>\r\n"
        "    <StartDateTime>%02d.%02d.%04d %02d:%02d:%02d</StartDateTime>\r\n"
        "    <RealTimeDuration>PT%" PRIu32 "S</RealTimeDuration>\r\n",
        ts.tm_mday, ts.tm_mon + 1, ts.tm_year + 1900, ts.tm_hour, ts.tm_min, ts.tm_sec,
        sp->total_time_sec);
    httpd_resp_send_chunk(req, buf, n);
    if (sp->calib_valid) {
        httpd_resp_sendstr_chunk(req,
            "    <Spectrum id=\"SpectrumData\" radDetectorInformationReference=\"Detector\""
            " energyCalibrationReference=\"SpectrumCalibration-0\">\r\n");
    } else {
        httpd_resp_sendstr_chunk(req,
            "    <Spectrum id=\"SpectrumData\" radDetectorInformationReference=\"Detector\">\r\n");
    }
    n = snprintf(buf, 4096,
        "      <LiveTimeDuration>PT%.1fS</LiveTimeDuration>\r\n"
        "      <ChannelData compressionCode=\"None\">",
        compute_live_time(sp));
    httpd_resp_send_chunk(req, buf, n);
    for (int i = 0; i < SPECTRUM_CHANNELS; ) {
        int pos = 0;
        for (int j = 0; j < 80 && i < SPECTRUM_CHANNELS; j++, i++)
            pos += snprintf(buf + pos, 4096 - pos, "%" PRIu32 " ", sp->bins[i]);
        httpd_resp_send_chunk(req, buf, pos);
    }
    n = snprintf(buf, 4096,
        "</ChannelData>\r\n"
        "    </Spectrum>\r\n"
        "    <GrossCounts id=\"GrossForeground\" radDetectorInformationReference=\"Detector\">\r\n"
        "      <TotalCounts>%" PRIu32 "</TotalCounts>\r\n"
        "    </GrossCounts>\r\n"
        "  </RadMeasurement>\r\n"
        "</RadInstrumentData>",
        sp->total_counts + sp->lost_impulses);
    httpd_resp_send_chunk(req, buf, n);
    httpd_resp_send_chunk(req, NULL, 0);
    free(buf);
    return ESP_OK;
}

// #EXP-2 (2026-06-28): экспорт ЛСРМ SpectraLine *.spe (бинарный вариант), как у SpectraVibe
// (gamma.io.lsrm_spe.write_lsrm_spe): CP-1251 заголовок KEY=VALUE\r\n, маркер "SPECTR=", затем
// бинарный блок счётов uint32 little-endian (по каналу, без разделителей). Значения ASCII.
static esp_err_t render_spectrum_spe(httpd_req_t *req, const spectrum_data_t *sp, const char *filename)
{
    char *buf = malloc(4096);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/octet-stream");
    char named[48];
    web_build_export_name(named, sizeof(named), filename);   // #FW-42: префикс
    char disp[80];
    snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", named);
    httpd_resp_set_hdr(req, "Content-Disposition", disp);
    time_t end_time = (sp->saved_at > 0) ? sp->saved_at : time(NULL);
    struct tm ts;
    time_t t_start = end_time - sp->total_time_sec;
    localtime_r(&t_start, &ts);
    int pos = snprintf(buf, 4096,
        "SHIFR=%s\r\n"
        "CONFIGNAME=Atom Spectra\r\n"
        "MEASBEGIN=%02d-%02d-%02d %02d:%02d:%02d.00\r\n"
        "TLIVE=%.2f\r\n"
        "TREAL=%.2f\r\n"
        "DETECTOR=Atom Spectra\r\n",
        sp->serial_number[0] ? sp->serial_number : "AtomSpectra",
        ts.tm_mday, ts.tm_mon + 1, (ts.tm_year + 1900) % 100,
        ts.tm_hour, ts.tm_min, ts.tm_sec,
        compute_live_time(sp), (double)sp->total_time_sec);
    // #FIELD-9 (A11): near-epoch → строка COMMENT (SPE игнорирует неизвестные ключи); только при рассинхроне
    if (!time_is_synced(end_time))
        pos += snprintf(buf + pos, 4096 - pos, "COMMENT=TIME NOT SYNCHRONIZED (board clock near-epoch)\r\n");
    if (sp->calib_valid) {
        double emax = 0.0;
        for (int i = sp->calib_order; i >= 0; i--)
            emax = emax * (SPECTRUM_CHANNELS - 1) + sp->calibration[i];
        if (emax < 0.0) emax = 0.0;
        pos += snprintf(buf + pos, 4096 - pos, "ENBOUNDS=0,%d\r\n", (int)(emax + 0.5));
        pos += snprintf(buf + pos, 4096 - pos, "ENERGY=%d", sp->calib_order);
        for (int i = 0; i < 7; i++) {
            double c = (i <= sp->calib_order) ? sp->calibration[i] : 0.0;
            pos += snprintf(buf + pos, 4096 - pos, ",%.10g", c);
        }
        pos += snprintf(buf + pos, 4096 - pos, "\r\n");
    }
    pos += snprintf(buf + pos, 4096 - pos, "SPECTRSIZE=%d\r\nSPECTR=", SPECTRUM_CHANNELS);
    httpd_resp_send_chunk(req, buf, pos);
    for (int i = 0; i < SPECTRUM_CHANNELS; ) {
        int cnt = SPECTRUM_CHANNELS - i;
        if (cnt > 1024) cnt = 1024;
        httpd_resp_send_chunk(req, (const char *)&sp->bins[i], cnt * 4);
        i += cnt;
    }
    httpd_resp_send_chunk(req, NULL, 0);
    free(buf);
    return ESP_OK;
}

static esp_err_t handle_export_xml(httpd_req_t *req)
{
    spectrum_data_t *sp = malloc(sizeof(*sp));
    if (!sp) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    if (!spectrum_get_snapshot(sp)) {
        free(sp);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "No spectrum data");
        return ESP_FAIL;
    }
    esp_err_t ret = render_spectrum_xml(req, sp, "spectrum.xml");
    free(sp);
    return ret;
}

static esp_err_t handle_export_spe(httpd_req_t *req)
{
    spectrum_data_t *sp = malloc(sizeof(*sp));
    if (!sp) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    if (!spectrum_get_snapshot(sp)) {
        free(sp);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "No spectrum data");
        return ESP_FAIL;
    }
    esp_err_t ret = render_spectrum_spe(req, sp, "spectrum.spe");
    free(sp);
    return ret;
}

static esp_err_t handle_export_n42(httpd_req_t *req)
{
    spectrum_data_t *sp = malloc(sizeof(*sp));
    if (!sp) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    if (!spectrum_get_snapshot(sp)) {
        free(sp);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "No spectrum data");
        return ESP_FAIL;
    }
    esp_err_t ret = render_spectrum_n42(req, sp, "spectrum.n42");
    free(sp);
    return ret;
}

static esp_err_t handle_export_csv(httpd_req_t *req)
{
    spectrum_data_t *sp = malloc(sizeof(*sp));
    if (!sp) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    if (!spectrum_get_snapshot(sp)) {
        free(sp);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "No spectrum data");
        return ESP_FAIL;
    }
    esp_err_t ret = render_spectrum_csv(req, sp, "spectrum.csv");
    free(sp);
    return ret;
}

static esp_err_t handle_saved_export_xml(httpd_req_t *req);
static esp_err_t handle_saved_export_csv(httpd_req_t *req);
static esp_err_t handle_saved_json(httpd_req_t *req);

static esp_err_t handle_saved_get(httpd_req_t *req)
{
    const char *p = req->uri + 11;
    while (*p >= '0' && *p <= '9') p++;
    if (strcmp(p, "/export.xml") == 0) return handle_saved_export_xml(req);
    if (strcmp(p, "/export.csv") == 0) return handle_saved_export_csv(req);
    if (strcmp(p, "/spectrum.json") == 0) return handle_saved_json(req);
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Unknown saved action");
    return ESP_FAIL;
}

static esp_err_t handle_saved_export_xml(httpd_req_t *req)
{
    int idx = parse_saved_index(req->uri);
    if (idx < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad index");
        return ESP_FAIL;
    }
    spectrum_data_t *sp = malloc(sizeof(spectrum_data_t));
    if (!sp) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    if (spectrum_load_from_flash(idx, sp) != 0) {
        free(sp);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Spectrum not found");
        return ESP_FAIL;
    }
    char fn[32];
    snprintf(fn, sizeof(fn), "spectrum_%04d.xml", idx);
    esp_err_t ret = render_spectrum_xml(req, sp, fn);
    free(sp);
    return ret;
}

static esp_err_t handle_saved_export_csv(httpd_req_t *req)
{
    int idx = parse_saved_index(req->uri);
    if (idx < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad index");
        return ESP_FAIL;
    }
    spectrum_data_t *sp = malloc(sizeof(spectrum_data_t));
    if (!sp) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    if (spectrum_load_from_flash(idx, sp) != 0) {
        free(sp);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Spectrum not found");
        return ESP_FAIL;
    }
    char fn[32];
    snprintf(fn, sizeof(fn), "spectrum_%04d.csv", idx);
    esp_err_t ret = render_spectrum_csv(req, sp, fn);
    free(sp);
    return ret;
}

static esp_err_t handle_saved_json(httpd_req_t *req)
{
    int idx = parse_saved_index(req->uri);
    if (idx < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad index");
        return ESP_FAIL;
    }
    spectrum_data_t *sp = malloc(sizeof(spectrum_data_t));
    if (!sp) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    if (spectrum_load_from_flash(idx, sp) != 0) {
        free(sp);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Spectrum not found");
        return ESP_FAIL;
    }
    esp_err_t ret = render_spectrum_json(req, sp);
    free(sp);
    return ret;
}

static esp_err_t handle_saved_delete(httpd_req_t *req)
{
    if (!csrf_check(req)) return ESP_FAIL;
    // #FS-3 (апстрим 1.2.31): удалять ТОЛЬКО по /api/saved/<цифры>/delete. Раньше любой
    // POST на /api/saved/<что угодно> удалял: atoi("abc") = 0 -> стиралась запись 0.
    const char *p = req->uri + 11;   // после "/api/saved/"
    const char *d = p;
    while (*d >= '0' && *d <= '9') d++;
    if (d == p || d - p > 6 || strcmp(d, "/delete") != 0) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Unknown saved action");
        return ESP_FAIL;
    }
    int idx = atoi(p);
    httpd_resp_set_type(req, "application/json");
    if (spectrum_delete_from_flash(idx) == 0)
        httpd_resp_sendstr(req, "{\"ok\":true}");
    else
        httpd_resp_sendstr(req, "{\"ok\":false}");
    return ESP_OK;
}

// #FS-1: рендер спектра из произвольного файла для web_files.c.
esp_err_t web_render_spectrum(httpd_req_t *req, const spectrum_data_t *sp,
                              const char *fmt, const char *filename)
{
    if (strcmp(fmt, "xml") == 0) return render_spectrum_xml(req, sp, filename);
    if (strcmp(fmt, "csv") == 0) return render_spectrum_csv(req, sp, filename);
    return render_spectrum_json(req, sp);
}

static esp_err_t handle_device(httpd_req_t *req)
{
    const device_info_t *di = spectrum_get_device_info();
    // Снимок под локом: serial/calibration пишутся CDC-задачей конкурентно.
    spectrum_data_t *sp = malloc(sizeof(*sp));
    bool have_sp = sp && spectrum_get_snapshot(sp);
    cJSON *root = cJSON_CreateObject();
    if (!root) {  // P2-7: при OOM cJSON_Add* разыменует NULL
        free(sp);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }
    cJSON_AddBoolToObject(root, "valid", di->valid);
    if (di->valid) {
        cJSON_AddNumberToObject(root, "dev", di->dev);
        cJSON_AddNumberToObject(root, "version", di->version);
        cJSON_AddNumberToObject(root, "rise", di->rise);
        cJSON_AddNumberToObject(root, "fall", di->fall);
        cJSON_AddNumberToObject(root, "noise", di->noise);
        cJSON_AddNumberToObject(root, "freq", di->freq);
        cJSON_AddNumberToObject(root, "max_integral", di->max_integral);
        cJSON_AddNumberToObject(root, "hyst", di->hyst);
        cJSON_AddNumberToObject(root, "mode", di->mode);
        cJSON_AddNumberToObject(root, "step", di->step);
        cJSON_AddNumberToObject(root, "time", di->time_sec);
        cJSON_AddNumberToObject(root, "pot", di->pot);
        cJSON_AddNumberToObject(root, "t1", di->t1);
        cJSON_AddNumberToObject(root, "t2", di->t2);
        cJSON_AddNumberToObject(root, "t3", di->t3);
        cJSON_AddBoolToObject(root, "tc_on", di->tc_on);
        cJSON_AddNumberToObject(root, "tp", di->tp);
    }
    if (have_sp && sp->serial_number[0])
        cJSON_AddStringToObject(root, "serial", sp->serial_number);
    if (have_sp && sp->calib_valid) {
        cJSON *cal = cJSON_CreateArray();
        for (int i = 0; i <= sp->calib_order; i++)
            cJSON_AddItemToArray(cal, cJSON_CreateNumber(sp->calibration[i]));
        cJSON_AddItemToObject(root, "calibration", cal);
        cJSON_AddNumberToObject(root, "calib_order", sp->calib_order);
    }
    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    free(json);
    cJSON_Delete(root);
    free(sp);
    return ESP_OK;
}

// #PWR-6: ручная установка предела входного тока IINLIM (REG00) поверх
// автоопределения источника питания BQ25895. Тело: {"ma": <100..2000>}.
static esp_err_t handle_battery_set_iinlim(httpd_req_t *req)
{
    if (!csrf_check(req)) return ESP_FAIL;
    char body[64] = {0};
    int recv_len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (recv_len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    body[recv_len] = '\0';
    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad JSON");
        return ESP_FAIL;
    }
    cJSON *it = cJSON_GetObjectItem(root, "ma");
    if (!it || !cJSON_IsNumber(it)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing 'ma'");
        return ESP_FAIL;
    }
    int ma = it->valueint;
    cJSON_Delete(root);

    int applied = (ma >= 100 && ma <= 2000) ? battery_set_iinlim((uint16_t)ma) : -1;

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", applied >= 0);
    cJSON_AddNumberToObject(resp, "iinlim_ma", applied >= 0 ? applied : 0);
    char *json = cJSON_PrintUnformatted(resp);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    free(json);
    cJSON_Delete(resp);
    return ESP_OK;
}

static esp_err_t handle_reboot_device(httpd_req_t *req)
{
    if (!csrf_check(req)) return ESP_FAIL;
    uint8_t pkt_buf[64];
    shproto_struct pkt;
    shproto_init(&pkt, pkt_buf, sizeof(pkt_buf));
    shproto_packet_start(&pkt, CMD_REBOOT);
    shproto_packet_complete(&pkt);
    usb_host_cdc_acq_intent_device_reboot();   // #ACQ-1: набор шёл — сторож вернёт его после перезагрузки
    int ret = usb_host_cdc_send(pkt.data, pkt.len);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, ret == 0 ? "{\"ok\":true}" : "{\"ok\":false}");
    return ESP_OK;
}

static esp_err_t handle_system(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {  // P2-7: при OOM cJSON_Add* разыменует NULL
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }
    // #FW-28: версия прошивки ESP32 из esp_app_get_description()->version
    // (= PROJECT_VER = содержимое version.txt в корне проекта). Клиент сверяет
    // с latest release VibeEngineering-LLC/atomspectra-waterfall-esp32 через GitHub API.
    const esp_app_desc_t *app_desc = esp_app_get_description();
    cJSON_AddStringToObject(root, "fw_version", app_desc ? app_desc->version : "?");
    cJSON_AddStringToObject(root, "app_part", ota_update_running_label());   // #OTA-1
    {   // #OTA-1: версия у всех наших сборок одна — различать по хэшу образа
        // (дата из esp_app_desc берётся из закэшированного объекта и не меняется).
        char sha[9] = "";
        esp_app_get_elf_sha256(sha, sizeof sha);
        cJSON_AddStringToObject(root, "fw_built", sha);
    }
    cJSON_AddBoolToObject(root, "ota_supported", ota_update_supported());
    cJSON_AddNumberToObject(root, "free_heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "min_free_heap", esp_get_minimum_free_heap_size());
    // #SD-2: внутренняя RAM отдельно от PSRAM — главный дефицит платы (#TFT-17).
    // free_heap выше включает 8 МБ PSRAM и про неё ничего не говорит.
    cJSON_AddNumberToObject(root, "esp_cpu0",     sys_load_core(0));   // #SYS-1: % за последнюю секунду
    cJSON_AddNumberToObject(root, "esp_cpu1",     sys_load_core(1));
    cJSON_AddNumberToObject(root, "int_free",     heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(root, "int_largest",  heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(root, "int_min_free", heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(root, "dma_free",     heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(root, "dma_largest",  heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(root, "uptime_sec", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddBoolToObject(root, "usb_connected", usb_host_cdc_is_connected());
    cJSON_AddBoolToObject(root, "wifi_connected", wifi_is_connected());
    cJSON_AddBoolToObject(root, "tcp_client", tcp_bridge_client_connected());
    size_t total = 0, used = 0;
    esp_littlefs_info("storage", &total, &used);
    cJSON_AddNumberToObject(root, "flash_total", (double)total);
    cJSON_AddNumberToObject(root, "flash_used", (double)used);
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        cJSON_AddNumberToObject(root, "rssi", ap.rssi);
        cJSON_AddStringToObject(root, "ssid", (char *)ap.ssid);
    }
    // #ETH-38: реальный IP Wi-Fi-интерфейса (не location.hostname, который может
    // быть Ethernet-адресом, если заходить по кабелю)
    {
        esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        esp_netif_ip_info_t ip_info;
        if (sta_netif && esp_netif_get_ip_info(sta_netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
            char wifi_ip_buf[16];
            esp_ip4addr_ntoa(&ip_info.ip, wifi_ip_buf, sizeof(wifi_ip_buf));
            cJSON_AddStringToObject(root, "wifi_ip", wifi_ip_buf);
        }
    }
    // #FIELD-6: сетевой режим + статус полевого AP + источник времени (для system.html).
    switch (wifi_manager_mode()) {
        case NET_MODE_FIELD_AP: cJSON_AddStringToObject(root, "net_mode", "field_ap"); break;
        case NET_MODE_SETUP:    cJSON_AddStringToObject(root, "net_mode", "setup");    break;
        default:                cJSON_AddStringToObject(root, "net_mode", "sta");      break;
    }
    cJSON_AddStringToObject(root, "ap_ssid",        wifi_manager_ap_ssid());
    cJSON_AddNumberToObject(root, "ap_clients",     wifi_manager_ap_clients());
    cJSON_AddBoolToObject(root,   "ap_pass_default", wifi_manager_ap_pass_is_default());
    cJSON_AddBoolToObject(root,   "ap_forced",      wifi_manager_ap_forced());
    cJSON_AddStringToObject(root, "time_source",    net_time_source_str());
    cJSON_AddBoolToObject(root,   "rtc_present",    rtc_ds3231_present());     // #RTC-1
    cJSON_AddBoolToObject(root,   "rtc_valid",      rtc_ds3231_time_valid());
    {   // #SD-1: слот SD модуля дисплея
        uint64_t sd_total = 0, sd_free = 0;
        bool sd = sd_card_present() && sd_card_space(&sd_total, &sd_free);
        cJSON_AddBoolToObject(root, "sd_present", sd);
        cJSON_AddBoolToObject(root, "saved_on_sd", spectrum_saved_on_sd());   // #SD-4
        if (sd) {
            cJSON_AddNumberToObject(root, "sd_total", (double)sd_total);
            cJSON_AddNumberToObject(root, "sd_free",  (double)sd_free);
            cJSON_AddStringToObject(root, "sd_name",  sd_card_name());
        }
    }
    cJSON_AddBoolToObject(root,   "sntp_synced",    net_time_sntp_synced());
    // #PWR-1/#PWR-2: значок батареи (CG_Charger_BQ-2v0/BQ25895) в шапке + полная
    // телеметрия для панели на странице «Система» — присутствует только если
    // плата физически подключена по I2C (battery.present).
    battery_status_t bs;
    battery_get_status(&bs);
    cJSON_AddBoolToObject(root,   "batt_present",      bs.present);
    if (bs.present) {
        cJSON_AddBoolToObject(root,   "batt_vbus",         bs.vbus_present);
        cJSON_AddBoolToObject(root,   "batt_power_good",   bs.power_good);
        cJSON_AddNumberToObject(root, "batt_vbus_stat",    bs.vbus_stat);
        cJSON_AddBoolToObject(root,   "batt_charging",     bs.charging);
        cJSON_AddBoolToObject(root,   "batt_chg_done",     bs.chg_done);
        cJSON_AddNumberToObject(root, "batt_voltage",      bs.voltage);
        cJSON_AddNumberToObject(root, "batt_percent",      bs.percent);
        cJSON_AddNumberToObject(root, "batt_vbus_voltage", bs.vbus_voltage);
        cJSON_AddNumberToObject(root, "batt_vsys_voltage", bs.vsys_voltage);
        cJSON_AddNumberToObject(root, "batt_ntc_pct",      bs.ntc_pct);
        cJSON_AddBoolToObject(root,   "batt_fault_wd",     bs.fault_watchdog);
        cJSON_AddBoolToObject(root,   "batt_fault_boost",  bs.fault_boost);
        cJSON_AddNumberToObject(root, "batt_fault_chrg",   bs.fault_chrg);
        cJSON_AddBoolToObject(root,   "batt_fault_bat",    bs.fault_bat);
        cJSON_AddNumberToObject(root, "batt_fault_ntc",    bs.fault_ntc);
        {   // #PWR-9: сырые регистры BQ25895 0x00..0x14 и счётчики неисправностей
            char hex[21 * 2 + 1];
            for (int i = 0; i < 21; i++) snprintf(hex + i * 2, 3, "%02x", bs.regs[i]);
            cJSON_AddStringToObject(root, "batt_regs", hex);
            cJSON_AddNumberToObject(root, "batt_boost_faults", bs.boost_faults);
            cJSON_AddNumberToObject(root, "batt_chrg_faults",  bs.chrg_faults);
            cJSON_AddNumberToObject(root, "batt_wd_faults",    bs.wd_faults);
        }
        cJSON_AddNumberToObject(root, "batt_iinlim_ma",    bs.iinlim_ma);
    }
    // #TFT-21: дисплей и калибровка его тача (кнопка на странице «Система»).
    cJSON_AddBoolToObject(root, "tft", tft_display_present());
    cJSON_AddBoolToObject(root, "tft_cal", tft_display_touch_calibrated());
    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    free(json);
    cJSON_Delete(root);
    return ESP_OK;
}

// #FW-22: глубокая USB-Host диагностика (read-only JSON снапшота usb_diag_snapshot_t).
// Инструментация для #FW-43 (hot-plug re-init): enum_cb_count/open_attempts/cdc_open/
// last_open_errno/rx_cb_count/pkt_hist позволяют увидеть, где рвётся реконнект прибора.
static esp_err_t handle_usb_diag(httpd_req_t *req)
{
    usb_diag_snapshot_t d;
    usb_host_cdc_diag_snapshot(&d);
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }
    cJSON_AddBoolToObject(root,   "host_installed",        d.host_installed);
    cJSON_AddBoolToObject(root,   "cdc_driver_installed",  d.cdc_driver_installed);
    cJSON_AddNumberToObject(root, "last_host_event_flags", d.last_host_event_flags);
    cJSON_AddNumberToObject(root, "host_event_ts_ms",      d.host_event_ts_ms);
    cJSON_AddNumberToObject(root, "enum_cb_count",         d.enum_cb_count);
    cJSON_AddNumberToObject(root, "last_seen_vid",         d.last_seen_vid);
    cJSON_AddNumberToObject(root, "last_seen_pid",         d.last_seen_pid);
    cJSON_AddNumberToObject(root, "last_seen_class",       d.last_seen_class);
    cJSON_AddNumberToObject(root, "last_seen_subclass",    d.last_seen_subclass);
    cJSON_AddNumberToObject(root, "last_seen_ts_ms",       d.last_seen_ts_ms);
    cJSON_AddNumberToObject(root, "bus_devs_now",          d.bus_devs_now);
    cJSON_AddNumberToObject(root, "open_attempts",         d.open_attempts);
    cJSON_AddNumberToObject(root, "last_open_errno",       d.last_open_errno);
    cJSON_AddStringToObject(root, "last_open_errstr",      esp_err_to_name(d.last_open_errno));
    cJSON_AddNumberToObject(root, "last_open_ts_ms",       d.last_open_ts_ms);
    cJSON_AddBoolToObject(root,   "cdc_open",              d.cdc_open);
    cJSON_AddNumberToObject(root, "ftdi_step_ok_mask",     d.ftdi_step_ok_mask);
    cJSON_AddNumberToObject(root, "ftdi_last_errno",       d.ftdi_last_errno);
    cJSON_AddNumberToObject(root, "tx_packets",            d.tx_packets);
    cJSON_AddNumberToObject(root, "tx_bytes",              d.tx_bytes);
    cJSON_AddNumberToObject(root, "last_tx_ts_ms",         d.last_tx_ts_ms);
    cJSON_AddNumberToObject(root, "last_tx_errno",         d.last_tx_errno);
    cJSON_AddStringToObject(root, "last_tx_cmd",           d.last_tx_cmd);
    cJSON_AddNumberToObject(root, "rx_cb_count",           d.rx_cb_count);
    cJSON_AddNumberToObject(root, "rx_bytes",              d.rx_bytes);
    cJSON_AddNumberToObject(root, "rx_last_ts_ms",         d.rx_last_ts_ms);
    cJSON_AddNumberToObject(root, "rx_last_len",           d.rx_last_len);
    cJSON_AddStringToObject(root, "rx_last_first16_hex",   d.rx_last_first16_hex);
    cJSON_AddNumberToObject(root, "line_status_errors",    d.line_status_errors);
    cJSON_AddNumberToObject(root, "pkt_hist",              d.pkt_hist);
    cJSON_AddNumberToObject(root, "pkt_text",              d.pkt_text);
    cJSON_AddNumberToObject(root, "pkt_stat",              d.pkt_stat);
    cJSON_AddNumberToObject(root, "pkt_osc",               d.pkt_osc);
    cJSON_AddNumberToObject(root, "pkt_unknown",           d.pkt_unknown);
    cJSON_AddNumberToObject(root, "drv_task_alive_ts_ms",  d.drv_task_alive_ts_ms);
    cJSON_AddNumberToObject(root, "conn_task_alive_ts_ms", d.conn_task_alive_ts_ms);
    cJSON_AddNumberToObject(root, "dma_free_largest",      d.dma_free_largest);
    cJSON_AddNumberToObject(root, "dma_free_total",        d.dma_free_total);
    cJSON_AddNumberToObject(root, "uptime_ms",             d.uptime_ms);
    // #FW-51: закрытия соединения; #ACQ-1: сторож набора; #SB-1: база спектра
    cJSON_AddNumberToObject(root, "cdc_error_count",       d.cdc_error_count);
    cJSON_AddNumberToObject(root, "rx_watchdog_trips",     d.rx_watchdog_trips);
    cJSON_AddNumberToObject(root, "bus_empty_trips",       d.bus_empty_trips);
    cJSON_AddNumberToObject(root, "last_fault_reason",     d.last_fault_reason);
    cJSON_AddNumberToObject(root, "last_fault_ts_ms",      d.last_fault_ts_ms);
    cJSON_AddNumberToObject(root, "acq_intent",            d.acq_intent);
    cJSON_AddNumberToObject(root, "acq_resend_count",      d.acq_resend_count);
    cJSON_AddNumberToObject(root, "last_acq_resend_ts_ms", d.last_acq_resend_ts_ms);
    cJSON_AddNumberToObject(root, "last_hist_ts_ms",       d.last_hist_ts_ms);
    cJSON_AddNumberToObject(root, "last_shproto_ts_ms",    d.last_shproto_ts_ms);
    cJSON_AddBoolToObject(root,   "connected",             usb_host_cdc_is_connected());
    {
        uint32_t bc = 0, bt = 0, bf = 0;
        spectrum_get_base_info(&bc, &bt, &bf);
        cJSON_AddNumberToObject(root, "base_counts", bc);
        cJSON_AddNumberToObject(root, "base_time",   bt);
        cJSON_AddNumberToObject(root, "base_folds",  bf);
    }
    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    free(json);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t handle_set_calibration(httpd_req_t *req)
{
    if (!csrf_check(req)) return ESP_FAIL;
    char body[512] = {0};
    int recv_len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (recv_len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    body[recv_len] = '\0';
    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }
    cJSON *arr = cJSON_GetObjectItem(root, "coeffs");
    if (!cJSON_IsArray(arr)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing coeffs");
        return ESP_FAIL;
    }
    int n = cJSON_GetArraySize(arr);
    if (n < 1) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty coeffs");
        return ESP_FAIL;
    }
    if (n > CALIB_COEFFS) n = CALIB_COEFFS;
    double coeffs[CALIB_COEFFS] = {0};
    for (int i = 0; i < n; i++) {
        cJSON *item = cJSON_GetArrayItem(arr, i);
        // P2-5: cJSON парсит "NaN"/"Infinity" в valuedouble как есть. NaN/Inf
        // в коэффициентах калибровки потом дают мусорные энергии и могут
        // распространиться в N42/CSV экспорт — отбраковываем на входе.
        if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble)) {
            cJSON_Delete(root);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad coeff");
            return ESP_FAIL;
        }
        coeffs[i] = item->valuedouble;
    }
    spectrum_set_calibration(coeffs, n - 1);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

// ---- #DEV-6: бэкап/восстановление настроек прибора (вкладка «Сервис») ----
//
// Формат — сырой текст ответов -inf + -tc_pot?, байт-в-байт совместимый с
// автосейвом MCA.exe (references/atomspectra_settings_backup_2026-07-01.md).
// Ниже — общий key/value-парсер этого текста для восстановления.

static inline bool is_kv_boundary(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

// Ищет "KEY" на границе слова (не внутри другого токена — напр. не путает
// "POT" с "POT2", "TC" с "TCpot") и возвращает указатель на начало значения
// (сразу после разделяющего пробела), либо NULL.
// Граница слова — пробел/таб/CR/LF (не только пробел): бэкап — две строки
// через "\r\n" ("Tcpot [...]" начинается сразу после перевода строки), якорь
// только на ' ' пропускал бы этот ключ (#DEV-7, 2026-07-01).
static const char *find_kv(const char *text, const char *key)
{
    size_t klen = strlen(key);
    const char *p = text;
    while ((p = strstr(p, key)) != NULL) {
        bool left_ok  = (p == text) || is_kv_boundary(p[-1]);
        bool right_ok = (p[klen] == '\0') || is_kv_boundary(p[klen]);
        if (left_ok && right_ok)
            return p + klen + (is_kv_boundary(p[klen]) && p[klen] != '\0' ? 1 : 0);
        p += 1;
    }
    return NULL;
}

static bool kv_get_int(const char *text, const char *key, long *out)
{
    const char *v = find_kv(text, key);
    if (!v) return false;
    char *end;
    long val = strtol(v, &end, 10);
    if (end == v) return false;
    *out = val;
    return true;
}

static bool kv_get_double(const char *text, const char *key, double *out)
{
    const char *v = find_kv(text, key);
    if (!v) return false;
    char *end;
    double val = strtod(v, &end);
    if (end == v) return false;
    *out = val;
    return true;
}

// Копирует значение-токен до пробела (для "ON"/"OFF" и т.п.)
static bool kv_get_token(const char *text, const char *key, char *buf, size_t bufsz)
{
    const char *v = find_kv(text, key);
    if (!v) return false;
    size_t n = 0;
    while (v[n] && v[n] != ' ' && n < bufsz - 1) { buf[n] = v[n]; n++; }
    buf[n] = '\0';
    return n > 0;
}

// Парсит "KEY [n1 n2 ...]" -> массив long (до max элементов). Возвращает
// число элементов (0 для "[]"), либо -1 если ключ/скобка не найдены.
static int kv_get_array(const char *text, const char *key, long *out, int max)
{
    const char *v = find_kv(text, key);
    if (!v) return -1;
    while (*v == ' ') v++;
    if (*v != '[') return -1;
    v++;
    int n = 0;
    while (*v && *v != ']') {
        char *end;
        long val = strtol(v, &end, 10);
        if (end == v) break;
        if (n < max) out[n++] = val;
        v = end;
        while (*v == ' ') v++;
    }
    return n;
}

// GET /api/settings/backup — read-only относительно физического состояния
// прибора: шлёт -inf/-tc_pot? и отдаёт сырые ответы (см. spectrum_get_info_raw/
// spectrum_get_tcpot_raw, atomspectra.h). CSRF не требуется (не мутирует прибор).
static esp_err_t handle_settings_backup(httpd_req_t *req)
{
    if (!usb_host_cdc_is_connected()) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Device not connected");
        return ESP_FAIL;
    }

    char line[700];
    uint32_t seq_before = 0, seq_after = 0;

    spectrum_get_info_raw(line, sizeof(line), &seq_before);
    usb_host_send_text_command("-inf");
    for (int waited = 0; waited < 2000; waited += 50) {
        vTaskDelay(pdMS_TO_TICKS(50));
        spectrum_get_info_raw(line, sizeof(line), &seq_after);
        if (seq_after != seq_before) break;
    }
    if (seq_after == seq_before) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No response to -inf (device busy/offline?)");
        return ESP_FAIL;
    }
    // #FW-17: static + 2048Б — под полную -inf с 99-элементным PileUp[] (см.
    // s_info_raw в spectrum.c). НЕ на стеке: httpd-воркер сериализует запросы
    // (async off), реентранси нет; 2КБ на стеке 8192 — лишний риск.
    static char info_line[2048];
    spectrum_get_info_raw(info_line, sizeof(info_line), NULL);

    spectrum_get_tcpot_raw(line, sizeof(line), &seq_before);
    usb_host_send_text_command("-tc_pot?");
    seq_after = seq_before;
    for (int waited = 0; waited < 2000; waited += 50) {
        vTaskDelay(pdMS_TO_TICKS(50));
        spectrum_get_tcpot_raw(line, sizeof(line), &seq_after);
        if (seq_after != seq_before) break;
    }
    if (seq_after == seq_before) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No response to -tc_pot? (older firmware?)");
        return ESP_FAIL;
    }
    char tcpot_line[700];
    spectrum_get_tcpot_raw(tcpot_line, sizeof(tcpot_line), NULL);

    char bkp_name[48], bkp_disp[80];                         // #FW-42: префикс
    web_build_export_name(bkp_name, sizeof(bkp_name), "atomspectra_backup.txt");
    snprintf(bkp_disp, sizeof(bkp_disp), "attachment; filename=\"%s\"", bkp_name);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Content-Disposition", bkp_disp);
    httpd_resp_sendstr_chunk(req, info_line);
    httpd_resp_sendstr_chunk(req, "\r\n");
    httpd_resp_sendstr_chunk(req, tcpot_line);
    httpd_resp_sendstr_chunk(req, "\r\n");
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static void send_cmd_delayed(const char *cmd)
{
    usb_host_send_text_command(cmd);
    // Пауза между командами настройки — не заваливать вход прибора пачкой
    // текстовых команд подряд (см. -t/-t_pot до 20 точек каждая).
    vTaskDelay(pdMS_TO_TICKS(30));
}

// POST /api/settings/restore — body = текст бэкапа (тот же формат, что
// отдаёт handle_settings_backup). Разбирает поля и шлёт прибору
// последовательность команд настройки по PROTOCOL.md.
// ⚠ МЕНЯЕТ физическую конфигурацию прибора (RISE/FALL/NOISE/MAX/HYST/F/POT/
// POT2/термокомпенсацию/pile-up). CSRF обязателен. Первый реальный тест на
// железе — только по явному отдельному «да» оператора (см. CLAUDE.md #DEV-6,
// прецедент #DEV-5 — оператор ранее сказал «не сейчас» именно для -frq).
static esp_err_t handle_settings_restore(httpd_req_t *req)
{
    if (!csrf_check(req)) return ESP_FAIL;
    if (!usb_host_cdc_is_connected()) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Device not connected");
        return ESP_FAIL;
    }

    char *body = malloc(1600);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    int total = 0, r;
    while (total < 1599 && (r = httpd_req_recv(req, body + total, 1599 - total)) > 0)
        total += r;
    if (total <= 0) {
        free(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    body[total] = '\0';

    if (!strstr(body, "VERSION ") || !strstr(body, "RISE ")) {
        free(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Not an AtomSpectra backup (VERSION/RISE missing)");
        return ESP_FAIL;
    }

    char cmd[160];
    long v;
    double d;
    char tok[16];
    long arr[40];
    int n;

    if (kv_get_int(body, "RISE", &v))  { snprintf(cmd, sizeof(cmd), "-ris %ld", v);  send_cmd_delayed(cmd); }
    if (kv_get_int(body, "FALL", &v))  { snprintf(cmd, sizeof(cmd), "-fall %ld", v); send_cmd_delayed(cmd); }
    if (kv_get_int(body, "NOISE", &v)) { snprintf(cmd, sizeof(cmd), "-nos %ld", v);  send_cmd_delayed(cmd); }
    if (kv_get_int(body, "HYST", &v))  { snprintf(cmd, sizeof(cmd), "-hyst %ld", v); send_cmd_delayed(cmd); }
    if (kv_get_int(body, "MAX", &v))   { snprintf(cmd, sizeof(cmd), "-max %ld", v);  send_cmd_delayed(cmd); }
    if (kv_get_int(body, "STEP", &v))  { snprintf(cmd, sizeof(cmd), "-step %ld", v); send_cmd_delayed(cmd); }
    if (kv_get_double(body, "F", &d) && d > 0) {
        snprintf(cmd, sizeof(cmd), "-frq %ld", (long)(d + 0.5));
        send_cmd_delayed(cmd);
    }
    // POT/POT2 = значения U (HV) и V (baseline) соответственно (см.
    // atomspectra_protocol_official_spec.txt:141). Синтаксис без пробела.
    if (kv_get_int(body, "POT", &v))  { snprintf(cmd, sizeof(cmd), "-U%ld", v); send_cmd_delayed(cmd); }
    if (kv_get_int(body, "POT2", &v)) { snprintf(cmd, sizeof(cmd), "-V%ld", v); send_cmd_delayed(cmd); }

    if (kv_get_int(body, "Prise", &v)) { snprintf(cmd, sizeof(cmd), "-prise %ld", v); send_cmd_delayed(cmd); }
    if (kv_get_int(body, "Srise", &v)) { snprintf(cmd, sizeof(cmd), "-srise %ld", v); send_cmd_delayed(cmd); }
    if (kv_get_int(body, "Pfall", &v)) { snprintf(cmd, sizeof(cmd), "-pfall %ld", v); send_cmd_delayed(cmd); }
    if (kv_get_int(body, "Sfall", &v)) { snprintf(cmd, sizeof(cmd), "-sfall %ld", v); send_cmd_delayed(cmd); }

    if (kv_get_int(body, "PileUpThr", &v)) { snprintf(cmd, sizeof(cmd), "-pthr %ld", v); send_cmd_delayed(cmd); }
    n = kv_get_array(body, "PileUp", arr, 100);
    if (n > 0) {
        int off = snprintf(cmd, sizeof(cmd), "-pileup");
        for (int i = 0; i < n && off < (int)sizeof(cmd) - 8; i++)
            off += snprintf(cmd + off, sizeof(cmd) - off, " %ld", arr[i]);
        send_cmd_delayed(cmd);
    }

    // Tco[] — до 20 точек (temp, max_integral) термокомпенсации макс. интеграла.
    n = kv_get_array(body, "Tco", arr, 40);
    if (n >= 2) {
        send_cmd_delayed("-tclear");
        int npoints = n / 2;
        if (npoints > 20) npoints = 20;
        for (int i = 0; i < npoints; i++) {
            snprintf(cmd, sizeof(cmd), "-t %d %ld %ld", i + 1, arr[2 * i], arr[2 * i + 1]);
            send_cmd_delayed(cmd);
        }
    }
    if (kv_get_int(body, "TP", &v)) { snprintf(cmd, sizeof(cmd), "-tp %ld", v); send_cmd_delayed(cmd); }
    if (kv_get_token(body, "TC", tok, sizeof(tok))) {
        snprintf(cmd, sizeof(cmd), "-tc %s", strcasecmp(tok, "ON") == 0 ? "on" : "off");
        send_cmd_delayed(cmd);
    }

    // Вторая строка бэкапа: "Tcpot [...]" (регистр отличается от "TCpot" выше,
    // strstr — регистрозависим, поэтому find_kv не путает две строки) —
    // до 20 точек компенсации baseline (#DOC-3/BUG-AS-08).
    n = kv_get_array(body, "Tcpot", arr, 40);
    if (n >= 2) {
        int npoints = n / 2;
        if (npoints > 20) npoints = 20;
        for (int i = 0; i < npoints; i++) {
            snprintf(cmd, sizeof(cmd), "-t_pot %d %ld %ld", i + 1, arr[2 * i], arr[2 * i + 1]);
            send_cmd_delayed(cmd);
        }
    }
    if (kv_get_token(body, "TCpot", tok, sizeof(tok))) {
        snprintf(cmd, sizeof(cmd), "-tc_pot %s", strcasecmp(tok, "ON") == 0 ? "on" : "off");
        send_cmd_delayed(cmd);
    }

    free(body);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t handle_reboot_esp(httpd_req_t *req)
{
    if (!csrf_check(req)) return ESP_FAIL;
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    spectrogram_prepare_reboot();   // #OTA-3: не терять открытый сегмент водопада
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

// #TFT-21: запуск калибровки тача. Сама процедура идёт на экране (крестики),
// здесь — только постановка команды в очередь задачи экрана.
static esp_err_t handle_tft_calibrate(httpd_req_t *req)
{
    if (!csrf_check(req)) return ESP_FAIL;
    bool ok = tft_display_start_touch_cal();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, ok ? "{\"ok\":true}" : "{\"ok\":false}");
    return ESP_OK;
}

static esp_err_t handle_healthcheck(httpd_req_t *req)
{
    bool usb = usb_host_cdc_is_connected();
    bool wifi = wifi_is_connected();
    int64_t uptime_us = esp_timer_get_time();
    const spectrum_data_t *sp = spectrum_get_current();

    httpd_resp_set_type(req, "application/json");
    if (usb && wifi) {
        httpd_resp_set_status(req, "200 OK");
    } else {
        httpd_resp_set_status(req, "503 Service Unavailable");
    }

    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"status\":\"%s\",\"analyzer_connected\":%s,\"wifi_connected\":%s,"
        "\"uptime_sec\":%.1f,\"spectrum_valid\":%s}",
        (usb && wifi) ? "ok" : "degraded",
        usb ? "true" : "false",
        wifi ? "true" : "false",
        (double)uptime_us / 1000000.0,
        sp->valid ? "true" : "false");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

static esp_err_t handle_wifi_reset(httpd_req_t *req)
{
    if (!csrf_check(req)) return ESP_FAIL;
    nvs_handle_t nvs;
    if (nvs_open("wifi", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_erase_all(nvs);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    httpd_resp_sendstr(req, "{\"ok\":true}");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

// ===================== #FIELD-4/5/7: полевой AP — время и пароль =====================

// Нижняя граница вменяемого epoch для /api/time (2023-11-14). WF_SANE_EPOCH из
// spectrogram.c не экспортирован — дублируем как локальную санитарную границу.
#define FIELD_MIN_EPOCH_S  1700000000LL
#define FIELD_MAX_EPOCH_S  4102444800LL   // 2100-01-01
#define FIELD_AP_URL       "http://192.168.4.1/"

// #FIELD-5 (A2): POST /api/time — установка времени платы от браузера телефона.
// Тело JSON: {"epoch_ms": <ms since 1970 UTC>, "manual": <bool, опц.>}.
// В Outdoor (полевой AP) SNTP не поднят → sntp_synced=false → время идёт от телефона.
// Guard-логика (net_time_should_accept) — в net_time.c (host-тестируема):
//   sntp_synced → отклонить (война источников; T1); manual → принять; auto → |Δ|>5 с.
// При приёме — settimeofday + пометка источника + spectrogram_time_synced() (пересчёт
// started_at ongoing-записи из монотонного uptime, тот же путь что SNTP-cb #FW-23).
static esp_err_t handle_time_set(httpd_req_t *req)
{
    if (!csrf_check(req)) return ESP_FAIL;

    char body[128];
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no body"); return ESP_FAIL; }
    body[len] = '\0';

    cJSON *root = cJSON_Parse(body);
    if (!root) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json"); return ESP_FAIL; }
    cJSON *jms  = cJSON_GetObjectItem(root, "epoch_ms");
    cJSON *jman = cJSON_GetObjectItem(root, "manual");
    cJSON *jtz  = cJSON_GetObjectItem(root, "tz_min");
    // #MON-2: пояс отображения браузера — для экрана платы. Не зависит от того,
    // примут ли само время (при SNTP его отклонят, а пояс всё равно нужен).
    if (cJSON_IsNumber(jtz) && jtz->valuedouble >= BOOT_TZ_MIN_LO && jtz->valuedouble <= BOOT_TZ_MIN_HI)
        boot_config_set_tz_min((int16_t)jtz->valuedouble);
    if (!cJSON_IsNumber(jms)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "epoch_ms required");
        return ESP_FAIL;
    }
    int64_t epoch_ms = (int64_t)jms->valuedouble;
    bool manual = cJSON_IsTrue(jman);
    cJSON_Delete(root);

    if (epoch_ms < FIELD_MIN_EPOCH_S * 1000 || epoch_ms > FIELD_MAX_EPOCH_S * 1000) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "epoch out of range");
        return ESP_FAIL;
    }

    struct timeval now;
    gettimeofday(&now, NULL);
    int64_t now_ms = (int64_t)now.tv_sec * 1000 + now.tv_usec / 1000;
    int64_t dt_abs_sec = llabs(epoch_ms - now_ms) / 1000;

    bool sntp   = net_time_sntp_synced();
    bool accept = net_time_should_accept(sntp, manual, dt_abs_sec);

    if (accept) {
        struct timeval tv = { .tv_sec  = (time_t)(epoch_ms / 1000),
                              .tv_usec = (suseconds_t)((epoch_ms % 1000) * 1000) };
        settimeofday(&tv, NULL);
        net_time_set_source(manual ? TIME_SRC_MANUAL : TIME_SRC_BROWSER);
        rtc_ds3231_save_now();       // #RTC-1: принятое время переживёт выключение
        spectrogram_time_synced();   // пересчёт started_at ongoing-записи (#FW-23)
        ESP_LOGI(TAG, "FIELD-5: time set from %s (was off %lld s)",
                 manual ? "manual" : "browser", (long long)dt_abs_sec);
    }

    char out[96];
    int n = snprintf(out, sizeof(out),
        "{\"ok\":true,\"accepted\":%s,\"source\":\"%s\",\"sntp\":%s}",
        accept ? "true" : "false", net_time_source_str(), sntp ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, out, n);
    return ESP_OK;
}

// #FIELD-4: captive-portal probe endpoints. В полевом AP DNS-hijack (wifi_manager)
// заворачивает все имена на 192.168.4.1; OS-пробы связности (Android generate_204/
// gen_204, iOS/macOS hotspot-detect/library-test, Windows ncsi/connecttest) получают
// здесь 302 → всплывает «Вход в сеть», тап открывает Web UI. В STA (Indoor) плата —
// клиент, проба до неё не доходит; для чистоты отвечаем 204 (как настоящий generate_204).
static esp_err_t handle_captive_probe(httpd_req_t *req)
{
    if (wifi_manager_is_ap_mode()) {
        // #FIELD-10/#FIELD-11: вместо 302 на тяжёлый index отдаём лёгкую captive-landing
        // прямо в мини-браузере ОС — крупный адрес 192.168.4.1 (виден сразу) + авто-редирект
        // на страницу прибора + кнопка-fallback. 200+HTML вместо 204 → OS считает «captive
        // portal», всплывает «Вход в сеть» и открывает нашу страницу.
        return handle_captive_page(req);
    }
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// #FIELD-7 (#SEC-2): POST /api/ap-pass — смена пароля полевого AP. Тело {"password":"..."}.
// Пустой → сброс к дефолту (следующий boot возьмёт AP_PASS_DEFAULT). Непустой обязан быть
// ≥8 символов (мин WPA2-PSK). Хранится в NVS wifi/ap_pass (тот же ключ, что читает
// load_ap_config); применяется после ребута (способ A4 — пересоздавать netif/httpd на
// лету рискованно). Пароль общий для всех (#SEC-2): оператор раздаёт SSID+пароль голосом.
static esp_err_t handle_ap_pass(httpd_req_t *req)
{
    if (!csrf_check(req)) return ESP_FAIL;

    char body[128];
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no body"); return ESP_FAIL; }
    body[len] = '\0';

    cJSON *root = cJSON_Parse(body);
    if (!root) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json"); return ESP_FAIL; }
    cJSON *jp = cJSON_GetObjectItem(root, "password");
    if (!cJSON_IsString(jp)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "password required");
        return ESP_FAIL;
    }
    const char *pass = jp->valuestring;
    size_t plen = strlen(pass);
    if (plen > 0 && plen < 8) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "password must be >=8 chars (WPA2)");
        return ESP_FAIL;
    }
    if (plen >= WIFI_PASS_MAX) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "password too long");
        return ESP_FAIL;
    }

    nvs_handle_t nvs;
    if (nvs_open("wifi", NVS_READWRITE, &nvs) == ESP_OK) {
        if (plen == 0) nvs_erase_key(nvs, "ap_pass");   // сброс к дефолту (not-found ок)
        else           nvs_set_str(nvs, "ap_pass", pass);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"reboot\":true}");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

// #FIELD-6 (T4/T5): переключатель Indoor/Outdoor со стартовой страницы. Тело
// {"mode":"indoor"|"outdoor"}. outdoor → NVS ap_mode=1 (липкий forced AP); indoor →
// снять ap_mode (ребут в STA). Применяется ребутом (способ A4). Тот же ключ ap_mode,
// что читает wifi_manager на старте (FIELD-2b) и ставит setup-кнопка (FIELD-2c).
static esp_err_t handle_net_mode(httpd_req_t *req)
{
    if (!csrf_check(req)) return ESP_FAIL;
    char body[64];
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no body"); return ESP_FAIL; }
    body[len] = '\0';
    cJSON *root = cJSON_Parse(body);
    if (!root) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json"); return ESP_FAIL; }
    cJSON *jm = cJSON_GetObjectItem(root, "mode");
    const char *mode = cJSON_IsString(jm) ? jm->valuestring : NULL;
    bool outdoor;
    if (mode && strcmp(mode, "outdoor") == 0)      outdoor = true;
    else if (mode && strcmp(mode, "indoor") == 0)  outdoor = false;
    else { cJSON_Delete(root); httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "mode must be indoor|outdoor"); return ESP_FAIL; }
    cJSON_Delete(root);

    wifi_manager_set_boot_mode(outdoor);   // #TFT-29: тот же код, что у экрана платы
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"reboot\":true}");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

void web_server_init(void)
{
    csrf_generate();

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    // #WF-1: httpd на core 1 к остальной прикладной сети (#TCP-2). Дефолт
    // tskNO_AFFINITY позволял httpd (prio 5) исполняться на core 0 рядом с
    // USB-приёмом — уводим целиком.
    config.core_id = 1;
    config.max_uri_handlers = 84;        // #ALM-1: +2 /api/alarm (было 80).        // #FS-1: 47 своих + 20 водопада + 5 /api/fs/* = 72 — ровно в старый предел, +8 запас. #WF-2/#MON-1/#FIELD-4: 41 базовых (uris[]: было 33 + 8 FIELD: /api/time,/api/ap-pass,6 captive-проб) + 20 waterfall (web_waterfall_register: 19 reg + /ws/waterfall) = 61. Лимит 45 когда-то переполнялся → тихие 404 у последних хэндлеров → цикл reconnect. 72 = +11 запас.
    config.stack_size = 8192;
    config.max_open_sockets = 11;        // из 16 LWIP-сокетов; запас для tcp_bridge + sntp
    config.lru_purge_enable = true;      // при исчерпании пула закрыть LRU-соединение, не отказывать (errno 23)
    config.uri_match_fn = httpd_uri_match_wildcard;
    // #UI-15 P0: чистим WS-реестр при ЛЮБОМ закрытии сокета (RST/FIN/LRU/F5);
    // без callback зомбирующиеся fd ломают broadcast после нескольких F5.
    config.close_fn = web_waterfall_on_close;
    // #UI-15 P2: сжимаем default recv/send (~5 c) — освобождаем сокеты быстрее
    // под давлением F5+poll, иначе пул держит «полудохлые» соединения долго.
    config.recv_wait_timeout = 3;
    config.send_wait_timeout = 3;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return;
    }

    const httpd_uri_t uris[] = {
        {"/api/csrf-token",              HTTP_GET,  handle_csrf_token,       NULL},
        {"/api/status",                  HTTP_GET,  handle_status,           NULL},
        {"/api/spectrum",                HTTP_GET,  handle_spectrum,         NULL},
        {"/api/spectrum.json",           HTTP_GET,  handle_spectrum_json,    NULL},
        {"/api/command",                 HTTP_POST, handle_command,          NULL},
        {"/api/devlog",                  HTTP_GET,  handle_devlog,           NULL},
        {"/api/monitor/series",          HTTP_GET,  handle_monitor_series,   NULL},  // #MON-1
        {"/api/monitor/session",         HTTP_POST, handle_monitor_session,  NULL},  // #MON-2
        {"/api/reset",                   HTTP_POST, handle_reset,            NULL},
        {"/api/boot-config",             HTTP_GET,  handle_boot_config_get,  NULL},
        {"/api/boot-config",             HTTP_POST, handle_boot_config_set,  NULL},
        {"/api/alarm",                   HTTP_GET,  handle_alarm_get,        NULL},   // #ALM-1
        {"/api/alarm",                   HTTP_POST, handle_alarm_set,        NULL},
        {"/api/save",                    HTTP_POST, handle_save,             NULL},
        {"/api/list",                    HTTP_GET,  handle_list,             NULL},
        {"/api/export.xml",              HTTP_GET,  handle_export_xml,       NULL},
        {"/api/export.csv",              HTTP_GET,  handle_export_csv,       NULL},
        {"/api/export.n42",              HTTP_GET,  handle_export_n42,       NULL},
        {"/api/export.spe",              HTTP_GET,  handle_export_spe,       NULL},
        {"/api/saved/*",                 HTTP_GET,  handle_saved_get,        NULL},
        {"/api/saved/*",                 HTTP_POST, handle_saved_delete,     NULL},
        {"/api/device",                  HTTP_GET,  handle_device,           NULL},
        {"/api/system",                  HTTP_GET,  handle_system,           NULL},
        {"/api/usb-diag",                HTTP_GET,  handle_usb_diag,         NULL},  // #FW-22
        {"/api/reboot-device",           HTTP_POST, handle_reboot_device,    NULL},
        {"/api/battery/iinlim",          HTTP_POST, handle_battery_set_iinlim, NULL},   // #PWR-6
        {"/api/wifi/reset",              HTTP_POST, handle_wifi_reset,       NULL},
        {"/api/reboot-esp",              HTTP_POST, handle_reboot_esp,       NULL},
        {"/api/tft/calibrate",           HTTP_POST, handle_tft_calibrate,    NULL},   // #TFT-21
        {"/api/calibration",             HTTP_POST, handle_set_calibration,  NULL},
        {"/api/settings/backup",         HTTP_GET,  handle_settings_backup,  NULL},
        {"/api/settings/restore",        HTTP_POST, handle_settings_restore, NULL},
        // #FIELD-5/7: установка времени от браузера + смена пароля полевого AP
        {"/api/time",                    HTTP_POST, handle_time_set,         NULL},
        {"/api/ap-pass",                 HTTP_POST, handle_ap_pass,          NULL},
        {"/api/net-mode",                HTTP_POST, handle_net_mode,         NULL},  // #FIELD-6 T4/T5
        // #FIELD-4: captive-portal пробы связности ОС → 302 на Web UI (в AP-режиме)
        {"/generate_204",                HTTP_GET,  handle_captive_probe,    NULL},  // Android
        {"/gen_204",                     HTTP_GET,  handle_captive_probe,    NULL},  // Android alt
        {"/hotspot-detect.html",         HTTP_GET,  handle_captive_probe,    NULL},  // iOS/macOS
        {"/library/test/success.html",   HTTP_GET,  handle_captive_probe,    NULL},  // iOS/macOS
        {"/ncsi.txt",                    HTTP_GET,  handle_captive_probe,    NULL},  // Windows
        {"/connecttest.txt",             HTTP_GET,  handle_captive_probe,    NULL},  // Windows
        {"/captive",                     HTTP_GET,  handle_captive_page,     NULL},  // #FIELD-10/11 landing
        {"/healthcheck",                 HTTP_GET,  handle_healthcheck,      NULL},
        {"/saved",                       HTTP_GET,  handle_saved_page,       NULL},
        {"/system",                      HTTP_GET,  handle_system_page,      NULL},
        {"/service",                     HTTP_GET,  handle_service_page,     NULL},
        {"/monitor",                     HTTP_GET,  handle_monitor_page,     NULL},
        {"/common-time.js",              HTTP_GET,  handle_common_time_js,   NULL},  // #FIELD-5
        {"/",                            HTTP_GET,  handle_index,            NULL},
    };

    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++)
        httpd_register_uri_handler(server, &uris[i]);

    web_waterfall_register(server);      // /waterfall, /api/waterfall/*, /ws/waterfall
    web_files_register(server);          // #FS-1: /api/fs/*
    ota_update_register(server);         // #OTA-1: /api/ota

    ESP_LOGI(TAG, "Web server started on port %d", config.server_port);
}