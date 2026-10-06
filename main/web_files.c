// #FS-1: браузер файлов — см. web_files.h.
// Каталоги SD читаются через FatFs напрямую (f_readdir): он отдаёт размер и дату
// вместе с именем. Через VFS это был бы stat() на каждый файл, а stat в FAT —
// поиск по каталогу с начала: в /sdcard/wf за сутки записи ~700 сегментов, и
// листинг рос бы квадратично. Внутренняя RAM в дефиците (#TFT-17) — буферы
// ответов в PSRAM, имена в JSON без cJSON (тысячи мелких выделений).
#include "web_files.h"
#include "web_waterfall.h"   // web_csrf_check()
#include "sd_card.h"
#include "sd_log.h"          // sd_log_mon_file() — открытый файл мониторинга
#include "spectrogram.h"     // seg_open_index / seg_delete — сегменты водопада

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <inttypes.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "ff.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

static const char *TAG = "fs";

typedef struct {
    const char *id;
    const char *path;
} fs_root_t;

// Каталоги на карте — те же, куда пишут spectrum.c (#SD-4), sd_log.c (#SD-3),
// spectrogram.c (#SD-5). /sdcard/spectra (CSV-копии спектров) не показываем:
// их больше не пишут (#SD-7), спектр в CSV даёт «Export CSV». Папок флеша нет
// (#FS-2): пользовательские файлы во флеш не пишутся (#SD-5), а служебные
// (current.bin, calib.bin, wf_state.bin, dose_curve.csv) удалять нельзя.
static const fs_root_t ROOTS[] = {
    { "spec", SD_MOUNT_POINT "/spec"    },
    { "mon",  SD_MOUNT_POINT "/monitor" },
    { "wf",   SD_MOUNT_POINT "/wf"      },
};
#define N_ROOTS (sizeof ROOTS / sizeof ROOTS[0])

#define NAME_MAX_LEN  96
#define OUT_CAP       4096
#define DEL_BODY_MAX  (32 * 1024)

static const fs_root_t *root_by_id(const char *id)
{
    for (size_t i = 0; i < N_ROOTS; i++)
        if (strcmp(ROOTS[i].id, id) == 0) return &ROOTS[i];
    return NULL;
}

// Имя файла без каталогов: не пустое, без '/', '\\', управляющих, не "." / "..".
static bool name_ok(const char *n)
{
    size_t len = strlen(n);
    if (len == 0 || len > NAME_MAX_LEN) return false;
    if (strcmp(n, ".") == 0 || strcmp(n, "..") == 0) return false;
    for (const unsigned char *p = (const unsigned char *)n; *p; p++)
        if (*p < 0x20 || *p == '/' || *p == '\\' || *p == 0x7F) return false;
    return true;
}

// httpd_query_key_value() отдаёт значение как есть — %XX и '+' раскрываем сами.
static void url_decode(char *s)
{
    char *o = s;
    for (char *p = s; *p; p++) {
        if (*p == '%' && p[1] && p[2]) {
            char hx[3] = { p[1], p[2], 0 };
            char *end;
            long v = strtol(hx, &end, 16);
            if (*end == 0) { *o++ = (char)v; p += 2; continue; }
        }
        *o++ = (*p == '+') ? ' ' : *p;
    }
    *o = 0;
}

static bool query_arg(httpd_req_t *req, const char *key, char *out, size_t cap)
{
    char q[320];
    if (httpd_req_get_url_query_str(req, q, sizeof q) != ESP_OK) return false;
    if (httpd_query_key_value(q, key, out, cap) != ESP_OK) return false;
    url_decode(out);
    return true;
}

// root + name из запроса; при ошибке сам отвечает 400/404.
static const fs_root_t *req_root(httpd_req_t *req)
{
    char id[16];
    const fs_root_t *r = query_arg(req, "root", id, sizeof id) ? root_by_id(id) : NULL;
    if (!r) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad root");
        return NULL;
    }
    if (!sd_card_present()) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no SD card");
        return NULL;
    }
    return r;
}

static bool req_path(httpd_req_t *req, const fs_root_t **r, char *name, size_t ncap,
                     char *path, size_t pcap)
{
    if (!(*r = req_root(req))) return false;
    if (!query_arg(req, "name", name, ncap) || !name_ok(name)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad name");
        return false;
    }
    snprintf(path, pcap, "%s/%s", (*r)->path, name);
    return true;
}

// ---------------- обход каталога ----------------

typedef bool (*walk_cb_t)(void *ctx, const char *name, uint64_t size, uint32_t mtime);

