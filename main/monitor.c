// #MON-1 (#FW-40): сбор серии CPS на плате вместо браузера.
//
// Проблема: web/monitor.html копил дельты по live-poll /api/spectrum.json в JS.
// Вкладка в фоне → браузер троттлит setInterval до ~60 с (интервалы скачут,
// диагонали в графике), вкладка закрыта → дыра в данных.
//
// Решение: monitor_task подписан на коммиты свипов (spectrum_add_commit_listener,
// как wf_task/autosave) и на каждом коммите снимает атомарную пару
// (total_counts, total_time_sec) через spectrum_get_totals() — дёшево, без
// 32 КБ snapshot. Дельта против прошлого замера кладётся базовым сэмплом в
// кольцо PSRAM (6 ч при 1 Гц); httpd отдаёт хвост серии по seq
// (GET /api/monitor/series?since=<seq>, web_server.c). Точность не зависит от
// фазы задачи: dur считается по НАБОРНОМУ времени прибора — проспали два
// коммита → один честный сэмпл dur=2, а не искажение.
//
// #MON-2: здесь же — сессия мониторинга (Старт/Стоп/Очистить, интервал, SMA),
// общая для всех браузеров и экрана платы, и аптайм каждого сэмпла, из которого
// все клиенты получают одно и то же настенное время (monitor_wall_of).

#include "monitor.h"
#include "atomspectra.h"

#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <time.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "monitor";

#define MON_RING_PSRAM     21600   // 6 ч при 1 Гц: 21600 × 14 Б ≈ 295 КБ PSRAM
#define MON_RING_INTERNAL   3600   // fallback без PSRAM: 1 ч ≈ 49 КБ internal
#define MON_SANE_EPOCH  1700000000 // 2023-11-14: раньше — часы платы не выставлены
#define ACQ_IDLE_US     5000000LL  // #MON-9: время прибора стоит дольше — набор остановлен (свип раз в 1 с)

static monitor_sample_t *s_ring;
static uint32_t s_cap;       // фактическая ёмкость кольца (см. monitor_init)
static uint32_t s_head;      // индекс, куда ляжет следующий сэмпл
static uint32_t s_count;     // занято сэмплов (<= s_cap)
static uint32_t s_last_seq;  // seq последнего записанного (0 = ещё ничего → первый сэмпл seq=1)
static uint32_t s_epoch;     // эпоха серии: случайная на старте платы (#MON-7: сброс спектра её не меняет)
static monitor_session_t s_sess;   // #MON-2: под s_lock, как и кольцо

// Писатель — monitor_task (1 сэмпл/с), читатель — httpd-воркер. Лок держится
// микросекунды (запись 10 Б) / доли мс (копирование чанка <= 20 КБ).
static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_commit_sig;   // отдаёт spectrum.c на каждом коммите свипа

static uint32_t uptime_sec(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000);
}

uint32_t monitor_wall_of(uint32_t up_sec)
{
    int64_t now = (int64_t)time(NULL);
    if (now < MON_SANE_EPOCH) return 0;
    int64_t w = now - (int64_t)uptime_sec() + up_sec;
    return w > 0 ? (uint32_t)w : 0;
}

static void ring_push(uint32_t end_sec, uint32_t dcounts, uint16_t dur)
{
    uint32_t up = uptime_sec();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_ring[s_head].end_sec = end_sec;
    s_ring[s_head].counts  = dcounts;
    s_ring[s_head].up_sec  = up;
    s_ring[s_head].dur     = dur;
    s_head = (s_head + 1) % s_cap;
    if (s_count < s_cap) s_count++;   // при переполнении старые вытесняются, seq растёт дальше
    s_last_seq++;
    xSemaphoreGive(s_lock);
}


