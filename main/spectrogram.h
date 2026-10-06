#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <time.h>

// Водопад (спектрограмма): каждая строка — дельта накопительного спектра за
// интервал, uint16 на канал. Источник спектра — модуль spectrum (cmd 0x01).

#define WF_CHANNELS           8192            // = SPECTRUM_CHANNELS
#define WF_ROW_BYTES          (WF_CHANNELS * 2)
#define WF_RING_ROWS_DEFAULT  256             // окно в PSRAM (256 * 16 КБ = 4 МБ)
// #WF-3: 2 с — запрос пользователя 2026-09-29 (обновление водопада и в вебе, и на
// экране платы раз в 2 с). Было 5/5. Строка закрывается по времени ПРИБОРА
// (#FW-10), а он коммитит свип раз в секунду — 2 с укладываются. С записью во
// Flash (persist) место расходуется в 2.5 раза быстрее, чем при 5 с.
#define WF_INTERVAL_DEFAULT   2               // сек между строками
#define WF_INTERVAL_MIN       2               // нижняя граница (UI: 2 с)
// Верхняя граница 3600 с (1 ч): автономная многосуточная запись на Flash.
// Ёмкость storage ≈ 763 строки → при 10-мин интервале ≈ 5.3 сут, при 1-ч ≈ 31 сут.
#define WF_INTERVAL_MAX       3600            // верхняя граница (UI: до 60 мин)

// #REC-11-A1: запись сегментами /storage/wf/seg_NNNNN.aswf (каждый — валидный .aswf).
// Сегмент = единица отправки (фаза A2/B), единица кольца keep-last и независимый файл
// для склейки в браузере. Открытый сегмент финализируется по достижении MAX_ROWS
// (~1 МБ) ИЛИ MAX_AGE (чтобы при больших интервалах не висел открытым часами).
#define WF_SEG_MAX_ROWS       64              // 64 * 16 КБ = 1 МБ payload на сегмент
#define WF_SEG_MAX_AGE_SEC    600             // финализировать открытый сегмент не реже 10 мин
#define WF_FSYNC_BATCH        4               // #WF-1: fsync раз в N строк снижает hist_drop (SPI); fflush каждой строки; seg_finalize() всегда syncs
#define WF_HDR_RESERVE        4096            // .aswf JSON-заголовок (добивается пробелами)
#define WF_SEG_HEADER         (8 + WF_HDR_RESERVE)  // offset payload в сегменте (= 4104)

// Формат сегмента v5 (ASWF v5) — #DATA-1 целостность данных, #FW-41 temperature:
// • После JSON-шапки (offset 8+hlen) идёт baseline-секция: 8192×uint32 LE (32768 Б) —
//   накопительный спектр на момент старта записи.
// • Payload (строки) начинается с offset 8+hlen+WF_BASELINE_BYTES.
// • Каждая строка = спектр (uint16×8192) + duration (uint16) + timestamp (uint32) +
//   latitude (float32) + longitude (float32) + dose_rate (float32) +
//   temperature (float32, #FW-41 t1 детектора, NaN пока -inf не прочитан) + crc32 (uint32).
//   crc32 (#DATA-1a) = стандартный CRC32 (init 0xFFFFFFFF, рефлексия, финальный XOR,
//   poly 0xEDB88320 — zlib-совместим, тот же #CMD-1) по 16406 предшествующим байтам
//   строки. PC при pull пересчитывает и сверяет → детект тихой порчи строки.
// • Шапка самоописываема: "version":5, "row_fields":[...temperature,crc32], "baseline":{...},
//   "seg_seq":N (#DATA-1b глобальный монотонный номер сегмента, NVS-персист, переживает
//   ребут/clear — PC детектит пропуск сегмента по разрыву seq), "total_at_open":T
//   (#DATA-1c накопительный total прибора на момент открытия сегмента — reconciliation:
//   события_прибор = seg[K+1].total_at_open − seg[K].total_at_open. Сверка ОДНОСТОРОННЯЯ:
//   Σ bins seg[K] ≥ события_прибор ВСЕГДА, т.к. per-channel дельта строки клампится в 0
//   при убыли канала (wf_task d<0→0), а total_counts держит истинный знаковый net.
//   Значит Σbins < события_прибор ⟺ ПОТЕРЯ; Σbins ≥ события_прибор = норма (избыток —
//   benign кламп при перекалибровке/дрейфе прибора, счёты мигрируют между каналами)).
// v1: stride=WF_ROW_BYTES (нет поля row_stride); v2: stride=16386; v3: stride=16402;
// v4: stride=16406; v5 (#FW-41): +temperature float32 перед crc → segment stride=16410,
// stream-export (без crc) stride=16406. Потребители авто-детектят stride и наличие
// поля temperature из row_fields шапки → v1..v4 читаются как прежде (temp=нет).
#define WF_DUR_BYTES          2                              // uint16 LE, секунды
#define WF_TS_BYTES           4                              // uint32 LE, unix timestamp
#define WF_GPS_BYTES          8                              // float32 LE lat + lon (NaN без GPS)
#define WF_DOSE_BYTES         4                              // float32 LE µSv/h (NaN если k=0)
#define WF_TEMP_BYTES         4                              // #FW-41: float32 LE t1 детектора °C (NaN пока -inf не прочитан)
#define WF_CRC_BYTES          4                              // #DATA-1a: uint32 LE CRC32 строки (16406 Б до него)
#define WF_ROW_PRECRC         (WF_ROW_BYTES + WF_DUR_BYTES + WF_TS_BYTES + WF_GPS_BYTES + WF_DOSE_BYTES + WF_TEMP_BYTES)
                                                             // = 16406, байты до CRC (= stride stream-экспорта без crc)
