#!/usr/bin/env python3
"""Корреляция дрейфа пиков в .aswf с температурой детектора (temps.csv).

Использование:
  python aswf_temp_drift.py <файл.aswf> [--block N] [--peaks p1,p2,...]

Для каждого опорного пика: позиция per-block, ближайшая температура T1 по
timestamp из state.json + temps.csv, Pearson-корреляция (T1, pos).
"""
import csv
import json
import os
import struct
import sys
from pathlib import Path

sys.stdout.reconfigure(encoding="utf-8")


def load_aswf(path):
    buf = Path(path).read_bytes()
    assert buf[:4] == b"ASWF", "не .aswf"
    hlen = struct.unpack_from("<I", buf, 4)[0]
    hdr = json.loads(buf[8:8 + hlen].decode("utf-8"))
    ch = hdr["channels"]
    stride = hdr.get("row_stride", ch * 2)
    payload = buf[8 + hlen:]
    n = len(payload) // stride
    rows = [struct.unpack_from(f"<{ch}H", payload, i * stride) for i in range(n)]
    return hdr, rows


def mean_spectrum(rows, ch):
    m = [0.0] * ch
    for r in rows:
        for k in range(ch):
            m[k] += r[k]
    return m


def smooth(x, w=5):
    return [sum(x[max(0, i - w):i + w + 1]) / (min(len(x), i + w + 1) - max(0, i - w))
            for i in range(len(x))]


def find_peaks(mean_sp, ch, n_peaks=3, min_sep=80, lo=80, hi=None):
    """Находит n_peaks локальных максимумов в сглаженном спектре с мин. разносом."""
    if hi is None:
        hi = min(4000, ch - 80)
    sm = smooth(mean_sp, w=7)
    # Локальные максимумы: sm[i] строго > соседей в окне ±3
    lm = []
    for i in range(lo, hi):
        w = 3
        if all(sm[i] >= sm[i + d] for d in range(-w, w + 1) if d != 0):
            lm.append((i, sm[i]))
    lm.sort(key=lambda x: -x[1])
    peaks = []
    for i, _ in lm:
        if all(abs(i - p) >= min_sep for p in peaks):
            peaks.append(i)
        if len(peaks) == n_peaks:
            break
    peaks.sort()
    return peaks


def net_centroid(spec, p, half=25, ch=None):
    """Центроид пика @p (±half) с вычитанием линейного фона по боковым окнам."""
    if ch is None:
        ch = len(spec)
    wl0, wl1 = max(0, p - 2 * half + 5), p - half
    wr0, wr1 = p + half + 1, min(ch, p + 2 * half - 4)
    bl = sum(spec[wl0:wl1]) / max(1, wl1 - wl0)
    br = sum(spec[wr0:wr1]) / max(1, wr1 - wr0)
    s = w = 0.0
    for k in range(p - half, p + half + 1):
        f = (k - (p - half)) / (2.0 * half)
        v = spec[k] - (bl * (1 - f) + br * f)
        if v > 0:
            s += v * k
            w += v
    return (s / w if w > 0 else float(p)), w


def block_positions(rows, ch, peaks, B, half=25):
    """Позиции опорных пиков per-block. Возвращает {p: [pos, pos, ...]}, [center_row]."""
    n = len(rows)
    out = {p: [] for p in peaks}
    centers = []
    for b0 in range(0, n, B):
        b1 = min(n, b0 + B)
        spec = [0.0] * ch
        for r in rows[b0:b1]:
            for k in range(ch):
                spec[k] += r[k]
        for p in peaks:
            pos, _w = net_centroid(spec, p, half, ch)
            out[p].append(pos)
        centers.append((b0 + b1 - 1) / 2.0)
    return out, centers


def load_temps(temps_path):
    """Читает CSV: unix_ts;iso;t1;t2;t3 → список (ts, t1)."""
    out = []
    with open(temps_path, "r", encoding="utf-8") as f:
        rd = csv.reader(f, delimiter=";")
        header = next(rd)
        for row in rd:
            if len(row) < 3:
                continue
            try:
                ts = int(row[0])
                t1 = float(row[2])
                out.append((ts, t1))
            except (ValueError, IndexError):
                continue
    out.sort(key=lambda x: x[0])
    return out


def nearest_temp(temps, ts):
    """Бинарный поиск: ближайший T1 по времени."""
    lo, hi = 0, len(temps) - 1
    while lo < hi:
        mid = (lo + hi) // 2
        if temps[mid][0] < ts:
            lo = mid + 1
        else:
            hi = mid
    # candidates: lo, lo-1
    best = lo
    if lo > 0 and abs(temps[lo - 1][0] - ts) < abs(temps[lo][0] - ts):
        best = lo - 1
    return temps[best][1], temps[best][0]


