#pragma once

#include "esp_err.h"   // #TFT-29: wifi_manager_set_boot_mode()
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#define SPECTRUM_CHANNELS     8192
#define SPECTRUM_CHUNK_BINS   64
#define SPECTRUM_CHUNKS       (SPECTRUM_CHANNELS / SPECTRUM_CHUNK_BINS)

#define ANALYZER_VID          0x0403
#define ANALYZER_PID          0x6001
#define ANALYZER_BAUD         600000

#define WIFI_SSID_MAX         32
#define WIFI_PASS_MAX         64

#define TCP_BRIDGE_PORT       8234

// #DEDUP-1: единая точка монтирования LittleFS. Раньше дублировалась локальными
// #define в spectrum.c / spectrogram.c / web_server.c.
#define STORAGE_PATH          "/storage"
// #FW-24: сохранённые спектры — в подкаталоге, отделены от calib/current/wf_state
// в корне mount (как /storage/wf у водопада). Все spec_NNNN.bin — здесь.
// (Первопричина пустого списка была НЕ в readdir корня — он исправен — а в
// несовпадении формата пути: save %04d vs list %d. Исправлено в handle_list.)
#define SPEC_DIR              STORAGE_PATH "/spec"

#define CMD_HISTOGRAM         0x01
#define CMD_OSCILLOSCOPE      0x02
#define CMD_TEXT              0x03
#define CMD_STAT              0x04
#define CMD_REBOOT            0xF3

#define CALIB_COEFFS          5

typedef struct {
    uint32_t bins[SPECTRUM_CHANNELS];
    uint32_t total_counts;
    uint32_t total_time_sec;
    uint16_t cpu_load;
    uint32_t cps;
    uint32_t lost_impulses;
    uint32_t pulse_width;   // #DT-4: суммарная ширина импульсов (отсчёты АЦП), STAT offset 14 — диагностика; в расчёт МВ не идёт (метод BecqMoni)
    float    temperature[3];
    double   calibration[CALIB_COEFFS];
    int      calib_order;
    char     serial_number[64];
    time_t   saved_at;
    bool     valid;
    bool     calib_valid;
} spectrum_data_t;

typedef struct {
    uint8_t  dev;
    uint8_t  version;
    uint16_t rise;
    uint16_t fall;
    uint16_t srise;
    uint16_t sfall;
    uint16_t noise;
    float    freq;
    uint32_t max_integral;
    uint16_t hyst;
    uint8_t  mode;
    uint8_t  step;
    uint32_t time_sec;
    uint8_t  pot;
    float    t1, t2, t3;
    bool     tc_on;
    uint16_t tp;
    bool     valid;
} device_info_t;

typedef void (*usb_raw_rx_cb_t)(const uint8_t *data, size_t len);

void usb_host_cdc_init(void);
bool usb_host_cdc_is_connected(void);
int  usb_host_cdc_send(const uint8_t *data, size_t len);
void usb_host_cdc_set_raw_rx_cb(usb_raw_rx_cb_t cb);
int  usb_host_send_text_command(const char *cmd);
// #FW-2/#FW-3: настройки автозапуска/очистки при старте платы. Вызвать ОДИН раз
// ДО usb_host_cdc_init() — флаги применяются однократно при первом USB-коннекте.
void usb_host_cdc_set_autostart(bool autostart_spectrum, bool autostart_waterfall, bool clear_spectrum);

// #FIELD-1: три сетевых режима работы платы.
typedef enum {
    NET_MODE_STA = 0,      // Indoor: клиент домашней сети (STA)
    NET_MODE_FIELD_AP,     // Outdoor: полевой AP, полный Web UI
    NET_MODE_SETUP,        // captive setup-портал (свежая плата без конфига)
} net_run_mode_t;

void wifi_manager_init(void);
bool wifi_is_connected(void);
net_run_mode_t wifi_manager_mode(void);   // #FIELD-1: текущий сетевой режим
bool wifi_manager_is_ap_mode(void);        // true для FIELD_AP или SETUP (совместимость)
int  wifi_manager_ap_clients(void);        // #FIELD-6: число STA-клиентов на AP (-1 если не AP)
const char *wifi_manager_ap_ssid(void);    // #FIELD-6: SSID активного AP ("" если STA)
bool wifi_manager_ap_pass_is_default(void);// #SEC-2: пароль AP не менялся (дефолт)
esp_err_t wifi_manager_set_boot_mode(bool outdoor);
// #WIFI-1: настройка WiFi с экрана платы (вкладка Conf).
typedef struct { char ssid[33]; int8_t rssi; bool secure; } wifi_net_t;
int  wifi_manager_scan(wifi_net_t *out, int max);              // сетей найдено, -1 ошибка; блокирует ~3 с
bool wifi_manager_sta_info(char *ssid, size_t cap, int *rssi); // текущая сеть (только Indoor с IP)
enum { WIFI_TRY_OK = 0, WIFI_TRY_SAVED_REBOOT, WIFI_TRY_BAD_PASS, WIFI_TRY_NO_AP, WIFI_TRY_FAILED, WIFI_TRY_BAD_ARG };
// Indoor: подключиться к новой сети, при успехе сохранить; при неудаче вернуть прежнюю.
// Точка доступа: сохранить + Indoor (перезагрузку делает вызывающий). Блокирует до timeout_ms.
int  wifi_manager_try_sta(const char *ssid, const char *pass, int timeout_ms);   // #TFT-29: indoor/outdoor на следующий старт (ребут — вызывающий)
bool wifi_manager_ap_forced(void);         // #FIELD-6: field_ap липкий (ap_mode=1) vs fallback
void web_server_init(void);

