#include "atomspectra.h"
#include "shproto.h"
#include "acq_intent.h"   // #ACQ-1/#SB-1: команды ПК-программы прибору
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include <string.h>
#include <errno.h>

static const char *TAG = "tcp_bridge";

static int s_client_fd = -1;
static int s_server_fd = -1;

// P2-4: s_client_fd трогают четыре задачи (usb_to_tcp_cb feed / tcp_tx_task
// send+close / tcp_rx_task recv+close / tcp_server_task accept). Мьютекс
// закрывает use-after-close; блокирующие recv()/send() делаем вне лока.
static SemaphoreHandle_t s_fd_mutex = NULL;
#define FD_LOCK()   do { if (s_fd_mutex) xSemaphoreTake(s_fd_mutex, portMAX_DELAY); } while (0)
#define FD_UNLOCK() do { if (s_fd_mutex) xSemaphoreGive(s_fd_mutex); } while (0)

// #TCP-1: декаплинг USB-RX ↔ TCP-TX через PSRAM-кольцо (StreamBuffer).
// Корень бага: раньше usb_to_tcp_cb звал send(fd,…,MSG_DONTWAIT) прямо из
// CDC-задачи и игнорировал возврат — при переполнении крошечного LWIP-буфера
// (CONFIG_LWIP_TCP_SND_BUF_DEFAULT) байты молча терялись посреди потока →
// рассинхрон sh_proto-фрейминга у клиента («битые пакеты», ~41% на 61 с).
// Наивный блокирующий send() из CDC-задачи нельзя: она же читает USB →
// застрянет USB → потеря уедет в device lostImp. Поэтому producer
// (usb_to_tcp_cb) НИКОГДА не блокирует — только кладёт в кольцо; consumer
// (tcp_tx_task) блокирующе шлёт в сокет с SO_SNDTIMEO + partial-loop.
#define TX_RING_BYTES   (256 * 1024)   // ~4.4 с потока @ 58 КБ/с — буфер на WiFi-джиттер
#define TX_RING_TRIGGER 1              // будить consumer как только есть ≥1 байт
#define TX_CHUNK_BYTES  4096           // сколько тянем из кольца за раз
#define TX_SNDTIMEO_MS  1000           // потолок блокировки send() на застрявшем клиенте

static StreamBufferHandle_t s_tx_ring = NULL;
static StaticStreamBuffer_t s_tx_ring_struct;   // во внутренней RAM (маленькая)
static uint8_t *s_tx_ring_storage = NULL;       // в PSRAM
static volatile uint32_t s_bridge_dropped = 0;  // байт потеряно (overflow кольца + send-timeout)

// producer: вызывается из CDC-задачи на каждый де-FTDI'нутый кусок. Не блокирует.
static void usb_to_tcp_cb(const uint8_t *data, size_t len)
{
    FD_LOCK();
    int fd = s_client_fd;
    FD_UNLOCK();
    if (fd < 0) return;                  // нет клиента — не копим зря

    if (s_tx_ring) {
        size_t put = xStreamBufferSend(s_tx_ring, data, len, 0);   // 0 = без блокировки
        if (put < len) s_bridge_dropped += (uint32_t)(len - put);  // кольцо переполнено
    } else {
        // фоллбэк, если PSRAM-кольцо не выделилось: деградированный прямой режим
        send(fd, data, len, MSG_DONTWAIT);
    }
}

