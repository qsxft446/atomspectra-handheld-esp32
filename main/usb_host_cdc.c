#include "atomspectra.h"
#include "shproto.h"
#include "spectrogram.h"
#include "acq_intent.h"      // #ACQ-1: намерение набора, команды сброса прибора
#include "boot_config.h"     // #CAL-1: «всегда читать калибровку с прибора»
#include "esp_log.h"
#include <inttypes.h>
#include "usb/cdc_acm_host.h"
#include "usb/usb_host.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"   /* #BRIDGE-1: RX-кольцо декаплинга data_cb */
#include "esp_heap_caps.h"   /* #TCP-5: диагностика свободного DMA-блока перед open */
#include "esp_timer.h"       /* #FW-22: timestamp для last_* полей */
#include <string.h>

static const char *TAG = "usb_cdc";

// #FW-22: state диагностики USB Host. Обновляется под mutex из hot-path (RX cb,
// enum cb, open, TX). Getter копирует под тем же mutex.
static usb_diag_snapshot_t s_diag;
static SemaphoreHandle_t s_diag_mutex = NULL;
#define DIAG_LOCK()   do { if (s_diag_mutex) xSemaphoreTake(s_diag_mutex, portMAX_DELAY); } while (0)
#define DIAG_UNLOCK() do { if (s_diag_mutex) xSemaphoreGive(s_diag_mutex); } while (0)

static inline uint32_t diag_now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

#define FTDI_SIO_RESET          0
#define FTDI_SIO_SET_MODEM_CTRL 1
#define FTDI_SIO_SET_FLOW_CTRL  2
#define FTDI_SIO_SET_BAUDRATE   3
#define FTDI_SIO_SET_DATA       4
#define FTDI_REQTYPE_OUT        0x40
#define FTDI_FS_PACKET_SIZE     64

static cdc_acm_dev_hdl_t s_cdc_dev = NULL;
static SemaphoreHandle_t s_tx_mutex = NULL;

// #FW-51 (перенос из апстрима 1.2.5): CDC_ACM_HOST_ERROR раньше только логировался —
// соединение оставалось «открытым», DEV горел, а данные не шли (немое зависание).
// Теперь ошибка, пустая шина или тишина RX закрывают соединение одним путём
// (cdc_teardown), и usb_connect_task открывает его заново.
#define CDC_FAULT_NONE         0
#define CDC_FAULT_DISCONNECT   1
#define CDC_FAULT_ERROR        2
#define CDC_FAULT_RX_WATCHDOG  3
#define CDC_FAULT_BUS_EMPTY    4
#define CDC_RX_GRACE_MS        5000u    // после open столько «подключён» без RX
#define CDC_RX_STALE_MS        8000u    // RX старше — is_connected() = false
#define CDC_RX_WATCHDOG_ARM_MS 10000u   // сторож RX включается через столько после open
#define CDC_RX_WATCHDOG_MS     15000u   // тишина RX столько — закрыть и переоткрыть
// Сброс разбора RX — только в usb_rxw (единственный читатель кольца и парсера).
static volatile bool s_rx_reset_req = false;
static void cdc_teardown(uint8_t reason);

static uint8_t s_rx_buf[4096];
static shproto_struct s_rx_packet;

// #BRIDGE-1: RX-кольцо (internal RAM) для декаплинга USB data_cb от разбора.
// Корень бага (репро Аудитора 2026-07-08): handle_rx (cdc data_cb) делал ВЕСЬ разбор
// синхронно — де-FTDI, s_raw_rx_cb (TCP), побайтный feed_shproto → handle_rx_packet →
// spectrum_process_histogram_chunk (8192-сумма + 32КБ memcpy под SPEC_LOCK). Пока
// data_cb висит, CDC-драйвер не переотправляет USB-IN transfer → FTDI FIFO 256Б
// переполняется за 4.3мс @600000 бод → битый CRC свипа + "histogram sweep dropped".
// Проявлялось ТОЛЬКО с TCP-клиентом: tcp_tx (#TCP-1) молотит 256КБ PSRAM-кольцо, и
// контеншен octal-PSRAM-шины замедлял PSRAM-доступы разбора в data_cb (s_hist_staging
// в PSRAM) → data_cb ещё дольше. Фикс: data_cb делает МИНИМУМ (де-FTDI → payload в это
// кольцо, без блокировки), а usb_rx_worker разгребает. Кольцо в INTERNAL RAM (не PSRAM)
// — иммунитет к контеншену PSRAM-шины на самом hot-path.
#define RX_RING_BYTES     (16 * 1024)
#define RX_RING_TRIGGER   1
static StreamBufferHandle_t s_rx_ring = NULL;
static StaticStreamBuffer_t s_rx_ring_struct;
static uint8_t *s_rx_ring_storage = NULL;
static volatile uint32_t s_rx_ring_drops = 0;  // payload-байт потеряно при переполнении кольца

static uint8_t s_tx_buf[256];
static shproto_struct s_tx_packet;

static usb_raw_rx_cb_t s_raw_rx_cb = NULL;
static int s_rx_cb_count = 0;
static volatile uint32_t s_usb_rx_err = 0;  // #TCP-4: FTDI line-status RX errors (OE/PE/FE/FIFO)

static char s_text_accum[4096];
static int  s_text_accum_len = 0;

// #UI-1: кольцо последних текстовых ответов прибора для веб-лога команд (/api/devlog).
// devlog_push() вызывается из cdc-acm data_cb (handle_rx_packet, CMD_TEXT); чтение —
// из httpd-таска (usb_host_cdc_devlog_json). Синхронизация — отдельный мьютекс.
#define DEVLOG_RING   8
#define DEVLOG_TEXTSZ 480
static char     s_devlog_text[DEVLOG_RING][DEVLOG_TEXTSZ];
static uint16_t s_devlog_len [DEVLOG_RING];
static uint32_t s_devlog_seq [DEVLOG_RING];   // seq записи в слоте (0 = пусто)
static uint32_t s_devlog_next = 1;            // seq для следующей записи (монотонный)
static SemaphoreHandle_t s_devlog_mutex = NULL;

static void devlog_push(const uint8_t *txt, int len)
{
    if (len <= 0 || !s_devlog_mutex) return;
    if (len > DEVLOG_TEXTSZ - 1) len = DEVLOG_TEXTSZ - 1;
    xSemaphoreTake(s_devlog_mutex, portMAX_DELAY);
    uint32_t seq = s_devlog_next++;
    int slot = seq % DEVLOG_RING;
    memcpy(s_devlog_text[slot], txt, len);
    s_devlog_text[slot][len] = '\0';
    s_devlog_len[slot] = (uint16_t)len;
    s_devlog_seq[slot] = seq;
    xSemaphoreGive(s_devlog_mutex);
}

