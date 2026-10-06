#!/usr/bin/env python3
"""Задвоение пика: сравнение счёта в штатном и смещённом каналах
по времени, сопоставление с T1 из temps.csv.

Использование:
  python aswf_k40_bimodal.py <файл.aswf> [--block N] [--ctr-a K] [--ctr-b K]
                                          [--label NAME] [--half N] [--bg-half N]

По умолчанию K-40: A=3151, B=3424.
Пример Tl-208: --ctr-a 5545 --ctr-b 6015 --label "Tl-208"
"""
import csv
import json
import struct
import sys
from datetime import datetime, timezone
from pathlib import Path

sys.stdout.reconfigure(encoding="utf-8")


def load_aswf(path):
    buf = Path(path).read_bytes()
    hlen = struct.unpack_from("<I", buf, 4)[0]
    hdr = json.loads(buf[8:8 + hlen].decode("utf-8"))
    ch = hdr["channels"]
    stride = hdr.get("row_stride", ch * 2)
    payload = buf[8 + hlen:]
    n = len(payload) // stride
    rows = [struct.unpack_from(f"<{ch}H", payload, i * stride) for i in range(n)]
    return hdr, rows


def net_count(spec, ctr, half=30, bg_half=60, ch=None):
    """Счёт в окне (ctr±half) - линейный фон по (ctr-2*half..ctr-half) и (ctr+half..ctr+2*half)."""
    if ch is None:
        ch = len(spec)
    left_lo = max(0, ctr - bg_half - half)
    left_hi = max(0, ctr - half)
    right_lo = min(ch, ctr + half + 1)
    right_hi = min(ch, ctr + bg_half + half + 1)
    bg_left = sum(spec[left_lo:left_hi]) / max(1, left_hi - left_lo)
    bg_right = sum(spec[right_lo:right_hi]) / max(1, right_hi - right_lo)
    bg_mean = (bg_left + bg_right) / 2.0
    win_lo, win_hi = max(0, ctr - half), min(ch, ctr + half + 1)
    total = sum(spec[win_lo:win_hi])
    return total - bg_mean * (win_hi - win_lo)


def load_temps(path):
    out = []
    with open(path, "r", encoding="utf-8") as f:
        rd = csv.reader(f, delimiter=";")
        next(rd)
        for row in rd:
            if len(row) < 3:
                continue
            try:
                out.append((int(row[0]), float(row[2])))
            except (ValueError, IndexError):
                continue
    out.sort()
    return out


def nearest_t1(temps, ts):
    lo, hi = 0, len(temps) - 1
    while lo < hi:
        m = (lo + hi) // 2
        if temps[m][0] < ts:
            lo = m + 1
        else:
            hi = m
    if lo > 0 and abs(temps[lo - 1][0] - ts) < abs(temps[lo][0] - ts):
        lo -= 1
    return temps[lo][1]