// consumer: единственный, кто шлёт в TCP. Блокирующий send с таймаутом и partial-loop.
static void tcp_tx_task(void *arg)
{
    uint8_t *buf = heap_caps_malloc(TX_CHUNK_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) buf = malloc(TX_CHUNK_BYTES);
    if (!buf) {
        ESP_LOGE(TAG, "tx_task buffer alloc failed");
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        if (!s_tx_ring) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }

        size_t n = xStreamBufferReceive(s_tx_ring, buf, TX_CHUNK_BYTES, pdMS_TO_TICKS(200));
        if (n == 0) continue;            // таймаут, данных нет

        FD_LOCK();
        int fd = s_client_fd;
        FD_UNLOCK();
        if (fd < 0) continue;            // клиент ушёл — слитые байты выбрасываем

        size_t off = 0;
        while (off < n) {
            int w = send(fd, buf + off, n - off, 0);   // блокирующий (с SO_SNDTIMEO)
            if (w > 0) { off += (size_t)w; continue; }
            if (w < 0 && errno == EINTR) continue;
            if (w < 0 && (errno == EWOULDBLOCK || errno == EAGAIN)) {
                // send-timeout: клиент тормозит дольше TX_SNDTIMEO_MS. Бросаем
                // остаток куска (last-resort), считаем, не вешаем USB.
                s_bridge_dropped += (uint32_t)(n - off);
                break;
            }
            // реальная ошибка сокета → закрываем клиента
            ESP_LOGI(TAG, "tx send error, closing client (errno=%d)", errno);
            FD_LOCK();
            if (s_client_fd == fd) { close(fd); s_client_fd = -1; }
            FD_UNLOCK();
            break;
        }
    }
}

// #ACQ-1/#SB-1: команды ПК-программы (BecqMoni/AtomSpectra) идут прибору сырыми
// SHPROTO-пакетами. Прибором управляет она — сторож набора замолкает; её -rst
// обнуляет прибор — база спектра платы тоже (иначе после сброса в ПК-программе
// веб и экран показывали бы старый спектр + новый).
static uint8_t s_tx_parse_buf[300];
static shproto_struct s_tx_parse;

static void bridge_watch_tx(const uint8_t *d, int n)
{
    usb_host_cdc_acq_intent_external();
    for (int i = 0; i < n; i++) {
        shproto_byte_received(&s_tx_parse, d[i]);
        if (!s_tx_parse.ready) continue;
        s_tx_parse.ready = false;
        if (s_tx_parse.cmd == CMD_TEXT && s_tx_parse.len > 0) {
            size_t l = s_tx_parse.len < sizeof(s_tx_parse_buf) - 1 ? s_tx_parse.len : sizeof(s_tx_parse_buf) - 1;
            s_tx_parse.data[l] = 0;
            if (cmd_is_device_reset((const char *)s_tx_parse.data)) spectrum_base_clear();
        }
    }
}