void tcp_bridge_init(void);
bool tcp_bridge_client_connected(void);
uint32_t tcp_bridge_dropped_bytes(void);
uint32_t usb_host_cdc_rx_errors(void);  // #TCP-4
uint32_t usb_host_cdc_rx_ring_drops(void);  // #BRIDGE-1: RX-кольцо переполнение
void usb_host_cdc_devlog_json(uint32_t since, char *out, size_t outsz);  // #UI-1

// #FW-22: глубокая диагностика USB Host. Снапшот всех счётчиков/last_* полей
// для endpoint /api/usb-diag. Все поля volatile-снимаются под mutex внутри.
// last_rx_first16 — hex 16 первых байт последнего RX-пакета (для анализа
// line-status/CMD-id при подозрении на не-FTDI устройство или битые кадры).
typedef struct {
    // Host stack
    bool     host_installed;
    bool     cdc_driver_installed;
    uint32_t last_host_event_flags;
    uint32_t host_event_ts_ms;
    // Enum
    uint32_t enum_cb_count;         // раз enum_filter_cb вызывался
    uint16_t last_seen_vid;
    uint16_t last_seen_pid;
    uint8_t  last_seen_class;
    uint8_t  last_seen_subclass;
    uint32_t last_seen_ts_ms;
    uint8_t  bus_devs_now;          // usb_host_device_addr_list_fill сейчас
    // CDC open
    uint32_t open_attempts;
    int32_t  last_open_errno;       // esp_err_t (0 = OK)
    uint32_t last_open_ts_ms;
    bool     cdc_open;              // s_cdc_dev != NULL
    // FTDI init (bitmask 6 бит: RESET/SETBAUD/SETDATA/SETFLOW/SETMODEM/RESET_END)
    uint8_t  ftdi_step_ok_mask;
    int32_t  ftdi_last_errno;
    // TX
    uint32_t tx_packets;
    uint32_t tx_bytes;
    uint32_t last_tx_ts_ms;
    int32_t  last_tx_errno;
    char     last_tx_cmd[32];       // "-inf" / "-sta" / "-cal" / "-tc_pot?"
    // RX
    uint32_t rx_cb_count;           // всего data_cb вызовов
    uint32_t rx_bytes;
    uint32_t rx_last_ts_ms;
    uint32_t rx_last_len;
    char     rx_last_first16_hex[48]; // hex-строка 16*2+15 разделителей+NUL
    uint32_t line_status_errors;    // #TCP-4 OE|PE|FE|FIFO
    // Parser
    uint32_t pkt_hist;
    uint32_t pkt_text;
    uint32_t pkt_stat;
    uint32_t pkt_osc;
    uint32_t pkt_unknown;
    uint32_t last_shproto_ts_ms;    // #FW-43: ts последнего CRC-валид SHPROTO-пакета (любой тип). Детект «определился, но не запитан».
    // Tasks
    uint32_t drv_task_alive_ts_ms;  // hint через RX cb
    uint32_t conn_task_alive_ts_ms; // отметка из usb_connect_task
    // DMA
    // #FW-51: закрытия соединения по причинам и последняя причина
    uint32_t cdc_error_count;
    uint32_t rx_watchdog_trips;
    uint32_t bus_empty_trips;
    uint8_t  last_fault_reason;     // 0 нет, 1 отключение, 2 ошибка CDC, 3 сторож RX, 4 пустая шина
    uint32_t last_fault_ts_ms;
    // #ACQ-1: сторож набора
    uint8_t  acq_intent;            // 0 неизвестно, 1 набор (плата слала -sta), 2 стоп
    uint32_t acq_resend_count;
    uint32_t last_acq_resend_ts_ms;
    uint32_t last_hist_ts_ms;       // последний пакет гистограммы
    uint32_t dma_free_largest;
    uint32_t dma_free_total;
    // Watermark
    uint32_t uptime_ms;
} usb_diag_snapshot_t;