size_t monitor_copy_since(uint32_t since, monitor_sample_t *out, size_t max,
                          uint32_t *out_first_seq, uint32_t *out_next_seq,
                          uint32_t *out_epoch, monitor_session_t *out_sess)
{
    if (!s_lock || !s_ring) {   // монитор отключён (alloc failed) — пустая серия
        if (out_first_seq) *out_first_seq = 1;
        if (out_next_seq)  *out_next_seq  = 1;
        if (out_epoch)     *out_epoch     = 0;
        if (out_sess)      *out_sess      = s_sess;
        return 0;
    }
    size_t n = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t next  = s_last_seq + 1;
    uint32_t first = next;                        // «отдать нечего» по умолчанию
    uint32_t epoch = s_epoch;                     // строго под локом — когерентно с сэмплами
    if (out_sess) *out_sess = s_sess;
    if (s_count > 0) {
        uint32_t oldest = s_last_seq - s_count + 1;
        uint64_t from   = (uint64_t)since + 1;    // uint64 — нет wrap на since=UINT32_MAX
        if (from < oldest) from = oldest;
        if (from <= s_last_seq) {
            size_t avail = (size_t)(s_last_seq - (uint32_t)from) + 1;
            n = (avail > max) ? max : avail;
            first = (uint32_t)from;
            for (size_t i = 0; i < n; i++) {
                uint32_t seq = first + (uint32_t)i;
                uint32_t idx = (s_head + s_cap - (s_last_seq - seq + 1)) % s_cap;
                out[i] = s_ring[idx];
            }
        }
    }
    xSemaphoreGive(s_lock);
    if (out_first_seq) *out_first_seq = first;
    if (out_next_seq)  *out_next_seq  = next;
    if (out_epoch)     *out_epoch     = epoch;
    return n;
}

// ---------------- #MON-2: сессия ----------------

static bool sess_lock(void)
{
    if (!s_lock) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    return true;
}

void monitor_session_start(uint16_t interval_s)
{
    if (!sess_lock()) return;
    if (interval_s >= MON_INTERVAL_MIN) s_sess.interval_s = interval_s;
    s_sess.running   = true;
    s_sess.start_seq = s_last_seq + 1;   // сессия — сэмплы с этого момента
    s_sess.stop_seq  = 0;
    s_sess.rev++;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "session start: seq %" PRIu32 ", interval %u s", s_sess.start_seq, s_sess.interval_s);
}

void monitor_session_stop(void)
{
    if (!sess_lock()) return;
    if (s_sess.running) {
        s_sess.running  = false;
        s_sess.stop_seq = s_last_seq;    // start_seq-1, если сэмплов ещё не было
    }
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "session stop: seq %" PRIu32, s_sess.stop_seq);
}

void monitor_session_clear(void)
{
    if (!sess_lock()) return;
    if (s_sess.running) {
        s_sess.start_seq = s_last_seq + 1;   // запись продолжается с чистого листа
    } else {
        s_sess.start_seq = 0;
        s_sess.stop_seq  = 0;
    }
    s_sess.rev++;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "session clear");
}

void monitor_session_set_sma(uint16_t win)
{
    if (win < MON_SMA_MIN) win = MON_SMA_MIN;
    if (win > MON_SMA_MAX) win = MON_SMA_MAX;
    if (!sess_lock()) return;
    s_sess.sma_win = win;
    xSemaphoreGive(s_lock);
}

void monitor_session_get(monitor_session_t *out)
{
    if (!sess_lock()) {
        *out = s_sess;
        return;
    }
    *out = s_sess;
    xSemaphoreGive(s_lock);
}