static void tcp_rx_task(void *arg)
{
    uint8_t buf[1024];
    shproto_init(&s_tx_parse, s_tx_parse_buf, sizeof(s_tx_parse_buf));
    while (1) {
        FD_LOCK();
        int fd = s_client_fd;
        FD_UNLOCK();
        if (fd < 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        int n = recv(fd, buf, sizeof(buf), 0);   // блокирующий recv — вне лока
        if (n <= 0) {
            ESP_LOGI(TAG, "Client disconnected");
            FD_LOCK();
            if (s_client_fd == fd) { close(fd); s_client_fd = -1; }
            FD_UNLOCK();
            continue;
        }
        if (usb_host_cdc_send(buf, n) == 0) bridge_watch_tx(buf, n);
    }
}

static void tcp_server_task(void *arg)
{
    s_server_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s_server_fd < 0) {
        ESP_LOGE(TAG, "Socket create failed");
        vTaskDelete(NULL);
        return;
    }

    int opt = 1;
    if (setsockopt(s_server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
        ESP_LOGW(TAG, "SO_REUSEADDR failed: errno=%d", errno);

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(TCP_BRIDGE_PORT),
        .sin_addr.s_addr = INADDR_ANY
    };
    if (bind(s_server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "Bind port %d failed", TCP_BRIDGE_PORT);
        close(s_server_fd);
        vTaskDelete(NULL);
        return;
    }
    if (listen(s_server_fd, 1) < 0) {
        ESP_LOGE(TAG, "Listen on port %d failed: errno=%d", TCP_BRIDGE_PORT, errno);
        close(s_server_fd);
        s_server_fd = -1;
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "Listening on port %d", TCP_BRIDGE_PORT);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t clen = sizeof(client_addr);
        int fd = accept(s_server_fd, (struct sockaddr *)&client_addr, &clen);
        if (fd < 0) continue;

        FD_LOCK();
        bool busy = (s_client_fd >= 0);
        FD_UNLOCK();
        if (busy) {
            ESP_LOGW(TAG, "Rejecting second client");
            close(fd);
            continue;
        }

        int nodelay = 1;
        if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay)) < 0)
            ESP_LOGW(TAG, "TCP_NODELAY failed: errno=%d", errno);

        // #TCP-1: ограничиваем блокировку send() в tcp_tx_task, чтобы застрявший
        // клиент не держал задачу бесконечно (после таймаута бросаем остаток).
        struct timeval snd_to = { .tv_sec = TX_SNDTIMEO_MS / 1000,
                                  .tv_usec = (TX_SNDTIMEO_MS % 1000) * 1000 };
        if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &snd_to, sizeof(snd_to)) < 0)
            ESP_LOGW(TAG, "SO_SNDTIMEO failed: errno=%d", errno);

        // Сбрасываем кольцо от хвоста прошлой сессии ДО публикации fd: producer
        // ещё гейтится s_client_fd<0 и не пишет, так что reset без гонки. Если
        // tx_task сейчас заблокирован в receive (reset вернёт fail) — кольцо и
        // так пусто, новому клиенту хвост не уедет.
        if (s_tx_ring) xStreamBufferReset(s_tx_ring);

        FD_LOCK();
        s_client_fd = fd;
        FD_UNLOCK();
        ESP_LOGI(TAG, "Client connected from %s", inet_ntoa(client_addr.sin_addr));
    }
}

void tcp_bridge_init(void)
{
    s_fd_mutex = xSemaphoreCreateMutex();

    // PSRAM-кольцо + статическая управляющая структура (см. #TCP-1 выше).
    s_tx_ring_storage = heap_caps_malloc(TX_RING_BYTES + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_tx_ring_storage) {
        s_tx_ring = xStreamBufferCreateStatic(TX_RING_BYTES, TX_RING_TRIGGER,
                                              s_tx_ring_storage, &s_tx_ring_struct);
    }
    if (!s_tx_ring) {
        ESP_LOGE(TAG, "TX ring alloc failed — fallback to direct non-blocking send");
    } else {
        ESP_LOGI(TAG, "TX ring %d KB in PSRAM", TX_RING_BYTES / 1024);
    }

    usb_host_cdc_set_raw_rx_cb(usb_to_tcp_cb);
    // #TCP-2: пинимся на core 1. USB-задачи запинены на core 0 (usb_host_cdc.c:
    // usb_lib prio 7, usb_conn prio 2, cdc driver prio 8 — после #FW-8 CDC выше
    // tcp_tx(6), прямое вытеснение приёмника невозможно). Привязку сети к core 1
    // держим: USB-приём на core 0 не делит ядро с сетевыми burst'ами вовсе
    // (кэш/критические секции LWIP), независимо от раскладки приоритетов.
    xTaskCreatePinnedToCore(tcp_server_task, "tcp_srv", 4096, NULL, 5, NULL, 1);
    xTaskCreatePinnedToCore(tcp_tx_task,     "tcp_tx",  4096, NULL, 6, NULL, 1);
    xTaskCreatePinnedToCore(tcp_rx_task,     "tcp_rx",  4096, NULL, 5, NULL, 1);
    ESP_LOGI(TAG, "TCP bridge initialized, port %d (net tasks pinned core 1)", TCP_BRIDGE_PORT);
}

bool tcp_bridge_client_connected(void)
{
    return s_client_fd >= 0;
}

uint32_t tcp_bridge_dropped_bytes(void)
{
    return s_bridge_dropped;
}
