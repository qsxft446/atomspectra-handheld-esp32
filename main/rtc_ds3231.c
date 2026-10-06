// #RTC-1: DS3231 — см. rtc_ds3231.h. Регистры (даташит Maxim DS3231):
//   0x00..0x06 — сек, мин, час, день недели, число, месяц, год (BCD);
//   час: бит 6 = 12-часовой режим (пишем 24-часовой); месяц: бит 7 — век;
//   0x0E control: бит 7 EOSC — 1 = генератор СТОИТ на батарейке (сбрасываем);
//   0x0F status:  бит 7 OSF — генератор останавливался, время недостоверно.
#include "rtc_ds3231.h"
#include "board_i2c.h"
#include "net_time.h"

#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "rtc";

#define DS3231_ADDR        0x68
#define REG_TIME           0x00
#define REG_CONTROL        0x0E
#define REG_STATUS         0x0F
#define CTRL_EOSC          0x80
#define STAT_OSF           0x80
#define RTC_SANE_EPOCH     1700000000   // 2023-11-14: раньше — мусор, не время
#define RTC_RESYNC_US      (3600LL * 1000000)
#define RTC_RESYNC_MIN_S   2            // чтение с точностью до секунды — меньше не трогаем

static i2c_master_dev_handle_t s_dev;
static bool s_present;
static bool s_valid;

static esp_err_t rd(uint8_t reg, uint8_t *buf, size_t n)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, n, 100);
}

static esp_err_t wr(uint8_t reg, const uint8_t *buf, size_t n)
{
    uint8_t tmp[8];
    if (n > sizeof tmp - 1) return ESP_ERR_INVALID_SIZE;
    tmp[0] = reg;
    memcpy(tmp + 1, buf, n);
    return i2c_master_transmit(s_dev, tmp, n + 1, 100);
}