// #FW-2/#FW-3: однократные действия при ПЕРВОМ USB-коннекте после ребута.
// Значения задаёт usb_host_cdc_set_autostart() из main.c (читая boot_config из NVS).
static bool s_boot_autostart_spec = false;
static bool s_boot_autostart_wf   = false;
static bool s_boot_clear_spectrum = false;
static bool s_boot_once_done      = false;

// #CMD-1: распознать завершённый ответ на -cal (дамп 40 регистров).
// Формат подтверждён на реальном приборе (Text(400), один CDC-пакет):
// строки ровно по 8 hex, разделённые \r\n; первые 12 — калибровка+CRC, 39-я — серийник.
// Первая строка = 8 hex + перевод строки, и накоплено >=40 строк → дамп целиком.
static bool is_complete_cal(const char *s)
{
    int h = 0;
    while (h < 8) {
        char c = s[h];
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f')))
            return false;
        h++;
    }
    if (s[8] != '\n' && s[8] != '\r') return false;
    int nl = 0;
    for (const char *q = s; *q; q++) if (*q == '\n') nl++;
    return nl >= 39;
}

// #CAL-1 (апстрим 1.2.28): прибор отвечает на -cal строкой "-ok\n", а дамп приходит
// следующим пакетом — в аккумуляторе перед дампом оказывался "-ok" (и хвосты -ok от
// -sta/-sto), is_complete_cal() с позиции 0 не срабатывала НИКОГДА: калибровка с
// прибора не применялась. Ищем начало дампа с любой строки.
static const char *find_cal_dump(const char *s)
{
    for (const char *p = s; *p; ) {
        if (is_complete_cal(p)) return p;
        const char *nl = strchr(p, '\n');
        if (!nl) break;
        p = nl + 1;
    }
    return NULL;
}

// #CAL-3: подтверждения прибора — строки "ok" / "-ok" в текстовых пакетах. Запись
// регистра (-cal N HHHHHHHH) прибор выполняет заметное время (видимо, пишет во
// флеш): 11 команд с паузой 60 мс вставали в очередь, проверочный -cal оказывался
// в её конце, и дамп не успевал за 3 с — запись считалась неудачной.
static volatile uint32_t s_ok_acks;

static void count_ok_lines(const char *s)
{
    for (const char *p = s; *p; ) {
        const char *e = p;
        while (*e && *e != '\n') e++;
        const char *a = p, *b = e;
        while (a < b && (*a == ' ' || *a == '-')) a++;
        while (b > a && (b[-1] == '\r' || b[-1] == ' ')) b--;
        if (b - a == 2 && a[0] == 'o' && a[1] == 'k') s_ok_acks++;
        p = *e ? e + 1 : e;
    }
}

static void handle_rx_packet(void)
{
    // #FW-22: счётчик пакетов по типу
    DIAG_LOCK();
    // #FW-43: любой дошедший сюда пакет уже прошёл CRC → прибор жив и отвечает.
    s_diag.last_shproto_ts_ms = diag_now_ms();
    switch (s_rx_packet.cmd) {
    case CMD_HISTOGRAM:    s_diag.pkt_hist++; s_diag.last_hist_ts_ms = s_diag.last_shproto_ts_ms; break;
    case CMD_TEXT:         s_diag.pkt_text++; break;
    case CMD_STAT:         s_diag.pkt_stat++; break;
    case CMD_OSCILLOSCOPE: s_diag.pkt_osc++; break;
    default:               s_diag.pkt_unknown++; break;
    }
    DIAG_UNLOCK();
    switch (s_rx_packet.cmd) {
    case CMD_HISTOGRAM:
        spectrum_process_histogram_chunk(s_rx_packet.data, s_rx_packet.len);
        break;
    case CMD_TEXT:
        if (s_rx_packet.len > 0) {
            s_rx_packet.data[s_rx_packet.len] = '\0';
            // #FW-13: LOGD — CDC-таск, консоль UART0 115200 блокирующая: одна строка
            // ~100 симв ≈ 8.7 мс > 4.3 мс FIFO-бюджета FT232R @600000 бод. Текст и так
            // уходит в веб-лог через devlog_push ниже.
            ESP_LOGD(TAG, "Text(%u): %.80s%s", (unsigned)s_rx_packet.len,
                     (const char*)s_rx_packet.data, s_rx_packet.len>80?"...":"");
            devlog_push(s_rx_packet.data, (int)s_rx_packet.len);  // #UI-1: в веб-лог команд
            count_ok_lines((const char *)s_rx_packet.data);         // #CAL-3
            int sp = sizeof(s_text_accum)-s_text_accum_len-1;
            int cp = (int)s_rx_packet.len < sp ? (int)s_rx_packet.len : sp;
            memcpy(s_text_accum+s_text_accum_len, s_rx_packet.data, cp);
            s_text_accum_len += cp; s_text_accum[s_text_accum_len] = '\0';
            // #CMD-1: два РАЗНЫХ ответа прибора, каждый завершает накопление и СБРАСЫВАЕТ
            // аккумулятор немедленно, чтобы они не склеивались (доказано на дампе: -cal,
            // не сброшенный, прилипал к следующему -inf → 48 строк вместо 40, CRC mismatch).
            //  • -inf  → один Text(404) с параметрами (VERSION..PileUpThr). Калибровки НЕ
            //            содержит (прежний #PR-1 комментарий про «калибровку в -inf» был
            //            НЕВЕРЕН — опровергнуто реальным дампом). Триггер: "PileUpThr "+"VERSION ".
            //  • -cal  → один Text(400) с дампом 40 регистров (по 8 hex, \r\n). Калибровка
            //            (L0..L9 = 5 double), CRC L10, серийник L39. Триггер: is_complete_cal().
            const char *cal = find_cal_dump(s_text_accum);   // #CAL-1
            if (cal || (strstr(s_text_accum,"PileUpThr ") && strstr(s_text_accum,"VERSION "))) {
                spectrum_process_info_response(cal ? cal : s_text_accum);
                s_text_accum_len = 0;
            } else if (s_text_accum_len >= 6 && s_text_accum[s_text_accum_len-1] == ']' &&
                       strstr(s_text_accum,"Tcpot ") != NULL) {
                // #DEV-6: ответ на -tc_pot? — один Text-пакет "Tcpot [...]", завершение
                // маркируется закрывающей скобкой (у -inf/-cal свои триггеры выше).
                // Живой прибор шлёт ведущий пробел перед "Tcpot" (подтверждено devlog
                // seq 18, 2026-07-01: " Tcpot [...]") — ищем подстроку, как у -inf,
                // а не якорим strncmp на позицию 0 (иначе триггер не срабатывал никогда).
                spectrum_process_tcpot_response(strstr(s_text_accum,"Tcpot "));
                s_text_accum_len = 0;
            } else if (s_text_accum_len >= (int)sizeof(s_text_accum) - 128) {
                ESP_LOGW(TAG, "text accum overflow without trigger, reset");
                s_text_accum_len = 0;
            }
        }
        break;
    case CMD_STAT:
        spectrum_process_stat_packet(s_rx_packet.data, s_rx_packet.len);
        break;
    case CMD_OSCILLOSCOPE:
        break;
    default:
        ESP_LOGW(TAG, "Unknown cmd 0x%02x len=%u", (unsigned)s_rx_packet.cmd, (unsigned)s_rx_packet.len);
        break;
    }
}

