#!/usr/bin/env python3
# -*- coding: utf-8 -*-
r"""#REC-13: детерминированный offline-тест v4-контроля целостности в сборщике.

Крафтит синтетические v4-сегменты (та же раскладка строки, что пишет
spectrogram.c: spectrum | dur | ts | lat | lon | dose | crc32) и прогоняет их
через wf_pull_client.Stitcher.append_segment, проверяя diag:
  - чистый сегмент      -> crc_bad=0, seq_gap непрерывен, recon без потерь;
  - битый CRC           -> crc_bad>0;
  - разрыв seg_seq      -> seq_gap>0;
  - недостача Σbins      -> recon с потерей событий.

Запуск: python test_rec13_integrity.py   (без платы, без сети)
"""
import json
import os
import struct
import sys
import tempfile
import zlib

sys.stdout.reconfigure(encoding="utf-8")
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import wf_pull_client as wpc  # noqa: E402

CH = 4                      # мало каналов -> компактная строка (структура та же)
WF_ROW_BYTES = CH * 2       # spectrum
# tail: dur(2) ts(4) lat(4) lon(4) dose(4) crc(4) — как в firmware
DUR_OFF = WF_ROW_BYTES
TS_OFF = DUR_OFF + 2
LAT_OFF = TS_OFF + 4
LON_OFF = LAT_OFF + 4
DOSE_OFF = LON_OFF + 4
CRC_OFF = DOSE_OFF + 4      # covers = всё до crc
COVERS = CRC_OFF
STRIDE = CRC_OFF + 4


def make_header(seg_seq, total_at_open, started_at=2_000_000_000):
    hdr = {
        "version": 4,
        "channels": CH,
        "row_stride": STRIDE,
        "saved_rows": 0,                 # #FW-14 derive-from-size
        "seg_seq": seg_seq,
        "total_at_open": total_at_open,
        "started_at": started_at,
        "interval_sec": 60,
        "calibration": [0.0, 1.0, 0.0],
        "row_fields": [
            {"name": "spectrum", "offset": 0, "type": "u16", "count": CH},
            {"name": "duration", "offset": DUR_OFF, "type": "u16"},
            {"name": "timestamp", "offset": TS_OFF, "type": "u32"},
            {"name": "latitude", "offset": LAT_OFF, "type": "f32"},
            {"name": "longitude", "offset": LON_OFF, "type": "f32"},
            {"name": "dose_rate", "offset": DOSE_OFF, "type": "f32"},
            {"name": "crc32", "offset": CRC_OFF, "covers": COVERS, "type": "u32"},
        ],
    }
    return hdr


def make_row(bins, dur=60, ts=0, corrupt=False):
    """bins: список из CH значений uint16. Возвращает bytes длиной STRIDE."""
    row = bytearray(STRIDE)
    for i, v in enumerate(bins):
        struct.pack_into("<H", row, i * 2, v & 0xFFFF)
    struct.pack_into("<H", row, DUR_OFF, dur)
    struct.pack_into("<I", row, TS_OFF, ts)
    struct.pack_into("<f", row, LAT_OFF, 0.0)
    struct.pack_into("<f", row, LON_OFF, 0.0)
    struct.pack_into("<f", row, DOSE_OFF, 0.0)
    crc = zlib.crc32(bytes(row[:COVERS])) & 0xFFFFFFFF
    if corrupt:
        crc ^= 0xDEADBEEF            # ломаем контрольную сумму
    struct.pack_into("<I", row, CRC_OFF, crc)
    return bytes(row)


def make_segment(hdr, rows):
    hj = json.dumps(hdr, ensure_ascii=False).encode("utf-8")
    return b"ASWF" + struct.pack("<I", len(hj)) + hj + b"".join(rows)


def approx_stitcher():
    d = tempfile.mkdtemp(prefix="rec13_")
    return wpc.Stitcher(os.path.join(d, "s.aswf")), d