#define WF_ROW_STRIDE         (WF_ROW_PRECRC + WF_CRC_BYTES) // = 16410, запись строки v5 (segment с crc)
#define WF_BASELINE_BYTES     (WF_CHANNELS * 4)             // = 32768, baseline секция (uint32 LE)

typedef struct {
    bool     recording;
    bool     persist;        // писать строки во флэш
    bool     flash_full;     // #REC-11-A1: кольцо активно (старые сегменты затираются)
    bool     sd;             // #SD-5: SD-карта есть — сегменты пишутся на неё
    bool     storage_err;    // #SD-5: запись включена, но карты нет — строки НЕ сохраняются
    bool     ready;          // PSRAM выделена
    uint32_t interval_sec;
    uint32_t ring_capacity;  // ёмкость кольца, строк
    uint32_t ring_count;     // валидных строк в кольце
    uint32_t total_rows;     // строк записано с момента start (монотонно)
    uint32_t flash_rows;     // строк во флэш записано за сессию (монотонно)
    uint32_t seg_count;      // #REC-11-A1: завершённых сегментов на Flash сейчас
    uint32_t seg_dropped;    // #REC-11-A1: сегментов удалено кольцом с момента boot
    time_t   started_at;
    // #FW-21: длительность ТЕКУЩЕЙ сессии записи по монотонным часам (esp_timer),
    // НЕ зависит от NTP/wall-time. Источник истины для «Время записи» в UI —
    // устойчив к невалидному started_at, ребуту (restore заводит заново) и F5
    // (клиент берёт готовое число с платы, не считает сам). 0 когда не пишем.
    uint32_t elapsed_sec;
} wf_status_t;

// Колбэк рассылки новой строки (web_server регистрирует для WS-броадкаста).
typedef void (*wf_row_cb_t)(const uint16_t *row, size_t bytes, uint32_t total_index);

void   spectrogram_init(void);
// #REC-11-A1: автономное возобновление после ребута/сбоя питания.
// Сначала сверяет каталог сегментов (/storage/wf): пропатчивает счётчики у
// недописанных файлов, удаляет пустые, восстанавливает s_seg_next/seg_count.
// Если запись была активна с persist — продолжает писать в НОВЫЙ сегмент
// (без mid-segment append — каждый файл остаётся валидным .aswf).
// Вызывать на boot ПОСЛЕ spectrogram_init() (см. main.c).
void   spectrogram_restore(void);
// #OTA-3: перед штатной перезагрузкой — дописать строки и закрыть сегмент
// (запись после перезагрузки продолжится).
void   spectrogram_prepare_reboot(void);
// true, если водопад сейчас в режиме записи (для USB-реконнекта: переслать -sta).
bool   spectrogram_is_recording(void);
void   spectrogram_get_status(wf_status_t *out);
// #FW-23: вызвать из SNTP time-sync callback. Если запись уже идёт и
// started_at был зафиксирован ДО синхронизации часов (near-epoch), пересчитать
// его через монотонный аптайм-якорь — иначе весь .n42-экспорт сегмента застрянет
// на 1970 год. Идемпотентно/безопасно вызывать и когда коррекция не нужна.
void   spectrogram_time_synced(void);
int    spectrogram_start(void);
int    spectrogram_stop(void);
int    spectrogram_clear(void);
void   spectrogram_set_interval(uint32_t sec);
void   spectrogram_set_persist(bool on);
void   spectrogram_set_row_cb(wf_row_cb_t cb);