static uint8_t bcd2bin(uint8_t v) { return (uint8_t)((v >> 4) * 10 + (v & 0x0F)); }
static uint8_t bin2bcd(uint8_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

// Прочитать время DS3231 как Unix UTC. false — ошибка шины или мусор в регистрах.
static bool read_time(time_t *out)
{
    uint8_t r[7];
    if (rd(REG_TIME, r, sizeof r) != ESP_OK) return false;
    struct tm tm = {0};
    tm.tm_sec  = bcd2bin(r[0] & 0x7F);
    tm.tm_min  = bcd2bin(r[1] & 0x7F);
    if (r[2] & 0x40) {   // 12-часовой режим (кто-то выставлял модуль до нас)
        int h = bcd2bin(r[2] & 0x1F) % 12;
        tm.tm_hour = (r[2] & 0x20) ? h + 12 : h;
    } else {
        tm.tm_hour = bcd2bin(r[2] & 0x3F);
    }
    tm.tm_mday = bcd2bin(r[4] & 0x3F);
    tm.tm_mon  = bcd2bin(r[5] & 0x1F) - 1;
    tm.tm_year = 100 + bcd2bin(r[6]);   // 2000-2099; бит века не используем
    if (tm.tm_sec > 59 || tm.tm_min > 59 || tm.tm_hour > 23 ||
        tm.tm_mday < 1 || tm.tm_mday > 31 || tm.tm_mon < 0 || tm.tm_mon > 11)
        return false;
    // Модуль хранит UTC; TZ процесса не задан (UTC), mktime даёт Unix-время.
    time_t t = mktime(&tm);
    if (t < RTC_SANE_EPOCH) return false;
    *out = t;
    return true;
}

static bool write_time(time_t t)
{
    struct tm tm;
    gmtime_r(&t, &tm);
    if (tm.tm_year < 100 || tm.tm_year > 199) return false;   // DS3231 — только 2000-2099
    uint8_t r[7] = {
        bin2bcd((uint8_t)tm.tm_sec), bin2bcd((uint8_t)tm.tm_min), bin2bcd((uint8_t)tm.tm_hour),
        (uint8_t)(tm.tm_wday + 1), bin2bcd((uint8_t)tm.tm_mday), bin2bcd((uint8_t)(tm.tm_mon + 1)),
        bin2bcd((uint8_t)(tm.tm_year - 100)),
    };
    if (wr(REG_TIME, r, sizeof r) != ESP_OK) return false;
    // Время записано — сбросить OSF: теперь оно достоверно.
    uint8_t st;
    if (rd(REG_STATUS, &st, 1) != ESP_OK) return false;
    st &= (uint8_t)~STAT_OSF;
    return wr(REG_STATUS, &st, 1) == ESP_OK;
}

void rtc_ds3231_save_now(void)
{
    if (!s_present) return;
    time_t now = time(NULL);
    if (now < RTC_SANE_EPOCH) return;   // часы платы сами не выставлены — нечего хранить
    if (write_time(now)) {
        if (!s_valid) ESP_LOGI(TAG, "время записано в DS3231, теперь переживёт выключение");
        s_valid = true;
    } else {
        ESP_LOGW(TAG, "запись времени в DS3231 не удалась");
    }
}

// Раз в час: пока нет SNTP, часы ESP32 подправляются по DS3231. esp_timer —
// без своего стека во внутренней RAM; I2C-чтение 7 байт — около миллисекунды.
static void resync_cb(void *arg)
{
    (void)arg;
    if (!s_valid || net_time_sntp_synced()) return;
    time_t t;
    if (!read_time(&t)) return;
    struct timeval now;
    gettimeofday(&now, NULL);
    long long d = (long long)t - now.tv_sec;
    if (d >= -RTC_RESYNC_MIN_S && d <= RTC_RESYNC_MIN_S) return;
    struct timeval tv = { .tv_sec = t, .tv_usec = 0 };
    settimeofday(&tv, NULL);
    ESP_LOGI(TAG, "часы платы подправлены по DS3231 на %lld с", d);
}

void rtc_ds3231_init(void)
{
    i2c_master_bus_handle_t bus = board_i2c_get_bus();
    if (!bus) {
        ESP_LOGW(TAG, "шина I2C не поднята — DS3231 не используется");
        return;
    }
    if (i2c_master_probe(bus, DS3231_ADDR, 100) != ESP_OK) {
        ESP_LOGI(TAG, "DS3231 на шине не найден (0x68) — время только от SNTP/браузера");
        return;
    }
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = DS3231_ADDR,
        .scl_speed_hz    = 100000,
    };
    if (i2c_master_bus_add_device(bus, &cfg, &s_dev) != ESP_OK) {
        ESP_LOGW(TAG, "i2c_master_bus_add_device(DS3231) не удался");
        return;
    }
    s_present = true;

    // Генератор должен идти и от батарейки: EOSC = 0.
    uint8_t ctrl, st;
    if (rd(REG_CONTROL, &ctrl, 1) == ESP_OK && (ctrl & CTRL_EOSC)) {
        ctrl &= (uint8_t)~CTRL_EOSC;
        wr(REG_CONTROL, &ctrl, 1);
        ESP_LOGW(TAG, "DS3231: генератор был выключен на батарейке (EOSC) — включён");
    }
    if (rd(REG_STATUS, &st, 1) != ESP_OK) {
        ESP_LOGW(TAG, "DS3231: не читается статус");
        return;
    }
    time_t t;
    if (st & STAT_OSF) {
        ESP_LOGW(TAG, "DS3231: время недостоверно (OSF — пропадало питание/нет батарейки), "
                      "ждём браузер или SNTP");
    } else if (!read_time(&t)) {
        ESP_LOGW(TAG, "DS3231: в регистрах некорректное время, ждём браузер или SNTP");
    } else {
        struct timeval tv = { .tv_sec = t, .tv_usec = 0 };
        settimeofday(&tv, NULL);
        net_time_set_source(TIME_SRC_RTC);
        s_valid = true;
        struct tm tm;
        gmtime_r(&t, &tm);
        ESP_LOGI(TAG, "время платы взято из DS3231: %04d-%02d-%02d %02d:%02d:%02d UTC",
                 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    }

    const esp_timer_create_args_t ta = { .callback = resync_cb, .name = "rtc_resync" };
    esp_timer_handle_t h;
    if (esp_timer_create(&ta, &h) == ESP_OK) esp_timer_start_periodic(h, RTC_RESYNC_US);
}

bool rtc_ds3231_present(void)    { return s_present; }
bool rtc_ds3231_time_valid(void) { return s_valid; }