def main():
    fails = []

    # --- 1) чистый сегмент: crc_bad=0, seq непрерывен (первый — seq_gap None) ---
    st, _ = approx_stitcher()
    seg0 = make_segment(make_header(seg_seq=10, total_at_open=1000),
                        [make_row([1, 2, 3, 4]), make_row([5, 6, 7, 8])])
    rows, gap, diag = st.append_segment("seg0", seg0)
    assert diag["crc_checked"] == 2, diag
    if diag["crc_bad"] != 0:
        fails.append(f"1: чистый CRC_bad={diag['crc_bad']} (ждали 0)")
    print(f"[1] чистый: rows={rows} crc {diag['crc_checked']-diag['crc_bad']}/"
          f"{diag['crc_checked']} OK, seq_gap={diag['seq_gap']}")

    # --- 2) следующий сегмент непрерывен (seq=11), recon сверка пред. Σbins=36 ---
    #   Σbins seg0 = 1+2+3+4+5+6+7+8 = 36. total_at_open растёт 1000->1036 = ровно.
    seg1 = make_segment(make_header(seg_seq=11, total_at_open=1036),
                        [make_row([2, 2, 2, 2])])
    rows, gap, diag = st.append_segment("seg1", seg1)
    if diag["seq_gap"] != 0:
        fails.append(f"2: seq_gap={diag['seq_gap']} (ждали 0, непрерывно)")
    rc = diag["recon"]
    if rc is None or rc[2] != 0:
        fails.append(f"2: recon={rc} (ждали точную сверку d=0)")
    print(f"[2] непрерывный: seq_gap={diag['seq_gap']} recon={rc}")

    # --- 3) битый CRC в строке ---
    st3, _ = approx_stitcher()
    segb = make_segment(make_header(seg_seq=1, total_at_open=0),
                        [make_row([1, 1, 1, 1]),
                         make_row([9, 9, 9, 9], corrupt=True)])
    rows, gap, diag = st3.append_segment("segb", segb)
    if diag["crc_bad"] != 1:
        fails.append(f"3: crc_bad={diag['crc_bad']} (ждали 1)")
    print(f"[3] битый CRC: crc_bad={diag['crc_bad']}/{diag['crc_checked']}")

    # --- 4) разрыв seg_seq (пропуск 2 сегментов кольцом) ---
    st4, _ = approx_stitcher()
    st4.append_segment("a", make_segment(make_header(seg_seq=5, total_at_open=0),
                                         [make_row([1, 0, 0, 0])]))
    _, _, diag = st4.append_segment("b", make_segment(
        make_header(seg_seq=8, total_at_open=500), [make_row([1, 0, 0, 0])]))
    # 8 - 5 - 1 = 2 пропущено
    if diag["seq_gap"] != 2:
        fails.append(f"4: seq_gap={diag['seq_gap']} (ждали 2)")
    print(f"[4] разрыв seq: seq_gap={diag['seq_gap']}")

    # --- 5) недостача Σbins: recon d>0 (#DATA-2: справочная метрика, не вердикт) ---
    st5, _ = approx_stitcher()
    # seg A: Σbins=4, total_at_open=0
    st5.append_segment("a", make_segment(make_header(seg_seq=1, total_at_open=0),
                                         [make_row([1, 1, 1, 1])]))
    # seg B непрерывен seq=2, прибор насчитал +100 (total 104), но пред. Σbins было 4
    #   -> device_delta = 104-0 = 104 > Σbins 4 -> потеря 100
    _, _, diag = st5.append_segment("b", make_segment(
        make_header(seg_seq=2, total_at_open=104), [make_row([0, 0, 0, 0])]))
    rc = diag["recon"]
    if rc is None or rc[2] <= 0:
        fails.append(f"5: recon={rc} (ждали потерю d>0)")
    print(f"[5] недостача Σbins: recon={rc} (d>0; #DATA-2 — справочно, не вердикт)")

    print()
    if fails:
        print("FAIL:")
        for f in fails:
            print("  -", f)
        sys.exit(1)
    print("RESULT: REC-13 v4-контроль целостности — ВСЕ 5 ПРОВЕРОК ЗЕЛЁНЫЕ ✅")


if __name__ == "__main__":
    main()