void usb_host_cdc_diag_snapshot(usb_diag_snapshot_t *out);

// #FW-43: детект «прибор определился (FTDI '01 60' идёт), но не запитан». Первопричина
// (INC): hotplug в живой USB-хост не даёт физического фронта VBUS 0→5В на плате с жёстко
// разведённым VBUS → осн. домен спектрометра не стартует (лампа off, pkt_hist заморожен),
// а bus-powered FTDI всё равно энумерится и шлёт keep-alive. true = CDC открыт, FTDI-кадры
// свежие, но с момента открытия НИ одного SHPROTO-пакета (прибор молчит даже на -inf).
bool usb_host_cdc_spectrometer_dead(void);
void usb_host_cdc_acq_intent_external(void);       // #ACQ-1: команды от TCP-моста
void usb_host_cdc_acq_intent_device_reboot(void);  // #ACQ-1: перед перезагрузкой прибора
// #CAL-1: калибровка на плате задана (не нули); номер разобранного дампа -cal
bool spectrum_calib_is_set(void);
uint32_t spectrum_cal_dump_seq(void);

void spectrum_init(void);
void spectrum_process_histogram_chunk(const uint8_t *data, size_t len);
void spectrum_process_stat_packet(const uint8_t *data, size_t len);
// #FW-8: счётчики staging-сборки свипов гистограммы (полных commit / отброшенных
// рваных). drops растёт на каждом свипе, побитом flash-freeze finalize+create.
void spectrum_get_hist_stats(uint32_t *commits, uint32_t *drops);
// #FW-13 фикс №2: регистрация binary-семафора (FreeRTOS SemaphoreHandle_t как void*,
// чтобы не тянуть FreeRTOS в общий заголовок), отдаваемого на каждом коммите свипа —
// маркер начала тихого USB-окна для фазовой привязки flash-записей.
void spectrum_add_commit_listener(void *freertos_binary_sem);
void spectrum_process_info_response(const char *text);
// #DEV-6: ответ на -tc_pot? ("Tcpot [...]") — таблица баз. темп. компенсации,
// НЕ входит в -inf (см. #DOC-3/BUG-AS-08). Хранится сырым текстом для бэкапа.
void spectrum_process_tcpot_response(const char *text);
// Сырые тексты последних ответов -inf / -tc_pot? (байт-в-байт, формат как у
// автосейва MCA.exe) — для /api/settings/backup. *out_seq — монотонный счётчик,
// растёт при каждом новом ответе; используется вызывающим для ожидания свежего
// ответа после отправки команды (сравнить seq до/после).
int spectrum_get_info_raw(char *out, size_t outsz, uint32_t *out_seq);
int spectrum_get_tcpot_raw(char *out, size_t outsz, uint32_t *out_seq);
void spectrum_reset(void);
const spectrum_data_t *spectrum_get_current(void);
bool spectrum_get_snapshot(spectrum_data_t *out);
// #MON-1: атомарная пара (total_counts, total_time_sec) под коммит-локом —
// для монитора CPS; НЕ копирует 32 КБ bins (в отличие от spectrum_get_snapshot).
void spectrum_get_totals(uint32_t *counts, uint32_t *time_sec);
const device_info_t   *spectrum_get_device_info(void);
int  spectrum_save_to_flash(void);  // >=0 idx; -1 нет валидного спектра; -2 мало места; -3 ошибка FS (#FW-24); -4 нет SD-карты (#SD-5)
int  spectrum_load_from_flash(int index, spectrum_data_t *out);
int  spectrum_delete_from_flash(int index);
// #SD-4: сохранённые спектры — на SD-карте, если она есть (иначе флеш). Имена
// *_flash выше исторические: работают с каталогом spectrum_saved_dir().
const char *spectrum_saved_dir(void);
bool spectrum_saved_on_sd(void);
void spectrum_saved_migrate_to_sd(void);   // из app_main() после sd_card_init()
void spectrum_set_calibration(const double *coeffs, int order);
void spectrum_save_calibration(void);
void spectrum_load_calibration(void);
void spectrum_autosave(void);
void spectrum_restore_autosave(void);
// #SB-1: база спектра (спектр до последнего перезапуска прибора), см. spectrum.c.
void spectrum_base_clear(void);          // прибору ушёл сброс мимо spectrum_reset()
void spectrum_usb_session_start(void);   // CDC открыт — новый USB-сеанс
void spectrum_base_tick(void);           // main loop раз в 10 с: запись базы
void spectrum_get_base_info(uint32_t *counts, uint32_t *time_sec, uint32_t *folds);