static void monitor_task(void *arg)
{
    (void)arg;
    uint32_t prev_counts = 0, prev_time = 0;
    uint32_t t_off = 0;   // #MON-7: end_sec ряда = наборное время + t_off (непрерывно через сбросы)
    bool prev_valid = false;
    // #MON-9 (запрос 2026-10-17): мониторинг стартует сам вместе с набором спектра. «Набор
    // начался» = время прибора пошло после паузы > ACQ_IDLE_US (или впервые после старта
    // платы) — так ловится старт откуда угодно: веб, экран, программа на ПК, кнопка прибора,
    // автостарт. Идущую сессию не трогаем; остановленную Старт заменяет новой, как кнопка.
    // Стоп набора мониторинг не останавливает: сессия ждёт и продолжится со следующим свипом.
    bool acq_on = false;
    int64_t last_adv_us = 0;
    for (;;) {
        // Будимся коммитом свипа (паттерн wf_task); 1500 мс — fallback-тик при
        // молчащем приборе (тогда totals не растут и сэмпл не эмитится).
        if (s_commit_sig) xSemaphoreTake(s_commit_sig, pdMS_TO_TICKS(1500));
        else vTaskDelay(pdMS_TO_TICKS(1000));

        uint32_t counts = 0, tsec = 0;
        spectrum_get_totals(&counts, &tsec);
        int64_t now_us = esp_timer_get_time();
        if (acq_on && now_us - last_adv_us > ACQ_IDLE_US) acq_on = false;   // набор встал

        if (!prev_valid) {
            // Первый замер (в т.ч. restored-autosave) — только опорная точка:
            // иначе первый сэмпл получил бы весь накопленный столбец разом.
            prev_counts = counts; prev_time = tsec; prev_valid = true;
            continue;
        }
        if (counts < prev_counts || tsec < prev_time) {
            // Откат счётчиков = сброс спектра (-rst, Clear, свайп экрана) или рестарт
            // прибора. #MON-7 (запрос 2026-10-13): мониторинг это НЕ сбрасывает — ряд
            // CPS и сессия идут дальше. Раньше была новая эпоха: кольцо очищалось,
            // сессия начиналась заново, и Clear спектра стирал график мониторинга.
            // Свип сброса пропускаем (приращения не узнать), наборное время ряда
            // продолжается с прежнего места — end_sec монотонен для веба и экрана.
            t_off = prev_time + t_off - tsec;
            ESP_LOGI(TAG, "counters rollback: series continues (time offset %" PRIu32 " s)", t_off);
            prev_counts = counts; prev_time = tsec;
            continue;
        }
        uint32_t dur = tsec - prev_time;
        if (dur == 0) continue;              // свип без прироста времени — ждём дальше
        if (dur > UINT16_MAX) {
            // Скачок крупнее формата сэмпла (>18 ч) — не данные, тихий ресинк.
            ESP_LOGW(TAG, "time jump %" PRIu32 "s > sample fmt, resync", dur);
            prev_counts = counts; prev_time = tsec;
            continue;
        }
        if (!acq_on) {   // #MON-9: набор пошёл — сессия с этого сэмпла (start_seq = его seq)
            monitor_session_t ss;
            monitor_session_get(&ss);
            if (!ss.running) {
                ESP_LOGI(TAG, "acquisition started -> monitoring session auto-start");
                monitor_session_start(0);   // интервал — прежний
            }
            acq_on = true;
        }
        last_adv_us = now_us;
        ring_push(tsec + t_off, counts - prev_counts, (uint16_t)dur);
        prev_counts = counts; prev_time = tsec;
    }
}

void monitor_init(void)
{
    // #MON-2: дефолты веба (#interval, #smaWin); rev случаен, как и эпоха.
    // #MON-3: интервал усреднения по умолчанию 1 с (было 10, запрос 2026-09-29).
    s_sess = (monitor_session_t){ .interval_s = 1, .sma_win = 10, .rev = esp_random() & 0xFFFF };
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        ESP_LOGE(TAG, "lock create failed - monitor disabled");
        return;
    }

    s_cap  = MON_RING_PSRAM;
    s_ring = heap_caps_malloc(s_cap * sizeof(monitor_sample_t), MALLOC_CAP_SPIRAM);
    if (!s_ring) {
        // Fallback: internal RAM, час истории — лучше, чем ничего.
        s_cap  = MON_RING_INTERNAL;
        s_ring = malloc(s_cap * sizeof(monitor_sample_t));
        if (s_ring)
            ESP_LOGW(TAG, "PSRAM alloc failed - internal ring %" PRIu32 " samples (1h)", s_cap);
    }
    if (!s_ring) {
        ESP_LOGE(TAG, "ring alloc failed - monitor disabled");
        return;
    }

    // Эпоха рандомизируется на старте: перезапуск платы (кольцо потеряно)
    // выглядит для клиента как смена эпохи → чистый ресинк, а не тихая дыра.
    s_epoch = esp_random() & 0x3FFFFFFF;

    s_commit_sig = xSemaphoreCreateBinary();
    if (s_commit_sig) spectrum_add_commit_listener(s_commit_sig);

    // Core 1 (как wf_task) — подальше от USB-приёма на core 0. Prio 2 хватает:
    // dur считается по времени прибора, опоздание задачи данные не искажает.
    // #RAM-1: стек в PSRAM — задача читает счётчики и пишет кольцо в PSRAM, во флеш
    // не пишет. Внутренней RAM впритык (#TFT-17, #SD-3).
    xTaskCreatePinnedToCoreWithCaps(monitor_task, "mon", 4096, NULL, 2, NULL, 1,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "ring %" PRIu32 " samples (%" PRIu32 " KB, %s), epoch %" PRIu32,
             s_cap, (uint32_t)(s_cap * sizeof(monitor_sample_t) / 1024),
             s_cap == MON_RING_PSRAM ? "PSRAM" : "internal", s_epoch);
}