static void feed_shproto(const uint8_t *d, size_t n) {
    for (size_t i = 0; i < n; i++) {
        shproto_byte_received(&s_rx_packet, d[i]);
        if (s_rx_packet.ready) { s_rx_packet.ready = false; handle_rx_packet(); }
    }
}

// #BRIDGE-1: consumer RX-кольца. ЕДИНСТВЕННЫЙ, кто зовёт feed_shproto/s_raw_rx_cb.
// Держит весь тяжёлый разбор (shproto → spectrum, включая 32КБ memcpy под SPEC_LOCK)
// и TCP-раздачу ВНЕ CDC data_cb, чтобы приём USB-IN не голодал. Приоритет 6: НИЖЕ
// CDC-драйвера(8) и usb_lib(7) на core0 — не вытесняет USB-сервис; ВЫШЕ httpd(5) —
// успевает разгребать кольцо. Стек 8192: тот же путь разбора (-inf парсер), что
// раньше жил в CDC-таске с 8192 (P1-1 static-буферы уже вынесены).
static void usb_rx_worker(void *arg)
{
    static uint8_t chunk[2048];
    while (1) {
        if (!s_rx_ring) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }
        if (s_rx_reset_req) {   // #FW-51: хвост прошлого соединения не склеивать с новым
            s_rx_reset_req = false;
            xStreamBufferReset(s_rx_ring);
            shproto_init(&s_rx_packet, s_rx_buf, sizeof(s_rx_buf));
            s_text_accum_len = 0;
        }
        size_t n = xStreamBufferReceive(s_rx_ring, chunk, sizeof(chunk), pdMS_TO_TICKS(200));
        if (n == 0) continue;                 // таймаут, данных нет
        if (s_raw_rx_cb) s_raw_rx_cb(chunk, n);   // порядок как раньше: raw → parser
        feed_shproto(chunk, n);
    }
}

static bool handle_rx(const uint8_t *data, size_t data_len, void *arg) {
    ++s_rx_cb_count;
    // #FW-22: снимок первых 16 байт + счётчики
    uint32_t now = diag_now_ms();
    char h[48]; size_t p=0, show = data_len<16?data_len:16;
    for (size_t i=0;i<show;i++) p+=snprintf(h+p,sizeof(h)-p,"%02x ",data[i]);
    DIAG_LOCK();
    s_diag.rx_cb_count = s_rx_cb_count;
    s_diag.rx_bytes += (uint32_t)data_len;
    s_diag.rx_last_ts_ms = now;
    s_diag.rx_last_len = (uint32_t)data_len;
    strncpy(s_diag.rx_last_first16_hex, h, sizeof(s_diag.rx_last_first16_hex)-1);
    s_diag.rx_last_first16_hex[sizeof(s_diag.rx_last_first16_hex)-1] = '\0';
    s_diag.drv_task_alive_ts_ms = now;
    DIAG_UNLOCK();
    if (s_rx_cb_count <= 10) {
        ESP_LOGI(TAG,"RX#%d len=%u: %s",s_rx_cb_count,(unsigned)data_len,h);
    }
    // #BRIDGE-1: data_cb — МИНИМУМ. Де-FTDI (снять 2 status-байта с каждого 64-байтного
    // USB-пакета — привязка к границам USB-кадра видна только здесь), проверить
    // line-status, сложить payload в RX-кольцо без блокировки. Разбор — в usb_rx_worker.
    for (size_t off=0; off<data_len; ) {
        size_t ch=data_len-off; if(ch>64)ch=64; if(ch<=2)break;
        if (data[off+1] & 0x8E) {
            s_usb_rx_err++;  // #TCP-4: FTDI line-status byte = OE|PE|FE|FIFO
            DIAG_LOCK(); s_diag.line_status_errors = s_usb_rx_err; DIAG_UNLOCK();
        }
        if (s_rx_ring) {
            size_t put = xStreamBufferSend(s_rx_ring, data+off+2, ch-2, 0);  // 0 = не блокировать
            if (put < ch-2) s_rx_ring_drops += (uint32_t)((ch-2) - put);      // кольцо переполнено
        } else {
            // фоллбэк: кольцо не выделилось — старый синхронный путь прямо в data_cb
            if(s_raw_rx_cb) s_raw_rx_cb(data+off+2,ch-2);
            feed_shproto(data+off+2,ch-2);
        }
        off+=ch;
    }
    return true;
}

static void handle_event(const cdc_acm_host_dev_event_data_t *event, void *user_ctx)
{
    switch (event->type) {
    case CDC_ACM_HOST_ERROR:
        ESP_LOGE(TAG, "CDC error — teardown, reconnect");   // #FW-51: было только в лог
        cdc_teardown(CDC_FAULT_ERROR);
        break;
    case CDC_ACM_HOST_DEVICE_DISCONNECTED:
        ESP_LOGW(TAG, "Device disconnected");
        cdc_teardown(CDC_FAULT_DISCONNECT);
        break;
    default:
        break;
    }
}

// #FW-51: единый путь закрытия (отключение / ошибка CDC / сторож RX / пустая шина).
// Хендл обнуляется первым — is_connected() сразу видит «нет», повторный вызов безвреден.
static void cdc_teardown(uint8_t reason)
{
    cdc_acm_dev_hdl_t h;
    DIAG_LOCK();
    h = s_cdc_dev;
    s_cdc_dev = NULL;
    s_diag.cdc_open = false;
    if (h) {
        if (reason == CDC_FAULT_ERROR)       s_diag.cdc_error_count++;
        if (reason == CDC_FAULT_RX_WATCHDOG) s_diag.rx_watchdog_trips++;
        if (reason == CDC_FAULT_BUS_EMPTY)   s_diag.bus_empty_trips++;
        s_diag.last_fault_reason = reason;
        s_diag.last_fault_ts_ms = diag_now_ms();
    }
    DIAG_UNLOCK();
    if (!h) return;
    esp_err_t e = cdc_acm_host_close(h);
    if (e != ESP_OK) ESP_LOGW(TAG, "cdc_acm_host_close: %s (reason=%u)", esp_err_to_name(e), (unsigned)reason);
    s_rx_reset_req = true;
    static const char *const names[] = { "none", "disconnect", "error", "rx_watchdog", "bus_empty" };
    ESP_LOGW(TAG, "CDC teardown: %s — reconnect armed", reason < 5 ? names[reason] : "?");
}