def pearson(xs, ys):
    n = len(xs)
    if n < 2:
        return 0.0
    mx = sum(xs) / n
    my = sum(ys) / n
    sxy = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
    sxx = sum((x - mx) ** 2 for x in xs)
    syy = sum((y - my) ** 2 for y in ys)
    if sxx == 0 or syy == 0:
        return 0.0
    return sxy / (sxx ** 0.5 * syy ** 0.5)


def linreg(xs, ys):
    """y = a + b*x. Возвращает (a, b, R²)."""
    n = len(xs)
    if n < 2:
        return 0.0, 0.0, 0.0
    mx = sum(xs) / n
    my = sum(ys) / n
    sxy = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
    sxx = sum((x - mx) ** 2 for x in xs)
    if sxx == 0:
        return my, 0.0, 0.0
    b = sxy / sxx
    a = my - b * mx
    ss_res = sum((y - (a + b * x)) ** 2 for x, y in zip(xs, ys))
    ss_tot = sum((y - my) ** 2 for y in ys)
    r2 = 1.0 - ss_res / ss_tot if ss_tot > 0 else 0.0
    return a, b, r2


def main(aswf_path, block_size=8, forced_peaks=None, half=25):
    hdr, rows = load_aswf(aswf_path)
    n = len(rows)
    ch = hdr["channels"]
    print(f"строк={n} каналов={ch}")

    base = Path(aswf_path)
    state_path = base.with_suffix(base.suffix + ".state.json")
    temps_path = base.with_suffix(base.suffix + ".temps.csv")

    with open(state_path, "r", encoding="utf-8") as f:
        state = json.load(f)
    started_at = state["started_at"]
    last_end = state["last_end"]
    dur_total = last_end - started_at
    row_dur = dur_total / n  # sec/row
    print(f"started_at={started_at}  last_end={last_end}  Δ={dur_total}s  row_dur={row_dur:.2f}s")

    temps = load_temps(temps_path)
    print(f"temps: {len(temps)} записей, T1 диапазон "
          f"{min(t[1] for t in temps):.1f}..{max(t[1] for t in temps):.1f}°C")

    mean_sp = mean_spectrum(rows, ch)
    if forced_peaks:
        peaks = sorted(forced_peaks)
    else:
        peaks = find_peaks(mean_sp, ch, n_peaks=3, min_sep=100, lo=100, hi=min(4000, ch - 100))
    print(f"опорные пики (канал): {peaks}")
    for p in peaks:
        pos, w = net_centroid(mean_sp, p, half=half, ch=ch)
        print(f"  @{p}: mean centroid={pos:.2f}  net_weight={w:.0f}  half={half}")

    B = block_size
    positions, centers = block_positions(rows, ch, peaks, B, half=half)
    nb = len(centers)

    # per-block: центральный ts + T1
    ts_block = [started_at + int(c * row_dur) for c in centers]
    t1_block = [nearest_temp(temps, ts)[0] for ts in ts_block]

    print(f"\n=== Блоки (B={B} строк ≈ {B*row_dur:.0f}s) ===")
    hdr_line = f"{'blk':>4} {'row':>5} {'ts_iso':>20} {'T1':>5}"
    for p in peaks:
        hdr_line += f" {'@'+str(p):>7}"
    print(hdr_line)
    from datetime import datetime, timezone
    for j in range(nb):
        iso = datetime.fromtimestamp(ts_block[j], timezone.utc).strftime("%m-%d %H:%M:%S")
        line = f"{j:>4} {int(centers[j]):>5} {iso:>20} {t1_block[j]:>5.1f}"
        for p in peaks:
            line += f" {positions[p][j]:>7.2f}"
        print(line)

    print(f"\n=== Корреляция T1 → позиция пика ===")
    for p in peaks:
        pos = positions[p]
        r = pearson(t1_block, pos)
        a, b, r2 = linreg(t1_block, pos)
        drift = max(pos) - min(pos)
        print(f"пик @{p}: r(T1,pos)={r:+.3f}  slope={b:+.3f} кан./°C  "
              f"intercept={a:.2f}  R²={r2:.3f}  дрейф_pos={drift:.2f} кан.  "
              f"диапазон T1={max(t1_block)-min(t1_block):.1f}°C")


if __name__ == "__main__":
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        sys.exit(1)
    aswf = args[0]
    B = 8
    forced = None
    half = 25
    i = 1
    while i < len(args):
        if args[i] == "--block" and i + 1 < len(args):
            B = int(args[i + 1])
            i += 2
        elif args[i] == "--peaks" and i + 1 < len(args):
            forced = [int(x) for x in args[i + 1].split(",")]
            i += 2
        elif args[i] == "--half" and i + 1 < len(args):
            half = int(args[i + 1])
            i += 2
        else:
            i += 1
    main(aswf, B, forced, half)
