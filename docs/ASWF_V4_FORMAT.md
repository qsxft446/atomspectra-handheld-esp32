# ASWF v5 — формат водопада AtomSpectra (сегмент + stream-export)

Спецификация бинарного формата `.aswf` версии 5 для других агентов и внешних
потребителей. Источник истины — прошивка: `main/spectrogram.h` (константы,
байтовая раскладка), `main/spectrogram.c` (`seg_header_build`, `crc32_upd`,
`seg_finalize` — путь **сегментов** на flash) и `main/web_waterfall.c`
(`h_export_aswf`, `aswf_row_emit` — путь **stream-export** из кольца ОЗУ).
Данный документ отражает их дословно; при расхождении прошивка приоритетна.

> Файл исторически назывался `ASWF_V4_FORMAT.md` (v4-спека, #DOC-10). Имя файла
> сохранено ради ссылок; содержимое отражает текущую v5 (#FW-41: +temperature).

Формат самоописываемый: все смещения полей и `row_stride` продублированы в
JSON-шапке, поэтому потребитель обязан читать раскладку **из шапки**, а не
хардкодить числа. Числа ниже — для v5 при `channels=8192`.

---

## 0. Два пути формата (важно для v5)

Прошивка производит `.aswf` двумя путями с **разной раскладкой строки**, но
**одинаковой** `version:5` (потребитель различает по `row_fields`, не по числу
версии):

| Путь | Источник | crc32 | baseline | `row_stride` | Как получить |
|---|---|:---:|:---:|---:|---|
| **Сегмент** (flash) | `spectrogram.c` | ✅ есть | ✅ есть | **16410** | `GET /api/waterfall/segment?name=…`, файлы `seg_NNNNN.aswf` |
| **Stream-export** (ОЗУ) | `web_waterfall.c` | ❌ нет | ❌ нет | **16406** | `GET /api/waterfall/export.aswf`, а также live-стрим сборщику |

Обе раскладки идентичны до `dose_rate` включительно и обе несут **`temperature`
float32 @ offset 16402** (новое в v5). Сегмент добавляет сверху `crc32` (→16406)
и baseline-секцию; stream-export — нет. §2–§6 описывают путь сегмента (полная
форма); §6a — отличия stream-export.

---

## 1. Назначение и модель

Водопад — спектрограмма: каждая **строка** = дельта накопительного спектра за
интервал, `uint16` на канал. Запись ведётся **сегментами**: прошивка пишет
`/storage/wf/seg_NNNNN.aswf`, каждый сегмент — самостоятельный валидный `.aswf`.

- Сегмент финализируется по достижении `WF_SEG_MAX_ROWS = 64` строк (~1 МБ
  payload) **ИЛИ** `WF_SEG_MAX_AGE_SEC = 600` (10 мин), что раньше.
- PC («сборщик») тянет финализированные сегменты по HTTP, сшивает в единый
  `.aswf`, удаляет на плате только после `fsync`.
- v4 добавил три контроля целостности данных (#DATA-1): per-row CRC32,
  глобальный `seg_seq` (детект пропуска сегментов), `total_at_open`
  (reconciliation прибор-total vs записанное).
- **v5 (#FW-41)** добавляет per-row `temperature` — t1 детектора (°C) на момент
  снятия строки. Отвечает на вопрос «как вела себя температура в момент скачка
  усиления» без косвенной оценки по CsI-коэффициенту.

---

## 2. Структура файла сегмента

```
offset  размер                        содержимое
------  ----------------------------  ------------------------------------------
0       4                             magic "ASWF" (ASCII 41 53 57 46)
4       4                             u32 LE = header_reserve (= 4096)
8       header_reserve (4096)         JSON-шапка UTF-8, добита пробелами (0x20)
8+4096  WF_BASELINE_BYTES (32768)     baseline: 8192 × uint32 LE
36872   n_rows × WF_ROW_STRIDE        payload: строки по 16410 байт
```

- `magic` — `"ASWF"`, 4 байта.
- Байты `[4..8)` — `uint32` little-endian, зарезервированный размер шапки
  (`WF_HDR_RESERVE = 4096`). JSON начинается на offset `8`, короче резерва —
  хвост добит пробелами до `8 + reserve`.
- **baseline-секция** (v3+): `8192 × uint32 LE` = накопительный спектр прибора на
  момент **старта записи**. Идёт между шапкой и payload. `WF_BASELINE_BYTES =
  channels × 4 = 32768`.
- **payload**: `n_rows` строк по `row_stride` байт. Начало payload для сегмента =
  `8 + header_reserve + WF_BASELINE_BYTES = 8 + 4096 + 32768 = 36872` (не зависит
  от версии строки — baseline и резерв не менялись).

### 2.1 Число строк выводится из размера файла (#FW-14)

Шапка содержит `"saved_rows":0` и `"saved_at":0` **всегда** — это конвенция
«выводить число строк из размера файла», а не патчить шапку при финализации
(дешёвый rollover, без повторной записи первого блока):

```
n_rows = (filesize − payload_offset) / row_stride
```

где `payload_offset = 8 + header_reserve + baseline_bytes` (baseline_bytes = 0
для форматов без секции baseline — см. историю версий). Поле `saved_rows` в шапке
игнорировать.

---

## 3. JSON-шапка

Пример (значения-плейсхолдеры; порядок ключей — как пишет `seg_header_build`):

```json
{
  "saved_rows": 0,
  "saved_at": 0,
  "format": "atomspectra-waterfall",
  "version": 5,
  "channels": 8192,
  "dtype": "uint16",
  "byte_order": "little",
  "row_stride": 16410,
  "row_fields": [
    {"name": "spectrum",    "dtype": "uint16",  "channels": 8192, "offset": 0},
    {"name": "duration",    "dtype": "uint16",  "unit": "sec",      "offset": 16384},
    {"name": "timestamp",   "dtype": "uint32",  "unit": "unix_sec", "offset": 16386},
    {"name": "latitude",    "dtype": "float32", "unit": "deg",      "offset": 16390},
    {"name": "longitude",   "dtype": "float32", "unit": "deg",      "offset": 16394},
    {"name": "dose_rate",   "dtype": "float32", "unit": "usv_h",    "offset": 16398},
    {"name": "temperature", "dtype": "float32", "unit": "celsius",  "offset": 16402},
    {"name": "crc32",       "dtype": "uint32",  "algo": "crc32", "covers": 16406, "offset": 16406}
  ],
  "baseline": {"dtype": "uint32", "channels": 8192, "byte_order": "little"},
  "compressed": false,
  "seg_seq": 12345,
  "total_at_open": 987654,
  "interval_sec": 60,
  "started_at": 1751000000,
  "serial": "AS-XXXXXXXX",
  "calibration": [0.0, 0.123, 0.0]
}
```

### 3.1 Поля шапки

| Ключ | Тип | Смысл |
|---|---|---|
| `saved_rows` | int | Всегда `0` (#FW-14, см. §2.1). Игнорировать. |
| `saved_at` | int | Всегда `0`. Игнорировать. |
| `format` | str | `"atomspectra-waterfall"`. |
| `version` | int | `5`. |
| `channels` | int | Число каналов спектра (`8192`). |
| `dtype` | str | Тип канала спектра — `"uint16"`. |
| `byte_order` | str | `"little"` для всех числовых полей. |
| `row_stride` | int | Полный размер строки в байтах (`16410` для сегмента v5). **Читать stride отсюда.** |
| `row_fields` | array | Самоописание раскладки строки (см. §4). |
| `baseline` | obj | Описание baseline-секции: `dtype=uint32`, `channels`, `byte_order`. Присутствует в v3+ (только путь сегмента). |
| `compressed` | bool | `false` (payload не сжат). |
| `seg_seq` | int | #DATA-1b: глобальный монотонный номер сегмента, NVS-персист, переживает ребут/clear. Детект пропуска сегментов. |
| `total_at_open` | int | #DATA-1c: накопительный `total_counts` прибора на момент открытия сегмента. Reconciliation (см. §6). |
| `interval_sec` | int | Заданный интервал между строками, сек. |
| `started_at` | int | Unix-время (сек) открытия **этого сегмента**. |
| `serial` | str | *Опционально.* Серийник прибора (если известен). |
| `calibration` | array of float | *Опционально.* Полиномиальные коэффициенты энергокалибровки `[c0, c1, c2, ...]`, keV = Σ cᵢ·chⁱ. Присутствует только если калибровка валидна. |

---

## 4. Раскладка строки сегмента (v5)

`row_stride = 16410` байт. Все числа little-endian.

| Поле | Offset | Размер | Тип | Смысл |
|---|---:|---:|---|---|
| `spectrum` | 0 | 16384 | `uint16` × 8192 | Дельта спектра за интервал (счёты на канал, клампится в ≥0). |
| `duration` | 16384 | 2 | `uint16` | Фактическая длительность интервала строки, сек. |
| `timestamp` | 16386 | 4 | `uint32` | Unix-время строки, сек. |
| `latitude` | 16390 | 4 | `float32` | Широта, град. `NaN` без GPS. |
| `longitude` | 16394 | 4 | `float32` | Долгота, град. `NaN` без GPS. |
| `dose_rate` | 16398 | 4 | `float32` | Мощность дозы, µSv/h. `NaN` если k-фактор=0. |
| `temperature` | 16402 | 4 | `float32` | **v5 (#FW-41):** t1 детектора, °C. `NaN` пока `-inf` не прочитан (см. §4a). |
| `crc32` | 16406 | 4 | `uint32` | #DATA-1a: CRC32 первых 16406 байт строки (см. §5). |

`WF_ROW_PRECRC = 16406` — число байт, покрытых CRC (всё до самого поля crc32,
теперь включая `temperature`). `covers` в `row_fields[crc32]` = это же значение.

### 4a. Семантика `temperature` (#FW-41)

- **Единица:** °C, `float32` little-endian.
- **Источник:** `spectrum_get_device_info()->t1` — температура детектора (t1),
  которую прошивка получает из ответа прибора на команду `-inf`.
- **`NaN`-конвенция:** если `-inf` ещё ни разу не прочитан (`device_info.valid ==
  false`), поле = `NaN` (`0x7FC00000`). Это **защита от ложного 0 °C**: структура
  `device_info` инициализирована нулями, и без guard-а `valid` непрочитанная t1
  выглядела бы как настоящие 0 °C.
- **Каденция ступенчатая.** `-inf` опрашивается ~раз в 30 мин (сознательно
  не чаще — фикс USB-шторма #FW-13). Значит t1 в строках обновляется ступенями:
  подряд идущие строки в пределах одного 30-мин окна несут **одно и то же**
  значение t1. Это не потеря — это разрешение источника. Для анализа дрейфа
  усиления vs температуры этого достаточно (усиление CsI меняется медленно).
- **Ретроактивно** восстановить t1 для файлов v1–v4 невозможно — там оно
  физически не записано.

---

## 5. CRC32 строки (#DATA-1a)

Стандартный CRC32, **zlib-совместимый** (`zlib.crc32` в Python, тот же алгоритм,
что #CMD-1 в `spectrum.c`):

- init = `0xFFFFFFFF`
- полином = `0xEDB88320` (рефлексированный)
- по байтам, финальный XOR = `0xFFFFFFFF`
- покрывает `covers = 16406` предшествующих байт строки (spectrum..temperature).

Референс прошивки (`crc32_upd`, аккумулятивный — старт `0xFFFFFFFF`, финал
`^0xFFFFFFFF`):

```c
static uint32_t crc32_upd(uint32_t crc, const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int j = 0; j < 8; j++)
            crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320u : (crc >> 1);
    }
    return crc;
}
```

Проверка на PC (Python):

```python
import struct, zlib
covers = header_row_field("crc32")["covers"]     # 16406 (v5), 16402 (v4)
stored = struct.unpack_from("<I", row, covers)[0]
calc   = zlib.crc32(row[:covers]) & 0xFFFFFFFF
crc_ok = (stored == calc)                         # False = тихая порча строки
```

> **Важно для парсера:** `covers` брать **из шапки**, не хардкодить. v4 → 16402,
> v5 → 16406. Референс-читатель (§8) делает это автоматически.

Несовпадение → строка повреждена в тракте (flash/USB/HTTP) — считать событием
потери целостности.

---

## 6. Контроли целостности v4 (#DATA-1)

### 6.1 seg_seq — детект пропуска сегментов (#DATA-1b)

`seg_seq` монотонно растёт на каждый открытый сегмент, персистится в NVS
(переживает ребут/clear/power-loss). Кольцо keep-last на плате может стереть
сегмент до того, как PC его вытянул. Разрыв последовательности → пропуск:

```
gap = seg[K+1].seg_seq − seg[K].seg_seq − 1
```

`gap > 0` ⟹ `gap` сегментов кольцо стёрло до pull → безвозвратная потеря строк.
`gap == 0` ⟹ непрерывно. Первый вытянутый сегмент опорного значения не имеет
(gap = None).

### 6.2 Reconciliation — прибор-total vs записанное (#DATA-1c)

`total_at_open` = накопительный `total_counts` прибора в момент открытия
сегмента. Число событий, зафиксированных прибором за сегмент K:

```
события_прибор(K) = seg[K+1].total_at_open − seg[K].total_at_open
```

Сверка с суммой записанных бинов `Σ bins(K)` — **односторонняя**:

> `Σ bins(K) ≥ события_прибор(K)` — норма.
> `Σ bins(K) <  события_прибор(K)` — ПОТЕРЯ событий.

Причина односторонности: per-channel дельта строки клампится в 0 при убыли канала
(`wf_task`: `d<0 → 0`), а `total_counts` прибора держит истинный знаковый net.
Значит записанная Σbins может быть только **больше либо равна** истинному
приросту; превышение (`d ≤ 0` при сверке) — benign кламп (перекалибровка/дрейф,
счёты мигрируют между каналами), не потеря. Недостача (`d > 0`) — реальная потеря
событий между прибором и записью.

```python
d = device_delta − sum_bins_prev      # d > 0  => потеря d событий
                                      # d <= 0 => норма (benign кламп)
```

---

## 6a. Путь stream-export (`export.aswf`) — отличия

Экспорт кольца ОЗУ (`web_waterfall.c: h_export_aswf` / `aswf_row_emit`) даёт тот
же `version:5`, но **облегчённую** раскладку:

- **`row_stride = 16406`** (нет crc32 в конце строки).
- **Нет baseline-секции** → `payload_offset = 8 + reserve = 4104`.
- **Нет** `seg_seq` / `total_at_open` в шапке (эти контроли только для сегментов).
- `row_fields` заканчивается на `temperature` (offset 16402), поля `crc32` нет:

```json
"row_stride": 16406,
"row_fields": [
  {"name": "spectrum",    "dtype": "uint16",  "channels": 8192, "offset": 0},
  {"name": "duration",    "dtype": "uint16",  "unit": "sec",      "offset": 16384},
  {"name": "timestamp",   "dtype": "uint32",  "unit": "unix_sec", "offset": 16386},
  {"name": "latitude",    "dtype": "float32", "unit": "deg",      "offset": 16390},
  {"name": "longitude",   "dtype": "float32", "unit": "deg",      "offset": 16394},
  {"name": "dose_rate",   "dtype": "float32", "unit": "usv_h",    "offset": 16398},
  {"name": "temperature", "dtype": "float32", "unit": "celsius",  "offset": 16402}
]
```

`temperature` в stream-export несёт ту же семантику §4a (NaN пока `-inf` не
прочитан, ступенчатая каденция). Именно этот путь производит файлы, скачиваемые
через `export.aswf` и приходящие live-сборщику. Поскольку `crc32` отсутствует —
контроль целостности per-row для stream-export не применяется (используется на
сегментах flash).

> **Совпадение номера версии безвредно.** Обе раскладки — `version:5`, но stride
> разный (16410 vs 16406). Потребитель **обязан** брать stride и набор полей из
> `row_fields`/`row_stride` шапки, а не из числа версии. Наличие поля `crc32` в
> `row_fields` = путь сегмента; отсутствие = stream-export.

---

## 7. История версий и обратная совместимость

Потребитель авто-детектит `row_stride` из шапки; v1..v4 читаются как прежде.

| Версия | `row_stride` | Отличия |
|---|---:|---|
| v1 | 16384 (`WF_ROW_BYTES`) | Нет поля `row_stride` в шапке; только spectrum. |
| v2 | 16386 | + `duration` (u16). |
| v3 | 16402 | + `timestamp`, `latitude`, `longitude`, `dose_rate`. Есть baseline-секция. **Нет** crc32. |
| v4 | 16406 | + `crc32` (u32) в конце строки. + `seg_seq`, `total_at_open` в шапке. Контроли целостности #DATA-1. |
| **v5** | **16410** (сегмент) / **16406** (stream-export) | **+ `temperature` (float32) перед crc32** (#FW-41). Сегмент: temperature@16402, crc32@16406, stride 16410. Stream-export: temperature@16402, crc32 нет, stride 16406. |

> **Коллизия strides между версиями:** v4-сегмент и v5-stream-export оба имеют
> stride 16406, но разный смысл последних 4 байт (v4: crc32; v5-stream:
> temperature). Различать по `version` **и** по наличию поля `crc32` в
> `row_fields`. Это ещё одна причина читать раскладку из `row_fields`, а не из
> одного числа.

Правила чтения смешанных версий:
- stride брать из `row_stride` (v1 — отсутствует ⟹ 16384).
- baseline-секцию читать только если в шапке есть ключ `"baseline"` (v3+, путь
  сегмента); соответственно `payload_offset` = `8 + reserve` (нет baseline) или
  `8 + reserve + baseline_bytes` (есть baseline).
- crc32 проверять только при наличии поля `crc32` в `row_fields` (v4+ сегмент).
- temperature читать только при наличии поля `temperature` в `row_fields` (v5+).

---

## 8. Референс-читатель (Python, эскиз)

Полностью самоописываемый — работает для v1..v5, обоих путей (сегмент/stream),
т.к. берёт stride, offsets, наличие crc/temperature из шапки:

```python
import json, struct, zlib

def read_aswf(path):
    with open(path, "rb") as f:
        blob = f.read()
    assert blob[:4] == b"ASWF", "bad magic"
    reserve = struct.unpack_from("<I", blob, 4)[0]
    hdr = json.loads(blob[8:8 + reserve].split(b"\x00")[0].decode("utf-8").rstrip())
    stride = hdr.get("row_stride", hdr["channels"] * 2)
    has_baseline = "baseline" in hdr
    baseline_bytes = hdr["channels"] * 4 if has_baseline else 0
    payload_off = 8 + reserve + baseline_bytes
    n_rows = (len(blob) - payload_off) // stride

    fields = {rf["name"]: rf for rf in hdr["row_fields"]}
    crc_field  = fields.get("crc32")         # None для stream-export
    temp_field = fields.get("temperature")   # None для v1..v4

    for i in range(n_rows):
        row = blob[payload_off + i * stride : payload_off + (i + 1) * stride]
        if crc_field:                                  # v4+ сегмент
            covers = crc_field["covers"]
            stored = struct.unpack_from("<I", row, crc_field["offset"])[0]
            if (zlib.crc32(row[:covers]) & 0xFFFFFFFF) != stored:
                raise ValueError(f"row {i}: CRC32 mismatch (порча)")
        spectrum = struct.unpack_from(f"<{hdr['channels']}H", row, 0)
        temp = None
        if temp_field:                                 # v5+
            temp = struct.unpack_from("<f", row, temp_field["offset"])[0]
            # temp может быть NaN — проверять math.isnan()
        # duration/timestamp/lat/lon/dose — по offsets из fields[...]
        yield spectrum, temp
```

---

## 9. Константы (из `main/spectrogram.h`, channels=8192)

| Константа | Значение | Смысл |
|---|---:|---|
| `WF_CHANNELS` | 8192 | Каналов спектра. |
| `WF_ROW_BYTES` | 16384 | spectrum = channels × 2. |
| `WF_DUR_BYTES` | 2 | duration u16. |
| `WF_TS_BYTES` | 4 | timestamp u32. |
| `WF_GPS_BYTES` | 8 | lat + lon (2 × float32). |
| `WF_DOSE_BYTES` | 4 | dose_rate float32. |
| `WF_TEMP_BYTES` | 4 | **v5:** temperature float32. |
| `WF_CRC_BYTES` | 4 | crc32 u32. |
| `WF_ROW_PRECRC` | 16406 | Байт, покрытых CRC (covers) = spectrum..temperature. |
| `WF_ROW_STRIDE` | 16410 | Полный размер строки сегмента v5 (`WF_ROW_PRECRC + WF_CRC_BYTES`). |
| `WF_HDR_RESERVE` | 4096 | Резерв JSON-шапки. |
| `WF_SEG_HEADER` | 4104 | `8 + reserve` (payload offset без baseline = stream-export). |
| `WF_BASELINE_BYTES` | 32768 | baseline = channels × 4. |
| `WF_SEG_MAX_ROWS` | 64 | Строк на сегмент (~1 МБ). |
| `WF_SEG_MAX_AGE_SEC` | 600 | Макс. возраст открытого сегмента, сек. |
| payload offset сегмента (v5) | 36872 | `8 + 4096 + 32768` (не изменился). |
| payload offset stream-export | 4104 | `8 + 4096` (baseline нет). |

Заметь: stride строки вырос (16406→16410 сегмент; 16402→16406 stream), но
payload-offset **не** изменился — baseline и резерв шапки те же.

---

*Источник: `firmware/atomspectra-waterfall/main/spectrogram.{h,c}` (сегмент),
`main/web_waterfall.c` (stream-export). PC-реализация чтения/сверки —
`scripts/wf_pull_client.py` (стежок + verify CRC/seq/recon),
`scripts/wf_recorder_app.py` (UI-сборщик, вывод контроля целостности).*