def main(aswf_path, B=4, ctr_a=3151, ctr_b=3424, half=30, bg_half=60,
         label="K-40", dump_csv=None):
    hdr, rows = load_aswf(aswf_path)
    ch = hdr["channels"]
    n = len(rows)
    print(f"строк={n} каналов={ch} блок={B} строк ≈ {B*hdr['interval_sec']}s  пик={label}")
    tag_a = f"A({ctr_a})"
    tag_b = f"B({ctr_b})"

    base = Path(aswf_path)
    state = json.loads(base.with_suffix(base.suffix + ".state.json").read_text(encoding="utf-8"))
    started_at = state["started_at"]
    last_end = state["last_end"]
    row_dur = (last_end - started_at) / n

    temps = load_temps(base.with_suffix(base.suffix + ".temps.csv"))

    print(f"окна: A={ctr_a}±{half} (штатно)  B={ctr_b}±{half} (смещённо)")
    ratios = []
    print(f"\n{'blk':>4} {'row':>5} {'ts_iso':>20} {'T1':>5} {'net_A':>7} {'net_B':>7} {'B/A':>6} {'состояние':>10}")

    for b0 in range(0, n, B):
        b1 = min(n, b0 + B)
        spec = [0.0] * ch
        for r in rows[b0:b1]:
            for k in range(ch):
                spec[k] += r[k]
        na = net_count(spec, ctr_a, half=half, bg_half=bg_half, ch=ch)
        nb = net_count(spec, ctr_b, half=half, bg_half=bg_half, ch=ch)
        center_row = (b0 + b1 - 1) / 2.0
        ts = started_at + int(center_row * row_dur)
        t1 = nearest_t1(temps, ts)
        iso = datetime.fromtimestamp(ts, timezone.utc).strftime("%m-%d %H:%M:%S")
        ratio = nb / na if na > 1 else float("inf") if nb > 1 else 0.0
        # состояние по доминирующему окну
        if na > nb * 1.5 and na > 5:
            state_tag = tag_a
        elif nb > na * 1.5 and nb > 5:
            state_tag = tag_b
        else:
            state_tag = "смесь"
        ratios.append((ts, t1, na, nb, ratio, state_tag))
        print(f"{b0//B:>4} {int(center_row):>5} {iso:>20} {t1:>5.1f} {na:>7.1f} {nb:>7.1f} {ratio:>6.2f} {state_tag:>10}")

    print(f"\n=== сводка по состояниям ===")
    tally = {tag_a: 0, tag_b: 0, "смесь": 0}
    per_t1 = {}
    for ts, t1, na, nb, ratio, tag in ratios:
        tally[tag] += 1
        per_t1.setdefault(t1, {tag_a: 0, tag_b: 0, "смесь": 0})
        per_t1[t1][tag] += 1
    for tag, cnt in tally.items():
        print(f"  {tag}: {cnt} блоков ({cnt*100/len(ratios):.1f}%)")
    print(f"\n=== раскладка по T1 ===")
    print(f"{'T1':>6} {tag_a:>10} {tag_b:>10} {'смесь':>8} {'%B':>6}")
    for t1 in sorted(per_t1):
        r = per_t1[t1]
        total = sum(r.values())
        pB = r[tag_b] * 100 / total if total else 0
        print(f"{t1:>6.1f} {r[tag_a]:>10} {r[tag_b]:>10} {r['смесь']:>8} {pB:>5.1f}%")

    if dump_csv:
        with open(dump_csv, "w", encoding="utf-8", newline="") as f:
            w = csv.writer(f, delimiter=";")
            w.writerow(["ts", "iso_utc", "t1", "net_a", "net_b", "ratio_b_a", "state", "label"])
            for ts, t1, na, nb, ratio, tag in ratios:
                iso = datetime.fromtimestamp(ts, timezone.utc).strftime("%Y-%m-%d %H:%M:%S")
                w.writerow([ts, iso, f"{t1:.1f}", f"{na:.1f}", f"{nb:.1f}",
                            f"{ratio:.3f}", tag, label])
        print(f"\nCSV: {dump_csv}")


if __name__ == "__main__":
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        sys.exit(1)
    aswf = args[0]
    B, ctr_a, ctr_b, half, bg_half = 4, 3151, 3424, 30, 60
    label = "K-40"
    dump_csv = None
    i = 1
    while i < len(args):
        a = args[i]
        if a == "--block": B = int(args[i+1]); i += 2
        elif a == "--ctr-a": ctr_a = int(args[i+1]); i += 2
        elif a == "--ctr-b": ctr_b = int(args[i+1]); i += 2
        elif a == "--half": half = int(args[i+1]); i += 2
        elif a == "--bg-half": bg_half = int(args[i+1]); i += 2
        elif a == "--label": label = args[i+1]; i += 2
        elif a == "--dump-csv": dump_csv = args[i+1]; i += 2
        else: i += 1
    main(aswf, B, ctr_a, ctr_b, half, bg_half, label, dump_csv)
