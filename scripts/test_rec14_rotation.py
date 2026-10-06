#!/usr/bin/env python3
# -*- coding: utf-8 -*-
r"""#REC-14 smoke-тест авто-ротации файла шва при смене формата прошивки.

Без живой платы: синтезируем два ASWF-blob'а с разным row_stride (имитация
v3 stride=16402 → v5 stride=16410) и проверяем, что Stitcher:
  1. первый (старый формат) создаёт файл шва;
  2. второй (новый формат) НЕ роняет запись, а ротирует в <base>__s<stride>.aswf;
  3. старый файл остаётся замороженным (размер не растёт);
  4. новый файл — валидный ASWF нового формата;
  5. при «рестарте» (новый Stitcher на старый путь) уже вшитый сегмент не
     дублируется (dedup после ротации).

Запуск:  python test_rec14_rotation.py
"""
import json
import os
import struct
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import wf_pull_client as wpc  # noqa: E402


def make_blob(ch, stride, n_rows, dur=5, cal="A"):
    """Минимальный валидный ASWF: spectrum@0, duration@(ch*2), хвост до stride."""
    rf = [{"name": "spectrum", "dtype": "uint16", "channels": ch, "offset": 0},
          {"name": "duration", "dtype": "uint16", "unit": "sec", "offset": ch * 2}]
    hdr = {"format": "aswf", "channels": ch, "row_stride": stride,
           "row_fields": rf, "calibration": cal, "started_at": 1000000000}
    hj = json.dumps(hdr).encode("utf-8")
    head = b"ASWF" + struct.pack("<I", len(hj)) + hj
    payload = bytearray()
    for _ in range(n_rows):
        row = bytearray(stride)
        struct.pack_into("<H", row, ch * 2, dur)  # duration в хвосте строки
        payload += row
    return head + bytes(payload)


def main():
    d = tempfile.mkdtemp(prefix="rec14_")
    p = os.path.join(d, "spectrogram.aswf")
    st = wpc.Stitcher(p)

    # --- 1. старый формат (ch=2, stride=6) создаёт файл шва -------------------
    old = make_blob(2, 6, 3, cal="A")
    rows0, _, _ = st.append_segment("seg_00000.aswf", old)
    assert rows0 == 3, f"old: ожидал 3 строки, got {rows0}"
    assert st.path == p, f"путь уехал до ротации: {st.path}"
    size_old = os.path.getsize(p)
    print(f"[1] старый формат: {rows0} строк -> {os.path.basename(p)} (stride 6)")

    # --- 2/3. новый формат (stride=8) ротирует, старый заморожен --------------
    new = make_blob(2, 8, 4, cal="A")
    rows1, _, _ = st.append_segment("seg_00001.aswf", new)
    exp = os.path.join(d, "spectrogram__s8.aswf")
    assert st.path == exp, f"ротации не было, path={st.path}"
    assert os.path.exists(exp), "новый файл шва не создан"
    assert rows1 == 4, f"new: ожидал 4 строки, got {rows1}"
    assert os.path.getsize(p) == size_old, "СТАРЫЙ файл вырос — не заморожен!"
    print(f"[2] новый формат: ротация -> {os.path.basename(exp)} (stride 8), {rows1} строк")
    print(f"[3] старый файл заморожен: {size_old} B без изменений")

    # --- 4. новый файл — валидный ASWF нового формата ------------------------
    with open(exp, "rb") as f:
        h, _, pay = wpc.parse_aswf(f.read(), "chk")
    assert h["row_stride"] == 8, f"новый stride в шапке {h['row_stride']} != 8"
    assert len(pay) % 8 == 0 and len(pay) // 8 == 4, "payload нового файла не кратен stride"
    print(f"[4] новый файл валиден: stride={h['row_stride']}, строк={len(pay)//8}")

    # --- 5. рестарт: уже вшитый сегмент не дублируется -----------------------
    st2 = wpc.Stitcher(p)                       # как перезапуск GUI: снова v3-путь
    size_new = os.path.getsize(exp)
    rows2, _, _ = st2.append_segment("seg_00001.aswf", new)  # он уже в __s8
    assert rows2 == 0, f"dedup: ожидал 0 строк, got {rows2}"
    assert st2.path == exp, "рестарт не привёл к ротации на новый файл"
    assert os.path.getsize(exp) == size_new, "ДУБЛЬ: новый файл вырос при повторе!"
    print(f"[5] рестарт-дедуп: сегмент не продублирован ({size_new} B без изменений)")

    print("\nOK — все 5 проверок #REC-14 пройдены")


if __name__ == "__main__":
    main()