// Дата FAT — время платы на момент записи; часы платы в UTC (TZ не задан,
// get_fattime() берёт localtime == UTC), поэтому считаем как UTC.
static uint32_t fat_epoch(WORD d, WORD t)
{
    if (!d) return 0;
    int y = (d >> 9) + 1980, m = (d >> 5) & 15, dd = d & 31;
    if (m < 1 || m > 12 || dd < 1) return 0;
    y -= m <= 2;   // days_from_civil (H. Hinnant)
    int era = y / 400, yoe = y - era * 400;
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + dd - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = (int64_t)era * 146097 + doe - 719468;
    return (uint32_t)(days * 86400 + (t >> 11) * 3600 + ((t >> 5) & 63) * 60 + (t & 31) * 2);
}

// Возвращает число файлов или -1, если каталога нет.
static int dir_walk(const fs_root_t *r, walk_cb_t cb, void *ctx)
{
    char ffp[48];
    if (!sd_card_ff_path(r->path, ffp, sizeof ffp)) return -1;
    FF_DIR *dir = heap_caps_malloc(sizeof *dir, MALLOC_CAP_SPIRAM);
    FILINFO *fi = heap_caps_malloc(sizeof *fi, MALLOC_CAP_SPIRAM);
    if (!dir || !fi || f_opendir(dir, ffp) != FR_OK) {
        free(dir);
        free(fi);
        return -1;
    }
    int n = 0;
    while (f_readdir(dir, fi) == FR_OK && fi->fname[0]) {
        if (fi->fattrib & AM_DIR) continue;
        n++;
        if (cb && !cb(ctx, fi->fname, (uint64_t)fi->fsize, fat_epoch(fi->fdate, fi->ftime))) break;
    }
    f_closedir(dir);
    free(dir);
    free(fi);
    return n;
}

// ---------------- выдача JSON кусками ----------------

typedef struct {
    httpd_req_t *req;
    char        *buf;
    int          pos;
    bool         first;
    bool         err;
    uint64_t     bytes;
} out_t;

static void out_flush(out_t *o)
{
    if (o->pos && !o->err && httpd_resp_send_chunk(o->req, o->buf, o->pos) != ESP_OK) o->err = true;
    o->pos = 0;
}

static void out_str(out_t *o, const char *s)
{
    size_t len = strlen(s);
    if (o->pos + len > OUT_CAP - 8) out_flush(o);
    if (len > OUT_CAP - 8) return;
    memcpy(o->buf + o->pos, s, len);
    o->pos += len;
}

// Строка JSON: кавычки и '\\' экранируются, управляющие отброшены (name_ok их не пропустит).
static void out_jstr(out_t *o, const char *s)
{
    char tmp[2 * NAME_MAX_LEN + 8];
    size_t k = 0;
    tmp[k++] = '"';
    for (; *s && k < sizeof tmp - 3; s++) {
        if ((unsigned char)*s < 0x20) continue;
        if (*s == '"' || *s == '\\') tmp[k++] = '\\';
        tmp[k++] = *s;
    }
    tmp[k++] = '"';
    tmp[k] = 0;
    out_str(o, tmp);
}

static bool list_cb(void *ctx, const char *name, uint64_t size, uint32_t mtime)
{
    out_t *o = ctx;
    if (strlen(name) > NAME_MAX_LEN) return true;   // такое имя всё равно не адресуется
    out_str(o, o->first ? "[" : ",[");
    o->first = false;
    out_jstr(o, name);
    char num[48];
    snprintf(num, sizeof num, ",%llu,%" PRIu32 "]", (unsigned long long)size, mtime);
    out_str(o, num);
    return !o->err;
}

static bool count_cb(void *ctx, const char *name, uint64_t size, uint32_t mtime)
{
    (void)name; (void)mtime;
    ((out_t *)ctx)->bytes += size;
    return true;
}

// Имя файла, который сейчас пишется в этой папке ("" — нет): удалять нельзя.
static void busy_name(const fs_root_t *r, char *out, size_t cap)
{
    out[0] = 0;
    if (strcmp(r->id, "wf") == 0) {
        uint32_t idx = spectrogram_seg_open_index();
        if (idx != 0xFFFFFFFFu) snprintf(out, cap, "seg_%05" PRIu32 ".aswf", idx);
    } else if (strcmp(r->id, "mon") == 0) {
        sd_log_mon_file(out, cap);
    }
}