static bool enum_filter_cb(const usb_device_desc_t *dev_desc, uint8_t *bConfigurationValue)
{
    ESP_LOGI(TAG, "USB device enumerated: VID=%04x PID=%04x class=%d subclass=%d",
             dev_desc->idVendor, dev_desc->idProduct,
             dev_desc->bDeviceClass, dev_desc->bDeviceSubClass);
    // #FW-22: снапшот последнего увиденного устройства (даже если VID/PID не наш)
    DIAG_LOCK();
    s_diag.enum_cb_count++;
    s_diag.last_seen_vid = dev_desc->idVendor;
    s_diag.last_seen_pid = dev_desc->idProduct;
    s_diag.last_seen_class = dev_desc->bDeviceClass;
    s_diag.last_seen_subclass = dev_desc->bDeviceSubClass;
    s_diag.last_seen_ts_ms = diag_now_ms();
    DIAG_UNLOCK();
    *bConfigurationValue = 1;
    return true;
}

static void usb_host_lib_task(void *arg)
{
    while (1) {
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        DIAG_LOCK();
        s_diag.last_host_event_flags = event_flags;
        s_diag.host_event_ts_ms = diag_now_ms();
        DIAG_UNLOCK();
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
        }
    }
}

static void try_open_device(void)
{
    if (s_cdc_dev) return;

    const cdc_acm_host_device_config_t dev_config = {
        .connection_timeout_ms = 5000,
        .out_buffer_size = 1024,
        .in_buffer_size = 1024,        /* #TCP-5: откат до 1024 — 36КБ → ESP_ERR_NO_MEM (DMA largest_block=31744). Тест на native Linux отложен до починки железа */
        .event_cb = handle_event,
        .data_cb = handle_rx,
        .user_arg = NULL,
    };

    static int s_attempt = 0;
    s_attempt++;

    int num_devs = 0;
    uint8_t dev_addrs[8];
    usb_host_device_addr_list_fill(sizeof(dev_addrs)/sizeof(dev_addrs[0]), dev_addrs, &num_devs);
    DIAG_LOCK(); s_diag.bus_devs_now = (uint8_t)num_devs; s_diag.conn_task_alive_ts_ms = diag_now_ms(); DIAG_UNLOCK();
    if (num_devs > 0 && (s_attempt <= 3 || (s_attempt % 15) == 0)) {
        ESP_LOGI(TAG, "USB bus: %d device(s) enumerated (addrs:", num_devs);
        for (int i = 0; i < num_devs; i++) ESP_LOGI(TAG, "  addr=%d", dev_addrs[i]);
    }

    /* #TCP-5 диагностика: сколько DMA/internal RAM реально доступно перед аллокацией in_buffer (36КБ) */
    if (s_attempt <= 3 || (s_attempt % 15) == 0) {
        ESP_LOGW(TAG, "#TCP-5 DMA free: largest_block=%u total=%u | INTERNAL largest=%u total=%u | want in_buf=%u",
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)dev_config.in_buffer_size);
    }

    // #FW-22: dma-снимок перед open (для web-diag)
    DIAG_LOCK();
    s_diag.dma_free_largest = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
    s_diag.dma_free_total   = heap_caps_get_free_size(MALLOC_CAP_DMA);
    s_diag.open_attempts    = (uint32_t)s_attempt;
    DIAG_UNLOCK();

    esp_err_t err = cdc_acm_host_open_vendor_specific(ANALYZER_VID, ANALYZER_PID, 0, &dev_config, &s_cdc_dev);
    DIAG_LOCK();
    s_diag.last_open_errno = (int32_t)err;
    s_diag.last_open_ts_ms = diag_now_ms();
    s_diag.cdc_open = (err == ESP_OK);
    DIAG_UNLOCK();
    if (err != ESP_OK) {
        if (s_attempt <= 3 || (s_attempt % 15) == 0) {
            ESP_LOGW(TAG, "open failed: %s (attempt %d, devs_on_bus=%d)", esp_err_to_name(err), s_attempt, num_devs);
        }
        return;
    }

    // #FW-22: захват результата каждого FTDI control-request в bitmask
    uint8_t ftdi_mask = 0;
    esp_err_t fe = ESP_OK, flast = ESP_OK;
    fe = cdc_acm_host_send_custom_request(s_cdc_dev,FTDI_REQTYPE_OUT,FTDI_SIO_RESET,0,0,0,NULL);        if (fe==ESP_OK) ftdi_mask |= 0x01; else flast = fe;
    fe = cdc_acm_host_send_custom_request(s_cdc_dev,FTDI_REQTYPE_OUT,FTDI_SIO_SET_BAUDRATE,5,0,0,NULL); if (fe==ESP_OK) ftdi_mask |= 0x02; else flast = fe;
    fe = cdc_acm_host_send_custom_request(s_cdc_dev,FTDI_REQTYPE_OUT,FTDI_SIO_SET_DATA,0x0008,0,0,NULL);if (fe==ESP_OK) ftdi_mask |= 0x04; else flast = fe;
    fe = cdc_acm_host_send_custom_request(s_cdc_dev,FTDI_REQTYPE_OUT,FTDI_SIO_SET_FLOW_CTRL,0,0,0,NULL);if (fe==ESP_OK) ftdi_mask |= 0x08; else flast = fe;
    fe = cdc_acm_host_send_custom_request(s_cdc_dev,FTDI_REQTYPE_OUT,FTDI_SIO_SET_MODEM_CTRL,0x0303,0,0,NULL); if (fe==ESP_OK) ftdi_mask |= 0x10; else flast = fe;
    fe = cdc_acm_host_send_custom_request(s_cdc_dev,FTDI_REQTYPE_OUT,FTDI_SIO_RESET,1,0,0,NULL);        if (fe==ESP_OK) ftdi_mask |= 0x20; else flast = fe;
    fe = cdc_acm_host_send_custom_request(s_cdc_dev,FTDI_REQTYPE_OUT,FTDI_SIO_RESET,2,0,0,NULL);        if (fe==ESP_OK) ftdi_mask |= 0x40; else flast = fe;
    DIAG_LOCK(); s_diag.ftdi_step_ok_mask = ftdi_mask; s_diag.ftdi_last_errno = (int32_t)flast; DIAG_UNLOCK();
    ESP_LOGI(TAG, "FTDI configured baud=%u 8N1", ANALYZER_BAUD);
    ESP_LOGI(TAG, "Analyzer connected (VID=%04x PID=%04x)", ANALYZER_VID, ANALYZER_PID);
    s_rx_cb_count = 0;
    s_rx_reset_req = true;          // #FW-51: разбор с чистого листа
    spectrum_usb_session_start();   // #SB-1: STAT прошлого сеанса не в счёт
    vTaskDelay(pdMS_TO_TICKS(100));

    shproto_init(&s_tx_packet, s_tx_buf, sizeof(s_tx_buf));
    shproto_packet_start(&s_tx_packet, CMD_TEXT);
    const char *cmd = "-inf";
    while (*cmd) shproto_packet_add_data(&s_tx_packet, *cmd++);
    shproto_packet_add_data(&s_tx_packet, '\0');
    shproto_packet_complete(&s_tx_packet);
    esp_err_t txerr = cdc_acm_host_data_tx_blocking(s_cdc_dev, s_tx_packet.data, s_tx_packet.len, 1000);
    DIAG_LOCK();
    s_diag.tx_packets++; s_diag.tx_bytes += s_tx_packet.len;
    s_diag.last_tx_ts_ms = diag_now_ms(); s_diag.last_tx_errno = (int32_t)txerr;
    strncpy(s_diag.last_tx_cmd, "-inf", sizeof(s_diag.last_tx_cmd)-1); s_diag.last_tx_cmd[sizeof(s_diag.last_tx_cmd)-1]='\0';
    DIAG_UNLOCK();
    ESP_LOGI(TAG, "Sent -inf (%u bytes) rc=%s", (unsigned)s_tx_packet.len, esp_err_to_name(txerr));

    // Однократно отметить, что первый коннект после ребута состоялся: автозапуск
    // (#FW-2) и очистка прибора (#FW-3) применяются ровно один раз за boot, не на
    // каждый реконнект.
    bool first_connect = !s_boot_once_done;
    s_boot_once_done = true;

    if (spectrogram_is_recording()) {
        // #REC-6: если на момент (ре)коннекта водопад пишется — возобновить набор
        // спектра на приборе (-sta). Защищает от случайной остановки анализатора и
        // от ситуации «ESP ребутнулся, восстановил запись, но прибор уже не набирает».
        // Запись уже идёт → автозапуск #FW-2 не нужен (намеренно пропускаем).
        vTaskDelay(pdMS_TO_TICKS(100));
        usb_host_send_text_command("-sta");
        ESP_LOGW(TAG, "recording active — resent -sta to resume acquisition");
    } else if (first_connect) {
        // #FW-3: очистка спектра при старте — сбросить гистограмму прибора тем же
        // путём, что кнопка «Сброс» (-rst). Локальный spectrum_reset() уже сделан в
        // main.c на boot; -rst синхронизирует прибор, чтобы первый пакет не «поднял»
        // обнулённый спектр обратно.
        if (s_boot_clear_spectrum) {
            vTaskDelay(pdMS_TO_TICKS(100));
            usb_host_send_text_command("-rst");
            ESP_LOGW(TAG, "FW-3: boot clear spectrum — sent -rst to device");
        }
        // #FW-2: автозапуск при старте платы (по настройкам NVS, по умолчанию OFF).
        if (s_boot_autostart_wf) {
            vTaskDelay(pdMS_TO_TICKS(100));
            spectrogram_start();              // начать запись водопада (сегменты)
            usb_host_send_text_command("-sta");
            ESP_LOGW(TAG, "FW-2: boot autostart — waterfall recording started");
        } else if (s_boot_autostart_spec) {
            vTaskDelay(pdMS_TO_TICKS(100));
            usb_host_send_text_command("-sta");
            ESP_LOGW(TAG, "FW-2: boot autostart — spectrum acquisition started");
        }
    }
}

