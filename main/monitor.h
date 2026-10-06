// #MON-1 (#FW-40): мониторинг CPS на плате — кольцо базовых 1с-сэмплов
// (дельты по коммитам свипов). Браузер больше НЕ копит историю сам (фоновая
// вкладка троттлит таймер до ~60 с → рваные Δt, закрытая — дыра в данных),
// а только дорисовывает серию через GET /api/monitor/series?since=<seq>.
//
// #MON-2: сессия мониторинга (Старт/Стоп/Очистить, интервал, окно SMA) тоже
// живёт на плате — общая для всех браузеров и экрана платы (tft_display.c).
// Раньше она была в localStorage каждого браузера, и экран считал свою.
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Базовый сэмпл серии: дельта одного (или нескольких слитых) коммитов свипа.
// 14 Б в упаковке — 21600 шт ≈ 295 КБ PSRAM на 6 ч при 1 Гц.
typedef struct __attribute__((packed)) {
    uint32_t end_sec;   // total_time_sec прибора на момент коммита (наборное время)
    uint32_t counts;    // прирост total_counts за dur
    uint32_t up_sec;    // #MON-2: аптайм платы на момент коммита, с (→ настенное время)
    uint16_t dur;       // прирост total_time_sec, с (обычно 1; больше при пропусках свипов)
} monitor_sample_t;

// #MON-2: сессия. Идентичность сессии — пара (epoch, rev): клиент, увидевший
// другую пару, выбрасывает свои интервалы и перестраивает их с start_seq.
// Стоп rev НЕ меняет — сессия та же, просто заморожена на stop_seq.
typedef struct {
    uint32_t rev;         // меняется на Старт / Очистить / смену эпохи
    uint32_t start_seq;   // seq первого сэмпла сессии; 0 — сессии нет
    uint32_t stop_seq;    // при !running: последний seq сессии (start_seq-1 — пустая)
    uint16_t interval_s;  // интервал усреднения, с
    uint16_t sma_win;     // окно SMA, интервалов
    bool     running;
} monitor_session_t;

#define MON_INTERVAL_MIN   1
#define MON_INTERVAL_MAX   86400   // как max у <input id="interval"> веба
#define MON_SMA_MIN        2
#define MON_SMA_MAX        200

// Поднять кольцо + задачу-подписчика на коммиты свипов. Вызывать из app_main()
// ПОСЛЕ spectrum_init() (spectrum_get_totals ходит под SPEC_LOCK).
void monitor_init(void);

// Снимок хвоста серии под внутренним локом (атомарно: сэмплы + курсоры + epoch
// + сессия одной точки времени). Копирует в out до max сэмплов с seq >= since+1
// (клампится к старейшему в кольце). Возвращает число скопированных.
// *out_first_seq — seq сэмпла out[0] (= *out_next_seq, если отдать нечего);
// *out_next_seq  — seq будущего сэмпла (последний записанный + 1);
// *out_epoch     — эпоха серии (меняется при откате счётчиков прибора и
//                  рандомизирована на старте платы — перезапуск виден клиенту);
// *out_sess      — сессия (NULL — не нужна).
size_t monitor_copy_since(uint32_t since, monitor_sample_t *out, size_t max,
                          uint32_t *out_first_seq, uint32_t *out_next_seq,
                          uint32_t *out_epoch, monitor_session_t *out_sess);

// #MON-2: настенное время (Unix, с) сэмпла по его up_sec — пересчёт текущим
// смещением часов платы, так что установка часов позже (браузер/SNTP) задним
// числом даёт верное время и уже накопленным сэмплам. 0 — часы не выставлены.
uint32_t monitor_wall_of(uint32_t up_sec);

// #MON-2: управление сессией (из веба и с экрана). Все меняют сессию атомарно
// под тем же локом, что и кольцо.
void monitor_session_start(uint16_t interval_s);   // interval_s 0 — оставить прежний
void monitor_session_stop(void);
void monitor_session_clear(void);
void monitor_session_set_sma(uint16_t win);
void monitor_session_get(monitor_session_t *out);
