# ASWF File Format — Integration Specification

[🇷🇺 Русская версия](ASWF_FORMAT.md) · [← README](README.en.md) · [Waterfall](WATERFALL.en.md)

**ASWF** (AtomSpectra Waterfall Format) is a binary format for storing a spectrogram
(waterfall): a chronological sequence of spectra captured at fixed intervals.
Each `.aswf` file is self-contained — it carries its full header and data, requiring
no external schema to parse.

---

## Version History

| Version | Key Changes |
|---------|-------------|
| **v1** | Base format. 16384 bytes/row (8192 × uint16 LE). No duration field. |
| **v2** | +2-byte uint16 LE duration appended to each row. `row_stride=16386`, `row_time` in header. |
| **v3** | Self-describing `row_fields`. Per-row timestamp, GPS, dose rate. Baseline section. Compression flag. `row_stride=16402`. |
| **v4** | +per-row `crc32` (row integrity, `covers=16402`). `row_stride=16406`. Header gains `seg_seq` and `total_at_open` (dropped-segment detection and reconciliation). |
| **v5** | +`temperature` (float32, detector °C) before `crc32` → `crc32.offset/covers` shift to `16406`. `row_stride=16410`. **Current firmware format** (`firmware-v1.0.11`, #FW-41). |

---

## File Structure

### v1 / v2

```
Offset 0:      4 bytes   Magic "ASWF" (ASCII)
Offset 4:      4 bytes   uint32 LE: hlen (reserved JSON area size)
Offset 8:      hlen bytes JSON header (UTF-8, space-padded to hlen)
Offset 8+hlen: N × stride data rows (oldest first)
```

### v3

```
Offset 0:          4 bytes     Magic "ASWF" (ASCII)
Offset 4:          4 bytes     uint32 LE: hlen
Offset 8:          hlen bytes  JSON header (UTF-8, space-padded to hlen)
Offset 8+hlen:     B bytes     Baseline spectrum (if "baseline" key present; otherwise 0)
Offset 8+hlen+B:   N × stride  Data rows (oldest first)
```

where `B = baseline.channels × 4` (uint32 per channel; early files used the key `count`),
or `0` if the section is absent.

> **Important:** `hlen` is the reserved JSON area size, not the actual JSON text length.
> Current value is always `4096`. Payload offset = `8 + hlen + B`.
> Parsers must use `hlen` and `B`, not scan for `}`.

---

## JSON Header

The header is fully self-describing — every field needed for decoding is present inside the file.

### Fields (all versions)

| Key            | Type             | Required | Description |
|----------------|------------------|:---:|------|
| `format`       | string           | yes | Always `"atomspectra-waterfall"` |
| `version`      | int              | yes | `1`…`5` (current firmware writes `5`) |
| `channels`     | int              | yes | Channels per row; current value `8192` |
| `dtype`        | string           | yes | Spectrum element type: `"uint16"` |
| `byte_order`   | string           | yes | Byte order: `"little"` |
| `interval_sec` | int              | yes | Nominal recording interval, seconds |
| `started_at`   | int (unix ts)    | yes | Timestamp of first row (UTC, epoch seconds); `0` = NTP not synced |
| `saved_rows`   | int              | yes | Rows in file; `0` for an open/unfinalised file |
| `saved_at`     | int (unix ts)    | yes | Finalisation timestamp; `0` for an open file |
| `serial`       | string           | no  | Device serial number |
| `calibration`  | array of floats  | no  | Energy calibration polynomial coefficients |

### Additional v2 Fields

| Key          | Type   | Description |
|--------------|--------|-------------|
| `row_stride` | int    | Row size in bytes (`16386`). Absent in v1 |
| `row_time`   | object | `{"dtype":"uint16","unit":"sec","offset":16384}` |

### Additional v3 Fields

| Key           | Type   | Description |
|---------------|--------|-------------|
| `row_fields`  | array  | Per-row field descriptors (see below) |
| `row_stride`  | int    | Total row size in bytes (derived from `row_fields`) |
| `compressed`  | bool   | `true` = rows are RLE-compressed (default `false`) |
| `baseline`    | object | Baseline section descriptor (see below) |

### `row_fields` Object (v3)

Array of field descriptors. Parsers read only described fields; unknown fields are ignored.

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

| Field              | Type    | Description |
|--------------------|---------|-------------|
| `spectrum`         | uint16  | Spectrum: `channels` channels LE, counts per interval |
| `duration`         | uint16  | Actual live time in seconds (0 = use `interval_sec`) |
| `timestamp`        | uint32  | Unix timestamp of row start (UTC, seconds); 0 = unknown |
| `latitude`         | float32 | Decimal degrees; NaN = no data |
| `longitude`        | float32 | Decimal degrees; NaN = no data |
| `dose_rate_usv_h`  | float32 | Dose rate in µSv/h; NaN = no data |

Fields `latitude`, `longitude`, `dose_rate_usv_h`, `timestamp` are optional.
If the device has no GPS or dosimeter, the corresponding descriptor is simply absent from `row_fields`.

### `baseline` Object (v3+)

```json
"baseline": {
  "dtype":      "uint32",
  "channels":   8192,
  "byte_order": "little"
}
```

Baseline is the device's cumulative spectrum at the **start** of the current recording session.
Physically: `channels` × uint32 LE = 32768 bytes, stored between the JSON area and the payload
(size `B = baseline.channels × 4`).

> **Naming:** current firmware writes the key `channels`; early examples used `count` —
> parsers must accept both (`baseline.channels ?? baseline.count`). There is no `offset`
> key in the object — the offset is always `8 + hlen`.

Useful for absolute spectrum reconstruction:
`total[ch] = baseline[ch] + sum of all waterfall rows[ch]`.

### Additional v4 / v5 Fields

**v4** adds per-row integrity checking; **v5** adds detector temperature.

| row_fields entry | Introduced | Type | Offset (v5) | Description |
|---|---|---|---|---|
| `temperature` | v5 | float32 | 16402 | Detector temperature, °C; `NaN` (`0x7FC00000`) until the device reports it |
| `crc32`       | v4 | uint32  | 16406 | Row CRC32; `covers` = number of leading bytes covered |

In v4, `crc32` sat at offset `16402` (right after `dose_rate`), `covers=16402`. In v5 a
`temperature` field is inserted before it, so `crc32.offset = covers = 16406`. **Parsers
must take `offset`/`covers` from `row_fields`, never hardcode them** — otherwise a version
bump breaks parsing.

### Header Fields v4+ (Integrity)

| Key | Type | Description |
|---|---|---|
| `seg_seq`       | int (uint32) | Global monotonic segment number (survives reboot/clear via NVS). A gap in the sequence of merged segments means a segment was lost — the ring erased it before offload (#DATA-1b) |
| `total_at_open` | int (uint32) | Device cumulative counter when the segment was opened. The delta between adjacent segments is reconciled against the Σ counts of the previous one (#DATA-1c) |

### Row CRC32 (v4+)

Standard CRC32: init `0xFFFFFFFF`, polynomial `0xEDB88320` (reflected), final XOR
`0xFFFFFFFF` — zlib-compatible. The `crc32` field covers the first `covers` bytes of the row
(everything before the field itself). Verification:

```python
import zlib
def row_crc_ok(row_bytes, covers, stored_crc):
    return (zlib.crc32(row_bytes[:covers]) & 0xFFFFFFFF) == stored_crc
```

A mismatch means the row was corrupted in transit/storage. `covers` comes from the `crc32`
descriptor in `row_fields` (v4 → 16402, v5 → 16406).

### Example v5 Header

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

## Spectrum Data

Each channel value in a row is a **delta** of the cumulative spectrum: counts during this interval. Type `uint16` — values `0–65535`.

### Absolute Spectrum (sum over all rows)

```python
absolute = [0] * channels
for row in rows:
    for ch in range(channels):
        absolute[ch] += row[ch]
```

With baseline (v3): total accumulated spectrum = `baseline[ch] + absolute[ch]`.

### Count Rate (counts/s) for Row i

```python
dur_i = duration[i] if duration[i] > 0 else interval_sec
rate  = [row[i][ch] / dur_i for ch in range(channels)]
```

---

## Row Timestamps

### v1 / v2 — Reconstruct via `started_at`

```
t[0] = started_at
t[i] = started_at + sum(duration[0..i-1])
```

For row `j` where `duration[j] == 0`, substitute `interval_sec`.
When `started_at == 0` (NTP not synced), the time axis is relative only.

### v3 — Direct `timestamp` Field

If `timestamp` is present in `row_fields`, the row timestamp is read directly from the row (uint32, UTC, seconds). `0` = unknown; fall back to reconstruction from `started_at`.

---

## Compression (v3, `compressed: true`)

When `compressed: true`, the spectrum portion of each row is stored in **RLE encoding**.
Non-spectrum fields (`duration`, `timestamp`, GPS, `dose_rate`) are always uncompressed,
appended after the spectrum stream.

### RLE Stream Format

The stream consists of uint16 LE elements:
- Value `< 0x8000` → **literal**: one channel with value `v`
- Value `0x8000..0xFFFE` → **run**: `v & 0x7FFF` consecutive zero channels
- Value `0xFFFF` → reserved

The stream ends when exactly `channels` channels have been decoded.

> **Limitation:** maximum per-channel value = 32767 (`0x7FFF`). Files where any channel
> exceeds this value must use `compressed: false`.

**Compression breaks random row access** — payload must be read sequentially from the start.
For `compressed: false` (default), random access by row index works normally.

When `compressed: true`, `row_stride` is **absent** (variable row length);
row count cannot be derived from file size — sequential decoding required.

---

## Version Detection and Offsets

```python
import json, struct
from pathlib import Path

def open_aswf(path):
    buf  = Path(path).read_bytes()
    assert buf[:4] == b"ASWF", f"Not ASWF: {buf[:4]!r}"
    hlen = struct.unpack_from("<I", buf, 4)[0]
    hdr  = json.loads(buf[8:8 + hlen].decode("utf-8"))
    ver  = hdr.get("version", 1)

    baseline_bytes = 0
    if "baseline" in hdr:
        bl = hdr["baseline"]
        baseline_bytes = (bl.get("channels") or bl.get("count") or 0) * 4

    payload_off = 8 + hlen + baseline_bytes

    if ver == 1:
        stride, compressed = hdr["channels"] * 2, False
    elif ver == 2:
        stride, compressed = hdr.get("row_stride", hdr["channels"] * 2), False
    else:  # v3 / v4 / v5 — stride and field offsets always come from the header (row_stride/row_fields)
        compressed = hdr.get("compressed", False)
        stride = hdr.get("row_stride", 0) if not compressed else 0

    n_rows = hdr.get("saved_rows") or (
        (len(buf) - payload_off) // stride if stride else None
    )
    return hdr, buf, payload_off, stride, n_rows, compressed
```

---

## Python: Full Parser (v1–v5)

```python
import json, struct, math
from pathlib import Path

def read_aswf(path):
    """Read an .aswf file (v1–v5), return (header, rows, baseline).

    rows  -- list of dicts with keys from row_fields:
      'spectrum'    : tuple of int
      'duration'    : int (seconds; 0 = use interval_sec)
      'timestamp'   : int (unix ts; 0 = unknown)    [v3+]
      'latitude'    : float (NaN = no data)         [v3+]
      'longitude'   : float (NaN = no data)         [v3+]
      'dose_rate'   : float (NaN = no data)         [v3+]
      'temperature' : float (NaN = no data, °C)     [v5+]
      'crc32'       : int (row CRC32, see covers)   [v4+]
    baseline -- tuple of uint32 (or None if absent)
    """
    buf  = Path(path).read_bytes()
    assert buf[:4] == b"ASWF", f"Not ASWF: {buf[:4]!r}"

    hlen = struct.unpack_from("<I", buf, 4)[0]
    hdr  = json.loads(buf[8:8 + hlen].decode("utf-8"))
    ver  = hdr.get("version", 1)
    ch   = hdr["channels"]
    iv   = hdr["interval_sec"]

    baseline = None
    baseline_bytes = 0
    if "baseline" in hdr:
        bi = hdr["baseline"]
        nbl = bi.get("channels") or bi.get("count") or 0
        baseline_bytes = nbl * 4
        baseline = struct.unpack_from(f"<{nbl}I", buf, 8 + hlen)

    payload_off = 8 + hlen + baseline_bytes
    payload     = buf[payload_off:]

    if ver >= 3 and "row_fields" in hdr:
        fields     = {f["name"]: f for f in hdr["row_fields"]}
        stride     = hdr.get("row_stride", 0)
        compressed = hdr.get("compressed", False)
    else:
        fields     = None
        stride     = hdr.get("row_stride", ch * 2)
        compressed = False

    def get_field(rb, off, dtype):
        if dtype == "uint16":  return struct.unpack_from("<H", rb, off)[0]
        if dtype == "uint32":  return struct.unpack_from("<I", rb, off)[0]
        if dtype == "float32": return struct.unpack_from("<f", rb, off)[0]
        raise ValueError(dtype)

    rows = []
    if not compressed:
        n = len(payload) // stride
        for i in range(n):
            rb  = payload[i * stride:(i + 1) * stride]
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
            if fields:
                for name, fd in fields.items():
                    if name == "spectrum": continue
                    row[name] = get_field(raw, pos, fd["dtype"]); pos += 4
            rows.append(row)

    return hdr, rows, baseline


def row_timestamp(hdr, rows, index):
    """Unix timestamp for the start of row `index`."""
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

## Energy Calibration: Channel → keV

```
E(ch) = a[0] + a[1]·ch + a[2]·ch² + …
```

`a = calibration` (coefficient array, index = polynomial degree).
If absent, use channel number as the X axis.

---

## Device HTTP API

### List Segments

```
GET /api/waterfall/segments
```

Response `200 application/json`:

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

`finalized: false` — segment still open; `saved_rows=0`, compute row count from file size.

### Download a Segment

```
GET /api/waterfall/segment?name=seg_00000.aswf
Authorization: Basic <base64(login:password)>
```

Response `200 application/octet-stream`.

### Delete a Segment (confirm receipt)

```
POST /api/waterfall/segment/delete?name=seg_00000.aswf
Authorization: Basic <base64(login:password)>
```

### Export RAM Ring as ASWF

```
GET /api/waterfall/export.aswf
Authorization: Basic <base64(login:password)>
```

Response `200 application/octet-stream` — complete ASWF file of the current version (firmware currently writes v5).

Exports the current PSRAM ring (last `ring_capacity` rows) as a single ASWF v5 file.
No baseline section (ring is a rolling window, not a full session).
Row fields: `spectrum`, `duration`, `timestamp`, `latitude` (NaN), `longitude` (NaN), `dose_rate`, `temperature`, `crc32`.
Returns `404` if no data has been recorded yet.

> **Note on field naming:** The device firmware uses `"name":"dose_rate"` with `"unit":"usv_h"` in `row_fields`;
> this doc uses the alias `dose_rate_usv_h` for clarity. Parsers should handle both names.

---

## Segment Ring

Each segment: up to 64 rows (~1 MB payload), at most 10 minutes of recording.
When the ring is full, the oldest finalised segment is deleted automatically.
The open segment (`finalized: false`) does not count toward the limit.

---

## Merging Segments

> **Firmware format change (#REC-14):** segments with different `row_stride` (e.g. v3 `16402`
> and v5 `16410`) **must not** be merged into one file — the reader lays the waterfall out by a
> single `row_stride` from the header, so the tail after the seam becomes garbage. The recorder
> routes mismatched formats into separate files `<base>__s<stride>.aswf`; the code below merges
> segments of **one** format.

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

## Edge Cases

| Situation | Behaviour |
|-----------|-----------|
| `saved_rows == 0` | File open. For uncompressed: compute from file size. |
| `saved_at == 0` | Finalisation time unknown. |
| `duration == 0` | Substitute `interval_sec`. |
| `started_at == 0` | NTP not synced. Time axis is relative only. |
| `timestamp == 0` | Row timestamp unknown (v3). Reconstruct via `started_at`. |
| `latitude/longitude == NaN` | No GPS fix. |
| `dose_rate_usv_h == NaN` | Dosimeter unavailable. |
| `temperature == NaN` | Device hasn't reported temperature yet (v5). |
| `crc32` mismatch | Row corrupted in transit/storage (v4+). |
| Unknown JSON keys | Ignore (extensibility). |
| Unknown `row_fields` entries | Skip (unknown `offset`+`dtype`). |
| `hlen` ≠ 4096 | Use the actual `hlen` from the file. |
| Truncated last row | `len(payload) % stride ≠ 0` → discard the tail. |
| `compressed: true` + baseline | Baseline is always uncompressed. |

---

## Version Compatibility

| Parser ↓ / File → | v1 | v2 | v3 (no baseline) | v3 (with baseline) |
|---|:---:|:---:|:---:|:---:|
| v1 parser | ✅ | ⚠ (2 extra bytes/row → offset drift) | ⚠ | ❌ |
| v2 parser | ✅ | ✅ | ✅ (spectrum+duration OK, new fields ignored) | ❌ (baseline read as rows) |
| v3 parser | ✅ | ✅ | ✅ | ✅ |

Always check `"version"` before parsing.

---

## Row Size Reference

| Version | Fields | Row Size |
|---------|--------|----------|
| v1 | 8192 × uint16 | 16384 bytes |
| v2 | spectrum + duration (uint16) | 16386 bytes |
| v3 minimal | + timestamp (uint32) | 16390 bytes |
| v3 full | + latitude + longitude + dose_rate | 16402 bytes |
| v4 | v3 full + crc32 | 16406 bytes |
| v5 | v3 full + temperature + crc32 | 16410 bytes |
| v3/v5 compressed | variable | — |