void usb_host_cdc_set_autostart(bool autostart_spectrum, bool autostart_waterfall, bool clear_spectrum)
{
    s_boot_autostart_spec = autostart_spectrum;
    s_boot_autostart_wf   = autostart_waterfall;
    s_boot_clear_spectrum = clear_spectrum;
}

static void usb_connect_task(void *arg)
{
    while (1) {
        // #FW-51: соединение «открыто», но шина пуста или RX молчит — закрыть и
        // переоткрыть, не дожидаясь DISCONNECTED, который после ERROR может не прийти.
        if (s_cdc_dev) {
            int num_devs = 0;
            uint8_t dev_addrs[8];
            usb_host_device_addr_list_fill(sizeof(dev_addrs)/sizeof(dev_addrs[0]), dev_addrs, &num_devs);
            DIAG_LOCK();
            s_diag.bus_devs_now = (uint8_t)num_devs;
            uint32_t now = diag_now_ms(), open_ts = s_diag.last_open_ts_ms, rx_ts = s_diag.rx_last_ts_ms;
            DIAG_UNLOCK();
            uint32_t open_age = now - open_ts;
            if (open_age >= CDC_RX_WATCHDOG_ARM_MS) {
                uint32_t rx_age = (int32_t)(rx_ts - open_ts) >= 0 ? now - rx_ts : open_age;
                if (num_devs == 0) {
                    ESP_LOGW(TAG, "bus empty while CDC open — teardown");
                    cdc_teardown(CDC_FAULT_BUS_EMPTY);
                } else if (rx_age >= CDC_RX_WATCHDOG_MS) {
                    ESP_LOGW(TAG, "RX watchdog: no RX for %u ms — teardown", (unsigned)rx_age);
                    cdc_teardown(CDC_FAULT_RX_WATCHDOG);
                }
            }
        }
        // #ACQ-1: набор запускала сама плата (голый -sta), а гистограмм нет 20 с —
        // прибор перезагрузился (провал питания, #PWR-9) и стоит. RX-сторож этого не
        // видит: FTDI шлёт 2-байтовый статус и без прибора. Одного -sta достаточно.
        if (s_cdc_dev) {
            DIAG_LOCK();
            uint32_t now = diag_now_ms(), hist_ts = s_diag.last_hist_ts_ms;
            bool due = acq_watch_resend_due(s_diag.acq_intent, now, s_diag.last_open_ts_ms,
                                            hist_ts, s_diag.last_acq_resend_ts_ms);
            if (due) { s_diag.last_acq_resend_ts_ms = now; s_diag.acq_resend_count++; }
            DIAG_UNLOCK();
            if (due) {
                ESP_LOGW(TAG, "acq watch: no histogram for %u ms, intent=run — resend -sta",
                         (unsigned)(now - hist_ts));
                usb_host_send_text_command("-sta");
            }
        }
        try_open_device();
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

void usb_host_cdc_init(void)
{
    shproto_init(&s_rx_packet, s_rx_buf, sizeof(s_rx_buf));
    shproto_init(&s_tx_packet, s_tx_buf, sizeof(s_tx_buf));
    s_tx_mutex = xSemaphoreCreateMutex();
    s_devlog_mutex = xSemaphoreCreateMutex();  // #UI-1
    s_diag_mutex = xSemaphoreCreateMutex();    // #FW-22
    memset(&s_diag, 0, sizeof(s_diag));

    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
        .enum_filter_cb = enum_filter_cb,
    };
    esp_err_t err = usb_host_install(&host_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "usb_host_install failed: %s", esp_err_to_name(err));
        return;
    }
    DIAG_LOCK(); s_diag.host_installed = true; DIAG_UNLOCK();  // #FW-22
    ESP_LOGI(TAG, "USB Host library installed");

    // #FW-8: usb_lib и CDC driver ВЫШЕ httpd (prio 5, tskNO_AFFINITY). Иначе при
    // скачивании сегмента httpd-таск секундами крутит flash-read+send на core 0 и
    // голодом душит USB-приём: FTDI FIFO переполняется, chunk-и свипа теряются
    // все ~35-40 c окна выгрузки → 7 строк dur=0 подряд (наблюдение 2026-07-02).
    // WiFi/LWIP (prio 18+) всё равно выше — их не задеваем.
    xTaskCreatePinnedToCore(usb_host_lib_task, "usb_lib", 4096, NULL, 7, NULL, 0);

    const cdc_acm_host_driver_config_t driver_config = {
        // 8192: парсинг info-response (spectrum_process_info_response) идёт в
        // контексте этого таска через data_cb→handle_rx_packet и раньше клал
        // ~3.5 КБ локальных буферов на стек (P1-1). Буфер вынесен в static,
        // плюс расширенный стек убирает остаточный риск переполнения.
        .driver_task_stack_size = 8192,
        .driver_task_priority = 8,   // #FW-8: выше httpd(5) и usb_lib(7)
        .xCoreID = 0,
    };
    err = cdc_acm_host_install(&driver_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cdc_acm_host_install failed: %s", esp_err_to_name(err));
        return;
    }
    DIAG_LOCK(); s_diag.cdc_driver_installed = true; DIAG_UNLOCK();  // #FW-22
    ESP_LOGI(TAG, "CDC-ACM driver installed");

    // #BRIDGE-1: RX-кольцо в INTERNAL RAM (не PSRAM — иммунитет к контеншену шины
    // PSRAM от tcp_tx 256КБ-кольца, тот контеншен = причина "только с TCP-клиентом").
    // 16КБ+1: xStreamBuffer держит 1 байт служебный. Non-DMA internal ~220КБ есть.
    s_rx_ring_storage = heap_caps_malloc(RX_RING_BYTES + 1, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (s_rx_ring_storage) {
        s_rx_ring = xStreamBufferCreateStatic(RX_RING_BYTES, RX_RING_TRIGGER,
                                              s_rx_ring_storage, &s_rx_ring_struct);
    }
    if (s_rx_ring) {
        // usb_rxw prio 6 core0: ниже CDC(8)/usb_lib(7) — не вытесняет USB-сервис,
        // выше httpd(5) — успевает разгребать кольцо в окне выгрузки сегмента.
        xTaskCreatePinnedToCore(usb_rx_worker, "usb_rxw", 8192, NULL, 6, NULL, 0);
        ESP_LOGI(TAG, "RX ring %d B (internal) + usb_rxw started", RX_RING_BYTES);
    } else {
        // фоллбэк: без кольца data_cb уходит в старый синхронный путь (s_rx_ring==NULL)
        ESP_LOGW(TAG, "RX ring alloc FAILED -> synchronous data_cb fallback");
    }

    xTaskCreatePinnedToCore(usb_connect_task, "usb_conn", 4096, NULL, 2, NULL, 0);
    ESP_LOGI(TAG, "USB Host CDC initialized, waiting for analyzer...");
}

bool usb_host_cdc_is_connected(void)
{
    // #FW-51: одного хендла мало — после ошибки CDC он часами «жил» при замёрзшем RX.
    if (!s_cdc_dev) return false;
    DIAG_LOCK();
    uint32_t open_ts = s_diag.last_open_ts_ms, rx_ts = s_diag.rx_last_ts_ms;
    DIAG_UNLOCK();
    uint32_t now = diag_now_ms();
    if (now - open_ts < CDC_RX_GRACE_MS) return true;
    if ((int32_t)(rx_ts - open_ts) < 0) return false;
    return now - rx_ts < CDC_RX_STALE_MS;
}

// #ACQ-1: прибором управляет чужой клиент (TCP-мост) — сторож молчит до своего -sta.
void usb_host_cdc_acq_intent_external(void)
{
    DIAG_LOCK();
    s_diag.acq_intent = ACQ_INTENT_UNKNOWN;
    DIAG_UNLOCK();
}

// #ACQ-1: перед перезагрузкой прибора с платы: если набор шёл — сторож вернёт его.
void usb_host_cdc_acq_intent_device_reboot(void)
{
    DIAG_LOCK();
    uint32_t hist_ts = s_diag.last_hist_ts_ms;
    if (hist_ts != 0 && diag_now_ms() - hist_ts < ACQ_WATCH_MS) s_diag.acq_intent = ACQ_INTENT_RUN;
    DIAG_UNLOCK();
}

int usb_host_cdc_send(const uint8_t *data, size_t len)
{
    if (!s_cdc_dev) return -1;
    if (xSemaphoreTake(s_tx_mutex, pdMS_TO_TICKS(500)) != pdTRUE) return -1;
    esp_err_t err = cdc_acm_host_data_tx_blocking(s_cdc_dev, data, len, 1000);
    xSemaphoreGive(s_tx_mutex);
    return (err == ESP_OK) ? 0 : -1;
}

void usb_host_cdc_set_raw_rx_cb(usb_raw_rx_cb_t cb)
{
    s_raw_rx_cb = cb;
}

uint32_t usb_host_cdc_rx_errors(void)
{
    return s_usb_rx_err;
}

// #BRIDGE-1: payload-байт потеряно при переполнении RX-кольца (data_cb → ring).
// >0 => usb_rx_worker не успевает разгребать => фикс развязки на пределе. Прямой
// индикатор здоровья буфера под TCP-нагрузкой (валидация Андрея).
uint32_t usb_host_cdc_rx_ring_drops(void)
{
    return s_rx_ring_drops;
}

// #FW-22: снапшот всей USB-Host диагностики под тем же mutex, что и hot-path писатели.
// uptime_ms проставляется в момент чтения (в s_diag не хранится) — по нему клиент судит
// «свежесть» last_*_ts_ms относительно now. Используется endpoint /api/usb-diag (#FW-43).
void usb_host_cdc_diag_snapshot(usb_diag_snapshot_t *out)
{
    if (!out) return;
    DIAG_LOCK();
    *out = s_diag;
    DIAG_UNLOCK();
    out->uptime_ms = diag_now_ms();
}

// #FW-43: см. декларацию в atomspectra.h. true = «прибор определился, но не запитан».
// CDC открыт (usb_host_cdc_is_connected) И FTDI-кадры свежие (rx_last_ts <4с — keep-alive
// '01 60' идёт, значит это НЕ обычный физ. disconnect) И с момента открытия CDC прошло >6с
// (дали окно на ответ -inf) И за это время НИ одного SHPROTO-пакета (last_shproto_ts_ms <
// last_open_ts_ms). Здоровый прибор отвечает на -inf за 1-2с → last_shproto_ts_ms > last_open
// → false навсегда. На реконнекте last_open_ts_ms обновляется → «молчащий» ловится снова.
bool usb_host_cdc_spectrometer_dead(void)
{
    if (!usb_host_cdc_is_connected()) return false;   // ничего не открыто → не «мёртвый»
    DIAG_LOCK();
    uint32_t now      = diag_now_ms();
    uint32_t rx_age   = now - s_diag.rx_last_ts_ms;
    uint32_t open_age = now - s_diag.last_open_ts_ms;
    bool silent_since_open = (s_diag.last_shproto_ts_ms < s_diag.last_open_ts_ms);
    DIAG_UNLOCK();
    if (rx_age >= 4000) return false;    // FTDI-кадры не идут → обычный disconnect, не наш случай
    if (open_age < 6000) return false;   // окно на ответ -inf после (ре)открытия CDC
    return silent_since_open;
}

// #UI-1: JSON-массив текстовых ответов прибора с seq>since:
//   {"lines":[{"seq":N,"text":"..."},...],"next":M}
// Вызывается из httpd-таска (handle_devlog). Текст эскейпится для JSON: " \ → \" \\,
// \n → \\n, \r/\t и непечатные/не-ASCII символы отбрасываются. out должен быть ≥ ~9 КБ.
void usb_host_cdc_devlog_json(uint32_t since, char *out, size_t outsz)
{
    if (!out || outsz < 32) { if (out && outsz) out[0] = '\0'; return; }
    uint32_t next, start;
    if (s_devlog_mutex) xSemaphoreTake(s_devlog_mutex, portMAX_DELAY);
    next = s_devlog_next - 1;                 // последний присвоенный seq (0 если пусто)
    if (s_devlog_mutex) xSemaphoreGive(s_devlog_mutex);

    size_t pos = 0;
    pos += snprintf(out + pos, outsz - pos, "{\"lines\":[");
    // окно: только то, что ещё в кольце (последние DEVLOG_RING записей)
    start = since + 1;
    if (next >= DEVLOG_RING && start < next - DEVLOG_RING + 1) start = next - DEVLOG_RING + 1;

    char tmp[DEVLOG_TEXTSZ];
    int emitted = 0;
    for (uint32_t s = start; s <= next && next != 0; s++) {
        int tlen = -1;
        if (s_devlog_mutex) xSemaphoreTake(s_devlog_mutex, portMAX_DELAY);
        int slot = s % DEVLOG_RING;
        if (s_devlog_seq[slot] == s) {        // ещё не вытеснен
            tlen = s_devlog_len[slot];
            if (tlen > DEVLOG_TEXTSZ - 1) tlen = DEVLOG_TEXTSZ - 1;
            memcpy(tmp, s_devlog_text[slot], tlen);
        }
        if (s_devlog_mutex) xSemaphoreGive(s_devlog_mutex);
        if (tlen < 0) continue;

        // запас под объект; если мало места — прекращаем (next всё равно отдадим)
        if (pos + (size_t)tlen * 2 + 48 >= outsz) break;
        pos += snprintf(out + pos, outsz - pos, "%s{\"seq\":%" PRIu32 ",\"text\":\"",
                        emitted ? "," : "", s);
        for (int i = 0; i < tlen && pos + 8 < outsz; i++) {
            unsigned char c = (unsigned char)tmp[i];
            if (c == '"' || c == '\\')      { out[pos++] = '\\'; out[pos++] = c; }
            else if (c == '\n')             { out[pos++] = '\\'; out[pos++] = 'n'; }
            else if (c >= 0x20 && c < 0x7f) { out[pos++] = c; }
            // \r, \t, прочие управляющие/не-ASCII — пропускаем
        }
        pos += snprintf(out + pos, outsz - pos, "\"}");
        emitted++;
    }
    snprintf(out + pos, outsz - pos, "],\"next\":%" PRIu32 "}", next);
}

static int send_text_raw(const char *cmd)
{
    uint8_t pkt_buf[600];   // до 255 символов + экранирование shproto
    shproto_struct pkt;
    shproto_init(&pkt, pkt_buf, sizeof(pkt_buf));
    shproto_packet_start(&pkt, CMD_TEXT);
    while (*cmd) shproto_packet_add_data(&pkt, *cmd++);
    shproto_packet_add_data(&pkt, '\0');
    shproto_packet_complete(&pkt);
    return usb_host_cdc_send(pkt.data, pkt.len);
}

// #CAL-1 (перенос из апстрима 1.2.28/1.2.30): перед стартом набора, если на плате
// калибровки нет (или включено «всегда читать с прибора»), спросить её у прибора
// (-cal) и дождаться разбора дампа — до 2 с. Только если набор на приборе не идёт
// (гистограмм нет последние 3 с): повторный старт идущего набора не читает её заново.
static void calib_autoread_before_start(void)
{
    if (spectrum_calib_is_set() && !boot_config_calib_always()) return;
    DIAG_LOCK();
    uint32_t hist_ts = s_diag.last_hist_ts_ms;
    DIAG_UNLOCK();
    if (hist_ts != 0 && diag_now_ms() - hist_ts < 3000) return;
    uint32_t seq0 = spectrum_cal_dump_seq();
    if (send_text_raw("-cal") != 0) return;
    for (int i = 0; i < 20 && spectrum_cal_dump_seq() == seq0; i++) vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGI(TAG, "#CAL-1: калибровка с прибора перед стартом: %s",
             spectrum_cal_dump_seq() != seq0 ? "ответ получен" : "ответа нет за 2 с");
}

// #CAL-2 (2026-10-14): калибровка жила только на плате, а прибор хранил старую —
// любой -cal (кнопка «Считать с прибора», «Инфо», программа на ПК через мост)
// возвращал старую, и плата её применяла: «калибруется, потом возвращается».
// Теперь применённая калибровка пишется и в прибор. Формат подтверждён дампом
// прибора пользователя: регистры 0..9 — 5 коэффициентов double, старшее 32-битное
// слово в чётном регистре; регистр 10 — CRC32 (стандартный) ASCII-строки регистров
// 0..9 заглавными hex; запись — "-cal N HHHHHHHH" (ответ "ok"; проверено на
// неиспользуемом регистре 38). После записи — дамп -cal и побитная сверка.
int usb_host_write_device_calibration(const double *coeffs, int order)
{
    if (!s_cdc_dev) return -1;
    char regs[11][9], cat[81] = "";
    for (int c = 0; c < CALIB_COEFFS; c++) {
        double v = c <= order ? coeffs[c] : 0.0;
        uint64_t raw;
        memcpy(&raw, &v, sizeof raw);
        snprintf(regs[2 * c], sizeof regs[0], "%08lX", (unsigned long)(raw >> 32));
        snprintf(regs[2 * c + 1], sizeof regs[0], "%08lX", (unsigned long)(raw & 0xFFFFFFFFu));
    }
    for (int i = 0; i < 10; i++) strncat(cat, regs[i], sizeof cat - strlen(cat) - 1);
    uint32_t cc = 0xFFFFFFFF;   // тот же CRC32, что проверяет разбор дампа (spectrum.c)
    for (int i = 0; cat[i]; i++) {
        cc ^= (uint8_t)cat[i];
        for (int j = 0; j < 8; j++) cc = (cc & 1) ? (cc >> 1) ^ 0xEDB88320 : cc >> 1;
    }
    snprintf(regs[10], sizeof regs[0], "%08lX", (unsigned long)(cc ^ 0xFFFFFFFF));
    for (int i = 0; i <= 10; i++) {
        char cmd[24];
        snprintf(cmd, sizeof cmd, "-cal %d %.8s", i, regs[i]);
        uint32_t ack0 = s_ok_acks;
        if (send_text_raw(cmd) != 0) {
            ESP_LOGW(TAG, "#CAL-2: запись регистра %d не отправлена", i);
            return -2;
        }
        // #CAL-3: следующая запись — только после "ok" на эту (до 2 с).
        int w = 0;
        while (s_ok_acks == ack0 && w < 200) { vTaskDelay(pdMS_TO_TICKS(10)); w++; }
        if (s_ok_acks == ack0) {
            ESP_LOGW(TAG, "#CAL-2: нет ok на запись регистра %d", i);
            return -3;
        }
        if (i == 0) ESP_LOGI(TAG, "#CAL-3: запись регистра прибора — ok через %d мс", w * 10);
    }
    uint32_t seq0 = spectrum_cal_dump_last(NULL);
    if (send_text_raw("-cal") != 0) return -2;
    double got[CALIB_COEFFS];
    for (int i = 0; i < 30 && spectrum_cal_dump_last(got) == seq0; i++) vTaskDelay(pdMS_TO_TICKS(100));
    if (spectrum_cal_dump_last(got) == seq0) {
        ESP_LOGW(TAG, "#CAL-2: прибор не прислал дамп -cal после записи");
        return -3;
    }
    for (int c = 0; c < CALIB_COEFFS; c++) {
        double v = c <= order ? coeffs[c] : 0.0;
        if (memcmp(&v, &got[c], sizeof v) != 0) {
            ESP_LOGW(TAG, "#CAL-2: в приборе не то: коэф. %d записан %.10g, прочитан %.10g", c, v, got[c]);
            return -4;
        }
    }
    ESP_LOGI(TAG, "#CAL-2: калибровка записана в прибор и проверена (CRC %s)", regs[10]);
    return 0;
}

// #CAL-4: фоновая запись калибровки в прибор.
static volatile int s_caldev_state;
static double       s_caldev_c[CALIB_COEFFS];
static int          s_caldev_order;

static void caldev_task(void *arg)
{
    (void)arg;
    int r = usb_host_write_device_calibration(s_caldev_c, s_caldev_order);
    s_caldev_state = r == 0 ? CALDEV_OK : r == -1 ? CALDEV_NODEV : CALDEV_FAIL;
    vTaskDelete(NULL);
}

int usb_host_write_device_calibration_async(const double *coeffs, int order)
{
    if (s_caldev_state == CALDEV_BUSY) return -1;
    if (!s_cdc_dev) { s_caldev_state = CALDEV_NODEV; return 0; }
    for (int i = 0; i < CALIB_COEFFS; i++) s_caldev_c[i] = i <= order ? coeffs[i] : 0.0;
    s_caldev_order = order;
    s_caldev_state = CALDEV_BUSY;
    // Стек во внутренней RAM на ~11 с (правило #ACAL-3: USB-команды — не из PSRAM-стека).
    if (xTaskCreate(caldev_task, "caldev", 4096, NULL, 3, NULL) != pdPASS) {
        s_caldev_state = CALDEV_FAIL;
        return -1;
    }
    return 0;
}

int usb_host_caldev_state(void) { return s_caldev_state; }

int usb_host_send_text_command(const char *cmd)
{
    if (!s_cdc_dev) return -1;
    bool start = acq_intent_for_cmd(cmd, ACQ_INTENT_STOP) != ACQ_INTENT_STOP &&
                 strncmp(cmd, "-sta", 4) == 0;
    if (start) calib_autoread_before_start();   // #CAL-1
    int r = send_text_raw(cmd);
    if (r == 0) {
        DIAG_LOCK();
        s_diag.acq_intent = acq_intent_for_cmd(cmd, s_diag.acq_intent);   // #ACQ-1
        strncpy(s_diag.last_tx_cmd, cmd, sizeof(s_diag.last_tx_cmd) - 1);
        s_diag.last_tx_cmd[sizeof(s_diag.last_tx_cmd) - 1] = '\0';
        DIAG_UNLOCK();
        if (cmd_is_device_reset(cmd)) spectrum_base_clear();   // #SB-1: прибор обнулится — база тоже
    }
    return r;
}
