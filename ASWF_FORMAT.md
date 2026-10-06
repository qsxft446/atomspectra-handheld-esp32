# Формат файла ASWF — спецификация для интеграции

[🇬🇧 English](ASWF_FORMAT.en.md) · [← README](README.md) · [Водопад](WATERFALL.md)

**ASWF** (AtomSpectra Waterfall Format) — бинарный формат хранения спектрограммы
(водопада): хронологическая последовательность спектров, снятых через фиксированные
интервалы. Каждый файл `.aswf` самодостаточен — содержит полный заголовок и данные,
не требует внешней схемы.

---

## История версий

| Версия | Ключевые изменения |
|--------|-------------------|
| **v1** | Базовый формат. 16384 байт/строка (8192 × uint16 LE). Нет поля длительности. |
| **v2** | +2 байта uint16 LE длительности в конце каждой строки. `row_stride=16386`, `row_time` в заголовке. |
| **v3** | Self-describing `row_fields`. Поля метки времени, GPS, мощности дозы. Секция baseline. Флаг сжатия. `row_stride=16402`. |
| **v4** | +per-row `crc32` (контроль целостности строки, `covers=16402`). `row_stride=16406`. В заголовок добавлены `seg_seq` и `total_at_open` (детект пропуска сегментов и reconciliation). |
| **v5** | +`temperature` (float32, °C детектора) перед `crc32` → `crc32.offset/covers` сдвинулись на `16406`. `row_stride=16410`. **Текущий формат прошивки** (`firmware-v1.0.11`, #FW-41). |

---

## Структура файла

### v1 / v2

```
Offset 0:      4 bytes   Магия "ASWF" (ASCII)
Offset 4:      4 bytes   uint32 LE: hlen (зарезервированный размер JSON-области)
Offset 8:      hlen bytes JSON-заголовок (UTF-8, дополнен пробелами до hlen)
Offset 8+hlen: N × stride строк данных (старейшая первая)
```

### v3

```
Offset 0:          4 bytes     Магия "ASWF" (ASCII)
Offset 4:          4 bytes     uint32 LE: hlen
Offset 8:          hlen bytes  JSON-заголовок (UTF-8, дополнен пробелами до hlen)
Offset 8+hlen:     B bytes     Baseline-спектр (если "baseline" есть в заголовке; иначе 0)
Offset 8+hlen+B:   N × stride  Строки данных (старейшая первая)
```

где `B = baseline.channels × 4` (uint32 на канал; в ранних файлах ключ назывался `count`),
либо `0` если секция отсутствует.

> **Важно:** `hlen` — зарезервированный размер, не фактическая длина JSON-текста.
> Текущее значение всегда `4096`. Смещение payload = `8 + hlen + B`.
> Парсер обязан использовать `hlen` и `B`, а не сканировать `}`.

---

## JSON-заголовок

Заголовок полностью самоописываем — все поля, нужные для декодирования, внутри файла.

### Поля (все версии)

| Ключ           | Тип              | Обязательный | Описание |
|----------------|------------------|:---:|------|
| `format`       | string           | да  | Всегда `"atomspectra-waterfall"` |
| `version`      | int              | да  | `1`…`5` (текущая прошивка пишет `5`) |
| `channels`     | int              | да  | Каналов в строке; текущее значение `8192` |
| `dtype`        | string           | да  | Тип элемента спектра: `"uint16"` |
| `byte_order`   | string           | да  | Порядок байт: `"little"` |
| `interval_sec` | int              | да  | Номинальный интервал записи, секунды |
| `started_at`   | int (unix ts)    | да  | Метка времени первой строки (UTC, с от эпохи); `0` = NTP не синхронизирован |
| `saved_rows`   | int              | да  | Число строк в файле; `0` у открытого/незакрытого файла |
| `saved_at`     | int (unix ts)    | да  | Время финализации; `0` у открытого файла |
| `serial`       | string           | нет | Серийный номер устройства |
| `calibration`  | array of floats  | нет | Коэффициенты энергетической калибровки |

### Дополнительные поля v2

| Ключ         | Тип   | Описание |
|--------------|-------|----------|
| `row_stride` | int   | Размер строки в байтах (`16386`). Отсутствует в v1 |
| `row_time`   | object | `{"dtype":"uint16","unit":"sec","offset":16384}` |

### Дополнительные поля v3

| Ключ          | Тип    | Описание |
|---------------|--------|----------|
| `row_fields`  | array  | Описание полей каждой строки (см. ниже) |
| `row_stride`  | int    | Суммарный размер строки в байтах (вычисляется из `row_fields`) |
| `compressed`  | bool   | `true` = строки сжаты RLE (по умолчанию `false`) |
| `baseline`    | object | Описание baseline-секции между заголовком и payload (см. ниже) |

### Объект `row_fields` (v3)

Массив дескрипторов полей строки. Парсер читает только описанные поля; неизвестные — игнорирует.

```json
"row_fields": [
  {"name": "spectrum",         "dtype": "uint16",  "channels": 8192, "offset": 0},
  {"name": "duration",         "dtype": "uint16",                    "offset": 16384},
  {"name": "timestamp",        "dtype": "uint32",                 "offset": 16386},
  {"name": "latitude",         "dtype": "float32",                "offset": 16390},
  {"name": "longitude",        "dtype": "float32",                "offset": 16394},
  {"name": "dose_rate_usv_h",  "dtype": "float32",                "offset": 16398}
]
```

| Поле               | Тип     | Описание |
|--------------------|---------|----------|
| `spectrum`         | uint16  | Спектр: `channels` каналов, LE, импульсы за интервал |
| `duration`         | uint16  | Фактическая длительность среза, секунды (0 = использовать `interval_sec`) |
| `timestamp`        | uint32  | Unix-метка начала строки (UTC, с); 0 = неизвестно |
| `latitude`         | float32 | Широта в десятичных градусах; NaN = нет данных |
| `longitude`        | float32 | Долгота в десятичных градусах; NaN = нет данных |
| `dose_rate_usv_h`  | float32 | Мощность дозы, мкЗв/ч; NaN = нет данных |

Поля `latitude`, `longitude`, `dose_rate_usv_h`, `timestamp` — опциональны.
Если устройство не имеет GPS или дозиметра — соответствующие дескрипторы отсутствуют в `row_fields`.

### Объект `baseline` (v3+)

```json
"baseline": {
  "dtype":      "uint32",
  "channels":   8192,
  "byte_order": "little"
}
```

Baseline — накопленный спектр устройства на момент **начала** текущей сессии записи.
Физически: `channels` × uint32 LE = 32768 байт, расположены между JSON-областью и payload
(размер `B = baseline.channels × 4`).

> **Именование:** текущая прошивка пишет ключ `channels`; в ранних примерах встречается
> `count` — парсер должен принимать оба (`baseline.channels ?? baseline.count`). Поля
> `offset` в объекте нет — смещение всегда `8 + hlen`.

Полезен для абсолютной реконструкции спектра (baseline + сумма строк водопада = полный накопленный спектр).

### Дополнительные поля v4 / v5

**v4** добавляет per-row контроль целостности; **v5** — температуру детектора.

| Поле row_fields | Введён | Тип | Offset (v5) | Описание |
|---|---|---|---|---|
| `temperature` | v5 | float32 | 16402 | Температура детектора, °C; `NaN` (`0x7FC00000`) пока прибор не отдал `-inf` |
| `crc32`       | v4 | uint32  | 16406 | CRC32 строки; `covers` = число покрытых байт от начала строки |

В v4 `crc32` был на offset `16402` (сразу за `dose_rate`), `covers=16402`. В v5 перед ним
вставлена `temperature`, поэтому `crc32.offset = covers = 16406`. **Парсер обязан брать
`offset`/`covers` из `row_fields`, а не хардкодить** — иначе смена версии ломает разбор.

### Заголовочные поля v4+ (контроль целостности)

| Ключ | Тип | Описание |
|---|---|---|
| `seg_seq`       | int (uint32) | Глобальный монотонный номер сегмента (переживает reboot/clear через NVS). Разрыв последовательности склеенных сегментов = сегмент потерян, кольцо стёрло его до выгрузки (#DATA-1b) |
| `total_at_open` | int (uint32) | Накопленный счётчик прибора на момент открытия сегмента. Дельта между соседними сегментами сверяется с Σ отсчётов предыдущего (reconciliation, #DATA-1c) |

### CRC32 строки (v4+)

Стандартный CRC32: init `0xFFFFFFFF`, полином `0xEDB88320` (reflected), финальный XOR
`0xFFFFFFFF` — zlib-совместимый. Поле `crc32` покрывает первые `covers` байт строки
(всё до самого поля). Проверка:

```python
import zlib
def row_crc_ok(row_bytes, covers, stored_crc):
    return (zlib.crc32(row_bytes[:covers]) & 0xFFFFFFFF) == stored_crc
```

Несовпадение = порча строки при передаче/хранении. `covers` берётся из дескриптора
`crc32` в `row_fields` (v4 → 16402, v5 → 16406).

### Пример заголовка v5

```json
{
  "format":       "atomspectra-waterfall",
  "version":      5,
  "channels":     8192,
  "dtype":        "uint16",
  "byte_order":   "little",
  "row_stride":   16410,
  "row_fields": [
    {"name": "spectrum",    "dtype": "uint16",  "channels": 8192,                  "offset": 0},
    {"name": "duration",    "dtype": "uint16",  "unit": "sec",                     "offset": 16384},
    {"name": "timestamp",   "dtype": "uint32",  "unit": "unix_sec",                "offset": 16386},
    {"name": "latitude",    "dtype": "float32", "unit": "deg",                     "offset": 16390},
    {"name": "longitude",   "dtype": "float32", "unit": "deg",                     "offset": 16394},
    {"name": "dose_rate",   "dtype": "float32", "unit": "usv_h",                   "offset": 16398},
    {"name": "temperature", "dtype": "float32", "unit": "celsius",                 "offset": 16402},
    {"name": "crc32",       "dtype": "uint32",  "algo": "crc32", "covers": 16406,  "offset": 16406}
  ],
  "baseline":      {"dtype": "uint32", "channels": 8192, "byte_order": "little"},
  "compressed":    false,
  "seg_seq":       42,
  "total_at_open": 1234567,
  "interval_sec":  60,
  "started_at":    1783157403,
  "saved_rows":    660,
  "saved_at":      1783197003,
  "serial":        "AS-001",
  "calibration":   [0.0, 0.298, 0.0]
}
```

---

## Данные спектра

Каждый канал строки — **дельта** накопительного спектра: число импульсов за данный интервал. Тип `uint16` — значения `0–65535`.

### Абсолютный спектр (сумма за всё время)

```python
absolute = [0] * channels
for row in rows:
    for ch in range(channels):
        absolute[ch] += row[ch]
```

При наличии baseline (v3): полный накопленный спектр = `baseline[ch] + absolute[ch]`.

### Скорость счёта (отсчётов/с) для строки i

```python
dur_i = duration[i] if duration[i] > 0 else interval_sec
rate  = [row[i][ch] / dur_i for ch in range(channels)]
```

---

## Временны́е метки строк

### v1 / v2 — реконструкция через `started_at`

```
t[0] = started_at
t[i] = started_at + sum(duration[0..i-1])
```

Для строки `j` с `duration[j] == 0` подставить `interval_sec`.
При `started_at == 0` (NTP не синхронизирован) временна́я шкала относительна.

### v3 — прямое поле `timestamp`

Если поле `timestamp` присутствует в `row_fields`, метка времени читается прямо из строки (uint32, UTC, секунды). `0` = неизвестно.
Поле `timestamp` имеет приоритет над реконструированным значением.

---

## Сжатие (v3, `compressed: true`)

При `compressed: true` область спектра каждой строки хранится в **RLE-кодировке**.
Поля вне спектра (`duration`, `timestamp`, GPS, `dose_rate`) — всегда без сжатия,
после спектрового потока.

### Формат RLE-потока

Поток состоит из uint16 LE элементов:
- Значение `< 0x8000` → **literal**: один канал со значением `v`
- Значение `0x8000..0xFFFE` → **run**: `v & 0x7FFF` последовательных нулей
- Значение `0xFFFF` → зарезервировано

Поток заканчивается когда декодировано ровно `channels` каналов.

> **Ограничение:** max значение в одном канале = 32767 (`0x7FFF`). Для каналов с
> большим числом отсчётов сжатие неприменимо — такие файлы должны иметь `compressed: false`.

**Сжатие нарушает произвольный доступ к строкам** — чтение возможно только
последовательно с начала payload. Для `compressed: false` (дефолт) random-access работает.

При `compressed: true` поле `row_stride` **отсутствует** (переменная длина строк);
число строк определяется декодированием, а не по размеру файла.

---

## Определение версии и смещений

```python
import json, struct, math
from pathlib import Path

def open_aswf(path):
    buf  = Path(path).read_bytes()
    assert buf[:4] == b"ASWF", f"Not ASWF: {buf[:4]!r}"
    hlen = struct.unpack_from("<I", buf, 4)[0]
    hdr  = json.loads(buf[8:8 + hlen].decode("utf-8"))
    ver  = hdr.get("version", 1)

    # Смещение baseline
    baseline_bytes = 0
    if "baseline" in hdr:
        bl = hdr["baseline"]
        baseline_bytes = (bl.get("channels") or bl.get("count") or 0) * 4

    payload_off = 8 + hlen + baseline_bytes

    # Stride и число строк
    if ver == 1:
        stride = hdr["channels"] * 2           # 16384
        compressed = False
    elif ver == 2:
        stride = hdr.get("row_stride", hdr["channels"] * 2)
        compressed = False
    else:  # v3 / v4 / v5 — stride и смещения полей всегда из шапки (row_stride/row_fields)
        compressed = hdr.get("compressed", False)
        stride = hdr.get("row_stride", 0) if not compressed else 0

    n_rows = hdr.get("saved_rows") or (
        (len(buf) - payload_off) // stride if stride else None
    )
    return hdr, buf, payload_off, stride, n_rows, compressed
```

---

## Python: полный парсер (v1–v5)

```python
import json, struct, math
from pathlib import Path

NAN = float("nan")

def read_aswf(path):
    """Читает .aswf (v1–v5), возвращает (header, rows).

    rows — list of dict с ключами из row_fields:
      'spectrum'    : tuple of int
      'duration'    : int (секунды; 0 = подставь interval_sec)
      'timestamp'   : int (unix ts; 0 = неизвестно)   [v3+]
      'latitude'    : float (NaN = нет данных)        [v3+]
      'longitude'   : float (NaN = нет данных)        [v3+]
      'dose_rate'   : float (NaN = нет данных)        [v3+]
      'temperature' : float (NaN = нет данных, °C)    [v5+]
      'crc32'       : int (CRC32 строки, см. covers)  [v4+]
    """
    buf  = Path(path).read_bytes()
    assert buf[:4] == b"ASWF", f"Not ASWF: {buf[:4]!r}"

    hlen = struct.unpack_from("<I", buf, 4)[0]
    hdr  = json.loads(buf[8:8 + hlen].decode("utf-8"))
    ver  = hdr.get("version", 1)
    ch   = hdr["channels"]
    iv   = hdr["interval_sec"]

    # Baseline
    baseline = None
    baseline_bytes = 0
    if "baseline" in hdr:
        bi = hdr["baseline"]
        nbl = bi.get("channels") or bi.get("count") or 0
        baseline_bytes = nbl * 4
        baseline = struct.unpack_from(f"<{nbl}I", buf, 8 + hlen)

    payload_off = 8 + hlen + baseline_bytes
    payload     = buf[payload_off:]

    # row_fields: строим lookup
    if ver >= 3 and "row_fields" in hdr:
        fields = {f["name"]: f for f in hdr["row_fields"]}
        stride = hdr.get("row_stride", 0)
        compressed = hdr.get("compressed", False)
    else:
        fields     = None
        stride     = hdr.get("row_stride", ch * 2)
        compressed = False

    def get_field(row_buf, off, dtype):
        if dtype == "uint16":  return struct.unpack_from("<H", row_buf, off)[0]
        if dtype == "uint32":  return struct.unpack_from("<I", row_buf, off)[0]
        if dtype == "float32": return struct.unpack_from("<f", row_buf, off)[0]
        raise ValueError(dtype)

    rows = []
    if not compressed:
        n = len(payload) // stride
        for i in range(n):
            rb = payload[i * stride:(i + 1) * stride]
            row = {}
            if fields:
                sf = fields["spectrum"]
                row["spectrum"] = struct.unpack_from(f"<{ch}H", rb, sf["offset"])
                for name, fd in fields.items():
                    if name == "spectrum": continue
                    row[name] = get_field(rb, fd["offset"], fd["dtype"])
            else:
                row["spectrum"] = struct.unpack_from(f"<{ch}H", rb, 0)
                dur_off = ch * 2
                if len(rb) >= dur_off + 2:
                    row["duration"] = struct.unpack_from("<H", rb, dur_off)[0]
                else:
                    row["duration"] = iv
            rows.append(row)
    else:
        # RLE-декодирование
        pos = 0
        raw = payload
        while pos < len(raw):
            spec = []
            while len(spec) < ch:
                v = struct.unpack_from("<H", raw, pos)[0]; pos += 2
                if v < 0x8000:
                    spec.append(v)
                elif v < 0xFFFF:
                    spec.extend([0] * (v & 0x7FFF))
            spec = tuple(spec[:ch])
            row = {"spectrum": spec}
            # фиксированные поля после спектра
            if fields:
                for name, fd in fields.items():
                    if name == "spectrum": continue
                    row[name] = get_field(raw, pos, fd["dtype"]); pos += 4
            rows.append(row)

    return hdr, rows, baseline


def row_timestamp(hdr, rows, index):
    """Unix-timestamp начала строки index (v2 реконструкция или v3 прямое поле)."""
    row = rows[index]
    if "timestamp" in row and row["timestamp"] > 0:
        return row["timestamp"]
    ts = hdr["started_at"]
    iv = hdr["interval_sec"]
    for j in range(index):
        d = rows[j].get("duration", iv)
        ts += d if d > 0 else iv
    return ts


def channel_to_kev(hdr, ch):
    cal = hdr.get("calibration")
    if not cal: return None
    return sum(a * ch**i for i, a in enumerate(cal))
```

---

## Калибровка: канал → энергия (кэВ)

```
E(ch) = a[0] + a[1]·ch + a[2]·ch² + …
```

`a = calibration` (массив коэффициентов, индекс = степень полинома).
Если поле отсутствует — ось X в номерах каналов.

---

## HTTP API устройства

### Список сегментов

```
GET /api/waterfall/segments
```

Ответ `200 application/json`:

```json
{
  "segments": [
    {"name": "seg_00000.aswf", "size": 1056870, "finalized": true},
    {"name": "seg_00001.aswf", "size": 524904,  "finalized": false}
  ],
  "ring_capacity": 64,
  "seg_count": 2
}
```

`finalized: false` — сегмент открыт; `saved_rows=0`, число строк вычислить по размеру.

### Скачивание сегмента

```
GET /api/waterfall/segment?name=seg_00000.aswf
Authorization: Basic <base64(login:password)>
```

Ответ `200 application/octet-stream`.

### Удаление (подтверждение приёма)

```
POST /api/waterfall/segment/delete?name=seg_00000.aswf
Authorization: Basic <base64(login:password)>
```

### Экспорт кольца ОЗУ в ASWF

```
GET /api/waterfall/export.aswf
Authorization: Basic <base64(login:password)>
```

Ответ `200 application/octet-stream` — полный файл ASWF актуальной версии (текущая прошивка — v5).

Экспортирует текущее кольцо PSRAM (последние `ring_capacity` строк) в единый файл ASWF v5.
Baseline-секция отсутствует (кольцо — скользящее окно, не полная сессия).
Поля строки: `spectrum`, `duration`, `timestamp`, `latitude` (NaN), `longitude` (NaN), `dose_rate`, `temperature`, `crc32`.
Возвращает `404`, если данных ещё нет.

> **Примечание об именовании полей:** firmware использует `"name":"dose_rate"` с `"unit":"usv_h"` в `row_fields`;
> в документации применяется псевдоним `dose_rate_usv_h` для ясности. Парсер должен поддерживать оба варианта.

---

## Кольцевой буфер сегментов

Каждый сегмент: до 64 строк (~1 МБ payload), не более 10 минут записи.
При переполнении кольца — старейший сегмент удаляется автоматически.
Незакрытый сегмент (`finalized: false`) в лимит не входит.

---

## Склейка сегментов

> **Смена формата прошивки (#REC-14):** сегменты с разным `row_stride` (напр. v3 `16402`
> и v5 `16410`) склеивать в один файл **нельзя** — читатель раскладывает водопад по
> единому `row_stride` из шапки, и хвост после стыка станет кашей. Сборщик разводит
> несовпадающие форматы по отдельным файлам `<base>__s<stride>.aswf`; ниже — склейка
> сегментов **одного** формата.

```python
import json, struct
from pathlib import Path

def merge_aswf(paths_sorted, out_path):
    HDR_RESERVE = 4096
    files  = [Path(p).read_bytes() for p in paths_sorted]
    first  = files[0]
    hlen   = struct.unpack_from("<I", first, 4)[0]
    hdr    = json.loads(first[8:8 + hlen].decode("utf-8"))
    stride = hdr.get("row_stride", hdr["channels"] * 2)

    # baseline берём из первого файла (если есть)
    baseline_bytes = 0
    baseline_data  = b""
    if "baseline" in hdr:
        _b = hdr["baseline"]
        baseline_bytes = (_b.get("channels") or _b.get("count") or 0) * 4
        baseline_data  = first[8 + hlen:8 + hlen + baseline_bytes]

    payload = b""
    for buf in files:
        h2 = struct.unpack_from("<I", buf, 4)[0]
        bl = baseline_bytes if "baseline" in hdr else 0
        payload += buf[8 + h2 + bl:]

    hdr["saved_rows"] = len(payload) // stride
    hdr["saved_at"]   = 0

    hdr_bytes = json.dumps(hdr, ensure_ascii=False).encode("utf-8")
    hdr_bytes = hdr_bytes.ljust(HDR_RESERVE)

    with open(out_path, "wb") as f:
        f.write(b"ASWF")
        f.write(struct.pack("<I", HDR_RESERVE))
        f.write(hdr_bytes)
        f.write(baseline_data)
        f.write(payload)
```

---

## Граничные случаи

| Ситуация | Поведение |
|----------|-----------|
| `saved_rows == 0` | Файл открыт. Для несжатых: вычислить из размера. |
| `saved_at == 0` | Время финализации неизвестно. |
| `duration == 0` | Подставить `interval_sec`. |
| `started_at == 0` | NTP не синхронизирован. Временна́я ось относительна. |
| `timestamp == 0` | Метка строки неизвестна (v3). Реконструировать через `started_at`. |
| `latitude/longitude == NaN` | Нет GPS-фикса. |
| `dose_rate_usv_h == NaN` | Дозиметр недоступен. |
| `temperature == NaN` | Прибор ещё не отдал температуру (v5). |
| `crc32` не совпал | Строка повреждена при передаче/хранении (v4+). |
| Неизвестные ключи JSON | Игнорировать (расширяемость). |
| Неизвестные поля в `row_fields` | Пропустить (неизвестный `offset`+`dtype`). |
| `hlen` ≠ 4096 | Использовать фактический `hlen`. |
| Усечённый хвост | `len(payload) % stride ≠ 0` → отбросить хвост. |
| `compressed: true` + baseline | Baseline без изменений (всегда несжатый). |

---

## Совместимость версий

| Парсер ↓ / Файл → | v1 | v2 | v3 (без baseline) | v3 (с baseline) |
|---|:---:|:---:|:---:|:---:|
| v1-парсер | ✅ | ⚠ (лишние 2 байта/строка → сдвиг) | ⚠ | ❌ |
| v2-парсер | ✅ | ✅ | ✅ (спектр+duration OK, новые поля пропускаются) | ❌ (baseline воспринимается как строки) |
| v3-парсер | ✅ | ✅ | ✅ | ✅ |

**v4 / v5:** парсер, который берёт `row_stride` из заголовка и смещения полей из
`row_fields`, открывает v4/v5 без правок — новые поля (`crc32`, `temperature`) описаны
в `row_fields`, неизвестные игнорируются. Парсер с хардкодом `stride=16402` прочитает
v4/v5 сдвинутой кашей — **всегда брать `row_stride` из шапки, offset'ы — из `row_fields`.**

Рекомендация: всегда проверять `"version"` перед парсингом.

---

## Таблица размеров строк

| Версия | Поля | Размер строки |
|--------|------|--------------|
| v1 | 8192 × uint16 | 16384 байт |
| v2 | спектр + duration (uint16) | 16386 байт |
| v3 minimal | спектр + duration + timestamp | 16390 байт |
| v3 full | + latitude + longitude + dose_rate | 16402 байт |
| v4 | v3 full + crc32 | 16406 байт |
| v5 | v3 full + temperature + crc32 | 16410 байт |
| v3/v5 compressed | переменная | — |