static esp_err_t h_roots(httpd_req_t *req)
{
    out_t o = { .req = req, .buf = heap_caps_malloc(OUT_CAP, MALLOC_CAP_SPIRAM), .first = true };
    if (!o.buf) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom"); return ESP_FAIL; }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    uint64_t sd_total = 0, sd_free = 0;
    bool sd = sd_card_space(&sd_total, &sd_free);
    char tmp[200];
    snprintf(tmp, sizeof tmp, "{\"sd\":%s,\"sd_total\":%llu,\"sd_free\":%llu,\"roots\":[",
             sd ? "true" : "false", (unsigned long long)sd_total, (unsigned long long)sd_free);
    out_str(&o, tmp);
    for (size_t i = 0; i < N_ROOTS; i++) {
        o.bytes = 0;
        int n = sd ? dir_walk(&ROOTS[i], count_cb, &o) : -1;
        snprintf(tmp, sizeof tmp, "%s{\"id\":\"%s\",\"exists\":%s,\"n\":%d,\"bytes\":%llu}",
                 i ? "," : "", ROOTS[i].id, n >= 0 ? "true" : "false",
                 n < 0 ? 0 : n, (unsigned long long)o.bytes);
        out_str(&o, tmp);
    }
    out_str(&o, "]}");
    out_flush(&o);
    free(o.buf);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t h_list(httpd_req_t *req)
{
    const fs_root_t *r = req_root(req);
    if (!r) return ESP_FAIL;
    out_t o = { .req = req, .buf = heap_caps_malloc(OUT_CAP, MALLOC_CAP_SPIRAM), .first = true };
    if (!o.buf) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom"); return ESP_FAIL; }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    char busy[NAME_MAX_LEN + 1];
    busy_name(r, busy, sizeof busy);
    out_str(&o, "{\"root\":");
    out_jstr(&o, r->id);
    out_str(&o, ",\"busy\":");
    out_jstr(&o, busy);
    out_str(&o, ",\"files\":[");
    int n = dir_walk(r, list_cb, &o);
    out_str(&o, n < 0 ? "],\"exists\":false}" : "],\"exists\":true}");
    out_flush(&o);
    free(o.buf);
    if (o.err) return ESP_FAIL;
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static const char *mime_of(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (dot && (strcasecmp(dot, ".csv") == 0 || strcasecmp(dot, ".txt") == 0)) return "text/plain; charset=utf-8";
    if (dot && strcasecmp(dot, ".json") == 0) return "application/json";
    return "application/octet-stream";
}

static esp_err_t h_file(httpd_req_t *req)
{
    const fs_root_t *r;
    char name[NAME_MAX_LEN + 1], path[200], arg[16];
    if (!req_path(req, &r, name, sizeof name, path, sizeof path)) return ESP_FAIL;
    long head = query_arg(req, "head", arg, sizeof arg) ? atol(arg) : 0;
    bool dl = query_arg(req, "dl", arg, sizeof arg) && arg[0] == '1';
    // read(), не stdio: буфер stdio newlib взял бы внутреннюю RAM, а fread без
    // буфера (_IONBF) читает по байту — замер: 33-42 КБ/с и с карты, и с флеша.
    int fd = open(path, O_RDONLY);
    if (fd < 0) { httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found"); return ESP_FAIL; }
    char *buf = heap_caps_malloc(OUT_CAP, MALLOC_CAP_SPIRAM);
    if (!buf) { close(fd); httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom"); return ESP_FAIL; }
    struct stat st;
    char len[24];
    if (stat(path, &st) == 0) {   // полный размер — браузеру для прогресса
        snprintf(len, sizeof len, "%ld", (long)st.st_size);
        httpd_resp_set_hdr(req, "X-File-Size", len);
    }
    httpd_resp_set_type(req, mime_of(name));
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    char disp[NAME_MAX_LEN + 40];
    if (dl) {
        snprintf(disp, sizeof disp, "attachment; filename=\"%s\"", name);
        httpd_resp_set_hdr(req, "Content-Disposition", disp);
    }
    long left = head > 0 ? head : -1;
    ssize_t rd;
    esp_err_t ret = ESP_OK;
    while (left != 0 && (rd = read(fd, buf, left > 0 && left < OUT_CAP ? (size_t)left : OUT_CAP)) > 0) {
        if (httpd_resp_send_chunk(req, buf, rd) != ESP_OK) { ret = ESP_FAIL; break; }
        if (left > 0) left -= (long)rd;
    }
    close(fd);
    free(buf);
    if (ret == ESP_OK) httpd_resp_send_chunk(req, NULL, 0);
    return ret;
}

static esp_err_t h_spectrum(httpd_req_t *req)
{
    const fs_root_t *r;
    char name[NAME_MAX_LEN + 1], path[200], fmt[8] = "json";
    if (!req_path(req, &r, name, sizeof name, path, sizeof path)) return ESP_FAIL;
    query_arg(req, "fmt", fmt, sizeof fmt);
    struct stat st;
    // Сырой spectrum_data_t (spectrum_save_to_flash): размер должен совпасть до байта,
    // иначе это файл другой сборки/формата — не гадаем о раскладке.
    if (stat(path, &st) != 0 || st.st_size != (off_t)sizeof(spectrum_data_t)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "not a spectrum file");
        return ESP_FAIL;
    }
    spectrum_data_t *sp = heap_caps_malloc(sizeof *sp, MALLOC_CAP_SPIRAM);
    int fd = sp ? open(path, O_RDONLY) : -1;
    bool ok = fd >= 0 && read(fd, sp, sizeof *sp) == (ssize_t)sizeof *sp;
    if (fd >= 0) close(fd);
    if (!ok) {
        free(sp);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "read failed");
        return ESP_FAIL;
    }
    int idx = -1;
    char fn[NAME_MAX_LEN + 8];
    if (sscanf(name, "spec_%d.bin", &idx) == 1 && idx >= 0)
        snprintf(fn, sizeof fn, "spectrum_%04d.%s", idx, fmt);   // как /api/saved/N/export.*
    else
        snprintf(fn, sizeof fn, "%.*s.%s", NAME_MAX_LEN - 4, name, fmt);
    esp_err_t ret = web_render_spectrum(req, sp, fmt, fn);
    free(sp);
    return ret;
}

// Удаление одного файла; NULL — удалён, иначе код причины для браузера.
static const char *del_one(const fs_root_t *r, const char *name, const char *busy)
{
    if (!name_ok(name)) return "badname";
    if (busy[0] && strcmp(name, busy) == 0) return "busy";
    char path[200];
    snprintf(path, sizeof path, "%s/%s", r->path, name);
    if (strcmp(r->id, "wf") == 0) {
        // Сегмент водопада — через spectrogram.c: он ведёт счётчик сегментов и не
        // даст снести открытый или выгружаемый (#REC-11-A2) сегмент.
        uint32_t idx;
        char tail[8];
        if (sscanf(name, "seg_%" SCNu32 "%7s", &idx, tail) == 2 && strcmp(tail, ".aswf") == 0) {
            if (spectrogram_seg_delete(idx)) return NULL;
            struct stat st;
            return stat(path, &st) == 0 ? "busy" : "notfound";
        }
    }
    if (unlink(path) == 0) return NULL;
    struct stat st;
    return stat(path, &st) == 0 ? "io" : "notfound";
}

static esp_err_t h_delete(httpd_req_t *req)
{
    if (!web_csrf_check(req)) return ESP_FAIL;
    const fs_root_t *r = req_root(req);
    if (!r) return ESP_FAIL;
    if (req->content_len <= 0 || req->content_len > DEL_BODY_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_FAIL;
    }
    char *body = heap_caps_malloc(req->content_len + 1, MALLOC_CAP_SPIRAM);
    out_t o = { .req = req, .buf = heap_caps_malloc(OUT_CAP, MALLOC_CAP_SPIRAM), .first = true };
    if (!body || !o.buf) {
        free(body);
        free(o.buf);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }
    int got = 0;
    while (got < (int)req->content_len) {
        int n = httpd_req_recv(req, body + got, req->content_len - got);
        if (n <= 0) {
            free(body);
            free(o.buf);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv");
            return ESP_FAIL;
        }
        got += n;
    }
    body[got] = 0;
    char busy[NAME_MAX_LEN + 1];
    busy_name(r, busy, sizeof busy);
    httpd_resp_set_type(req, "application/json");
    out_str(&o, "{\"fail\":[");
    int ok = 0, fail = 0;
    char *save = NULL;
    for (char *nm = strtok_r(body, "\n", &save); nm; nm = strtok_r(NULL, "\n", &save)) {
        size_t l = strlen(nm);
        if (l && nm[l - 1] == '\r') nm[--l] = 0;
        if (!l) continue;
        const char *why = del_one(r, nm, busy);
        if (!why) { ok++; continue; }
        out_str(&o, fail++ ? ",[" : "[");
        out_jstr(&o, nm);
        out_str(&o, ",");
        out_jstr(&o, why);
        out_str(&o, "]");
    }
    char tail[40];
    snprintf(tail, sizeof tail, "],\"ok\":%d}", ok);
    out_str(&o, tail);
    out_flush(&o);
    ESP_LOGI(TAG, "%s: удалено %d, отказ %d", r->path, ok, fail);
    free(body);
    free(o.buf);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

void web_files_register(httpd_handle_t server)
{
    static const httpd_uri_t uris[] = {
        { .uri = "/api/fs/roots",    .method = HTTP_GET,  .handler = h_roots    },
        { .uri = "/api/fs/list",     .method = HTTP_GET,  .handler = h_list     },
        { .uri = "/api/fs/file",     .method = HTTP_GET,  .handler = h_file     },
        { .uri = "/api/fs/spectrum", .method = HTTP_GET,  .handler = h_spectrum },
        { .uri = "/api/fs/delete",   .method = HTTP_POST, .handler = h_delete   },
    };
    for (size_t i = 0; i < sizeof uris / sizeof uris[0]; i++)
        httpd_register_uri_handler(server, &uris[i]);
}