// Копирует до max_rows новейших строк кольца в dst (row-major uint16,
// хронологически — старейшая из окна первой). Возвращает число строк;
// *first_total_index = total-индекс первой возвращённой строки.
size_t spectrogram_copy_window(uint16_t *dst, size_t max_rows, uint32_t *first_total_index);

// #FW-5: копирует реальные длительности (сек) до max_rows новейших строк кольца,
// выровненные со spectrogram_copy_window/stream_window (старейшая из окна первой).
// Возвращает число элементов. Элемент 0 = device-время не продвинулось за тик
// (потребитель N42 подставляет номинальный interval_sec при делении на длительность).
size_t spectrogram_copy_window_durations(uint16_t *dst, size_t max_rows);

// #FW-41: температуры детектора (t1, °C) до max_rows новейших строк кольца,
// выровненные с spectrogram_copy_window/stream_window (старейшая из окна первой).
// NaN в элементе = на момент строки прибор ещё не прислал -inf (T неизвестна).
// Возвращает число записанных элементов.
size_t spectrogram_copy_window_temps(float *dst, size_t max_rows);

// Колбэк отдачи одной строки окна (возврат false прерывает стрим).
typedef bool (*wf_emit_cb_t)(void *ctx, const uint16_t *row, size_t bytes);

// Потоковая отдача окна кольца БЕЗ большого буфера: каждая строка копируется
// под локом в bounce (размер WF_ROW_BYTES), затем emit() вызывается ВНЕ лока
// (сетевой I/O не держит мьютекс рекордера). Отдаёт все ring_count строк
// (хронологически), не упираясь в PSRAM. Возвращает число отданных строк;
// *first_total_index = total-индекс первой строки (выставляется до первого emit).
size_t spectrogram_stream_window(uint16_t *bounce, size_t max_rows,
                                 uint32_t *first_total_index,
                                 wf_emit_cb_t emit, void *ctx);

// Каталог сегментов на Flash (/storage/wf). Файлы seg_NNNNN.aswf — каждый
// самостоятельный валидный .aswf. Используется web-слоем для листинга/отдачи.
const char *spectrogram_seg_dir(void);

// Индекс СЕЙЧАС открытого (недописанного) сегмента или 0xFFFFFFFF, если открытого
// нет. Web-слой помечает его finalized:false в /segments (его шапка ещё не
// пропатчена — saved_rows=0; забирать его в браузер не нужно до finalize).
uint32_t spectrogram_seg_open_index(void);

// #REC-11-A2: координация автономной выгрузки с кольцом keep-last.
// claim — застолбить (pin) старейший завершённый сегмент: кольцо его не удалит,
//   пока он выгружается. Возвращает true и заполняет idx/name/path/size, либо
//   false (уже занято другой выгрузкой, либо нечего слать).
// done — вызвать после подтверждённого 2xx: удалить файл с Flash и снять пин.
// release — снять пин без удаления (выгрузка не удалась, файл оставить для повтора).
bool spectrogram_offload_claim(uint32_t *idx_out, char *name_out, size_t name_cap,
                               char *path_out, size_t path_cap, long *size_out);
void spectrogram_offload_done(uint32_t idx);
void spectrogram_offload_release(uint32_t idx);

// #REC-11 pull: удалить завершённый сегмент по индексу (PC подтвердил приём через
// POST /api/waterfall/segment/delete). Не трогает открытый/pinned сегмент. true=удалён.
bool spectrogram_seg_delete(uint32_t idx);

// v3: дозовый коэффициент µSv/h per cps. 0.0 → NaN в dose_rate строк.
// Сохраняется в NVS, переживает ребут.
void  spectrogram_set_dose_k(float k);
float spectrogram_get_dose_k(void);

// v3: кривая мощности дозы (LUT по каналам). Файл /storage/dose_curve.csv:
// две колонки channel,dose_k (float). До 512 точек; интерполируется линейно.
// При загруженной кривой dose_rate в строках = взвешенная сумма bins[i]*lut[i]/dur.
// Scalar s_dose_k используется как fallback если кривая не загружена.
void  spectrogram_load_dose_curve(void);  // (пере)загрузить с диска
int   spectrogram_get_dose_curve_n(void); // число точек (0 = кривая не загружена)
// v3: вычислить мощность дозы строки (LUT если загружена, иначе scalar k, иначе NaN).
float spectrogram_compute_dose_rate(const uint16_t *bins, uint32_t dur);