#include "boot_config.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "bootcfg";
static int8_t s_calib_al = -1;   // #CAL-1: -1 — ещё не читали из NVS
static int8_t s_mon_auto = -1;   // #MON-8: то же
#define BOOT_NS "boot"

static bool get_flag(nvs_handle_t h, const char *key)
{
    uint8_t v = 0;
    return (nvs_get_u8(h, key, &v) == ESP_OK) && (v != 0);
}

// #FW-42: санитизация префикса — только [A-Za-z0-9_-], усечение до cap-1.
// Гарантирует, что в NVS (и дальше в Content-Disposition) не попадёт мусор,
// ломающий заголовок или имя файла на ФС. dst всегда 0-терминируется.
static void sanitize_prefix(char *dst, size_t cap, const char *src)
{
    size_t j = 0;
    if (src) {
        for (size_t i = 0; src[i] && j + 1 < cap; i++) {
            char c = src[i];
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                (c >= '0' && c <= '9') || c == '_' || c == '-') {
                dst[j++] = c;
            }
        }
    }
    dst[j] = '\0';
}

void boot_config_load(boot_config_t *out)
{
    if (!out) return;
    out->autostart_spectrum  = false;
    out->autostart_waterfall = false;
    out->clear_spectrum      = false;
    out->clear_waterfall     = false;
    out->calib_always        = false;
    out->mon_autosave        = false;
    out->name_prefix[0]      = '\0';

    nvs_handle_t h;
    if (nvs_open(BOOT_NS, NVS_READONLY, &h) != ESP_OK) return;   // namespace ещё нет → все false / ""
    out->autostart_spectrum  = get_flag(h, "as_spec");
    out->autostart_waterfall = get_flag(h, "as_wf");
    out->clear_spectrum      = get_flag(h, "clr_spec");
    out->clear_waterfall     = get_flag(h, "clr_wf");
    out->calib_always        = get_flag(h, "calib_al");   // #CAL-1
    out->mon_autosave        = get_flag(h, "mon_auto");   // #MON-8
    size_t plen = sizeof(out->name_prefix);
    if (nvs_get_str(h, "nprefix", out->name_prefix, &plen) != ESP_OK) {
        out->name_prefix[0] = '\0';                             // ключа нет → без префикса
    }
    nvs_close(h);
}

int boot_config_save(const boot_config_t *in)
{
    if (!in) return -1;
    char clean[BOOT_NAME_PREFIX_CAP];
    sanitize_prefix(clean, sizeof(clean), in->name_prefix);
    nvs_handle_t h;
    if (nvs_open(BOOT_NS, NVS_READWRITE, &h) != ESP_OK) return -1;
    esp_err_t e = ESP_OK;
    e |= nvs_set_u8(h, "as_spec",  in->autostart_spectrum  ? 1 : 0);
    e |= nvs_set_u8(h, "as_wf",    in->autostart_waterfall ? 1 : 0);
    e |= nvs_set_u8(h, "clr_spec", in->clear_spectrum      ? 1 : 0);
    e |= nvs_set_u8(h, "clr_wf",   in->clear_waterfall     ? 1 : 0);
    e |= nvs_set_u8(h, "calib_al", in->calib_always        ? 1 : 0);   // #CAL-1
    e |= nvs_set_u8(h, "mon_auto", in->mon_autosave        ? 1 : 0);   // #MON-8
    e |= nvs_set_str(h, "nprefix", clean);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    if (e != ESP_OK) { ESP_LOGW(TAG, "save failed (0x%x)", (int)e); return -1; }
    s_calib_al = in->calib_always ? 1 : 0;   // #CAL-1: кэш
    s_mon_auto = in->mon_autosave ? 1 : 0;   // #MON-8: кэш
    return 0;
}

// #MON-8: кэш флага — читает sd_log при смене сессии (tft_task).
bool boot_config_mon_autosave(void)
{
    if (s_mon_auto < 0) {
        boot_config_t bc;
        boot_config_load(&bc);
        s_mon_auto = bc.mon_autosave ? 1 : 0;
    }
    return s_mon_auto == 1;
}

// #CAL-1: кэш флага — читается перед каждым стартом набора (usb_host_cdc).
bool boot_config_calib_always(void)
{
    if (s_calib_al < 0) {
        boot_config_t bc;
        boot_config_load(&bc);
        s_calib_al = bc.calib_always ? 1 : 0;
    }
    return s_calib_al == 1;
}

// #MON-2: часовой пояс экрана. Кэш: читают tft_task (раз в секунду) и httpd.
static int16_t s_tz_min;
static bool    s_tz_loaded;

int16_t boot_config_tz_min(void)
{
    if (!s_tz_loaded) {
        nvs_handle_t h;
        int16_t v = 0;
        if (nvs_open(BOOT_NS, NVS_READONLY, &h) == ESP_OK) {
            if (nvs_get_i16(h, "tz_min", &v) != ESP_OK) v = 0;
            nvs_close(h);
        }
        s_tz_min = v;
        s_tz_loaded = true;
    }
    return s_tz_min;
}

void boot_config_set_tz_min(int16_t m)
{
    if (m < BOOT_TZ_MIN_LO || m > BOOT_TZ_MIN_HI) return;
    if (m == boot_config_tz_min()) return;   // без лишних записей во флеш
    nvs_handle_t h;
    if (nvs_open(BOOT_NS, NVS_READWRITE, &h) != ESP_OK) return;
    esp_err_t e = nvs_set_i16(h, "tz_min", m);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    if (e != ESP_OK) { ESP_LOGW(TAG, "tz save failed (0x%x)", (int)e); return; }
    s_tz_min = m;
    ESP_LOGI(TAG, "tz set to %d min", m);
}
