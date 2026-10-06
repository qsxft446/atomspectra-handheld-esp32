#!/usr/bin/env python3
"""Инспектор .aswf-сегментов: заголовок, целостность, статистика строк, анализ границ.

Использование:
  python aswf_inspect.py file1.aswf [file2.aswf ...]     # локальные файлы
  python aswf_inspect.py --board http://<host>           # скачать все finalized с платы и разобрать

Для каждого файла: JSON-заголовок, bytes vs 8+hdr+rows*ch*2, суммы счётов
первых/последних строк. Между файлами: сравнение последней строки сегмента N
и первой строки N+1 (корреляция соседних строк внутри vs через границу).
"""
import json
import struct
import sys
import urllib.request
from pathlib import Path

sys.stdout.reconfigure(encoding="utf-8")

OPENER = urllib.request.build_opener(urllib.request.ProxyHandler({}))


def parse_aswf(buf: bytes, name: str):
    assert buf[:4] == b"ASWF", f"{name}: bad magic {buf[:4]!r}"
    hlen = struct.unpack_from("<I", buf, 4)[0]
    hdr = json.loads(buf[8:8 + hlen].decode("utf-8"))
    ch = hdr["channels"]
    row_bytes = ch * 2
    # v2: row_stride = counts + хвост-длительность (16386); legacy v1 без поля = row_bytes
    stride = hdr.get("row_stride", row_bytes)
    if not (row_bytes <= stride <= row_bytes + 64):
        stride = row_bytes
    base = 0
    if "baseline" in hdr:  # v4 baseline-блок (uint32×channels) между заголовком и строками — пропустить, иначе сдвиг строк
        base = hdr["baseline"].get("channels", hdr["baseline"].get("count", 0)) * 4
    payload = buf[8 + hlen + base:]
    n_rows = len(payload) // stride
    rem = len(payload) % stride
    rows = [struct.unpack_from(f"<{ch}H", payload, i * stride) for i in range(n_rows)]
    return hdr, hlen, rows, rem


def row_sum(r):
    return sum(r)


def corr_shift(a, b, max_shift=32):
    """Грубая оценка сдвига каналов: argmax скалярного произведения при сдвиге."""
    best, best_s = -1.0, 0
    n = len(a)
    for s in range(-max_shift, max_shift + 1):
        lo, hi = max(0, s), min(n, n + s)
        dot = sum(a[i] * b[i - s] for i in range(lo, hi, 8))  # прореживание x8 — скорость
        if dot > best:
            best, best_s = dot, s
    return best_s


def inspect(files):
    all_rows = []  # (fname, row_index_in_file, row)
    for f in files:
        buf = Path(f).read_bytes() if isinstance(f, (str, Path)) else f[1]
        name = str(f) if isinstance(f, (str, Path)) else f[0]
        hdr, hlen, rows, rem = parse_aswf(buf, name)
        sums = [row_sum(r) for r in rows]
        print(f"\n=== {name} ===")
        print(f"  header_len={hlen}  header={json.dumps(hdr, ensure_ascii=False)}")
        print(f"  file_bytes={len(buf)}  payload_rows={len(rows)}  payload_remainder={rem}"
              + ("  ⚠ НЕКРАТНЫЙ ХВОСТ!" if rem else ""))
        sr = hdr.get("saved_rows")
        # #FW-14: saved_rows=0 = штатная конвенция «строк — из размера файла»
        if sr:
            if sr != len(rows):
                print(f"  ⚠ saved_rows={sr} ≠ строк в payload={len(rows)}")
        else:
            print(f"  saved_rows=0 (derive-from-size, #FW-14) → rows={len(rows)}")
        if sums:
            print(f"  суммы строк: min={min(sums)} max={max(sums)} "
                  f"первая={sums[0]} вторая={sums[1] if len(sums)>1 else '-'} "
                  f"предпосл={sums[-2] if len(sums)>1 else '-'} последняя={sums[-1]}")
        for i, r in enumerate(rows):
            all_rows.append((name, i, r))

    print("\n=== ГРАНИЦЫ ===")
    prev = None
    for name, i, r in all_rows:
        if prev is not None:
            pn, pi, pr = prev
            boundary = pn != name
            if boundary or pi in (0, 1) or i <= 1:
                s = corr_shift(pr, r)
                tag = "ГРАНИЦА" if boundary else "внутри"
                print(f"  {pn}[{pi}] -> {name}[{i}]  {tag}: сдвиг≈{s} кан., "
                      f"суммы {row_sum(pr)} -> {row_sum(r)}")
        prev = (name, i, r)


def fetch_board(board):
    board = board.rstrip("/")
    with OPENER.open(board + "/api/waterfall/segments", timeout=20) as r:
        segs = json.loads(r.read())
    out = []
    for s in sorted([x for x in segs if x.get("finalized")], key=lambda x: x["idx"]):
        with OPENER.open(board + "/api/waterfall/segment?name=" + s["name"], timeout=60) as r:
            buf = r.read()
        print(f"скачан {s['name']}: {len(buf)} байт (листинг {s['bytes']})"
              + ("  ⚠ РАЗМЕР НЕ СОВПАЛ" if len(buf) != s["bytes"] else ""))
        out.append((s["name"], buf))
    return out


if __name__ == "__main__":
    args = sys.argv[1:]
    if args and args[0] == "--board":
        inspect(fetch_board(args[1] if len(args) > 1 else "http://atomspectra.local"))
    else:
        inspect(args)
