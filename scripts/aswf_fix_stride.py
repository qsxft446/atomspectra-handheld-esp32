#!/usr/bin/env python3
"""Чинит заголовок .aswf с неверным row_stride (#FW-39).

Симптом: h_export_aswf прошивок ≤v1.0.5 писал в заголовок row_stride=16406
(WF_ROW_STRIDE, с crc32), но HTTP-экспорт строки шлёт БЕЗ crc32 → 16402
(WF_ROW_PRECRC). Ридеры авто-детектят stride из заголовка → парсят по 16406 →
накопительный сдвиг 4 Б/строку → NaN GPS-байты уезжают в dur/ts, спектр плывёт,
хвост выглядит «обрезанным».

Фикс: подобрать stride, кратный payload, и переписать поле "row_stride":<N> в
заголовке (та же длина строки → offset не сдвигается). Оригинал не трогаем —
пишем <name>.fixed.aswf.

Использование: python aswf_fix_stride.py "waterfall (7).aswf" [out.aswf]
"""
import json
import struct
import sys
from pathlib import Path

sys.stdout.reconfigure(encoding="utf-8")


def fix(path_in: str, path_out: str | None = None):
    buf = bytearray(Path(path_in).read_bytes())
    assert buf[:4] == b"ASWF", f"bad magic {buf[:4]!r}"
    hlen = struct.unpack_from("<I", buf, 4)[0]
    hdr = json.loads(bytes(buf[8:8 + hlen]).decode("utf-8"))
    ch = hdr["channels"]
    payload = len(buf) - 8 - hlen
    declared = hdr.get("row_stride", ch * 2)
    # верный stride = кратный payload, в окне [ch*2 .. ch*2+64]
    good = [s for s in range(ch * 2, ch * 2 + 65) if payload % s == 0]
    if not good:
        print(f"НЕ найден кратный stride (payload={payload}, ch={ch}) — файл иной структуры")
        return
    correct = good[0]
    rows = payload // correct
    print(f"{path_in}: payload={payload} declared_stride={declared} "
          f"correct_stride={correct} rows={rows} rem={payload % correct}")
    if declared == correct:
        print("  заголовок уже верный — правка не нужна")
        return
    old = f'"row_stride":{declared}'.encode()
    new = f'"row_stride":{correct}'.encode()
    assert len(old) == len(new), f"длина строки различается ({len(old)}!={len(new)}) — сдвиг offset, стоп"
    i = buf.index(old, 8, 8 + hlen)
    buf[i:i + len(old)] = new
    out = path_out or str(Path(path_in).with_suffix(".fixed.aswf"))
    Path(out).write_bytes(buf)
    print(f"  → записан {out} (row_stride {declared}→{correct}, длина файла без изменений)")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    fix(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else None)
