#!/usr/bin/env python3
"""Диагностика дрейфа линий в склеенном .aswf: траектория центроида,
автокорреляция остатка на лаге 64 (остаток склейки?) vs плавный дрейф усиления.

Использование: python aswf_drift.py <файл.aswf>
"""
import json
import struct
import sys

sys.stdout.reconfigure(encoding="utf-8")


def load_n42(path):
    """N42: RadMeasurement/Spectrum/ChannelData, compressionCode CountedZeroes."""
    import re
    text = open(path, "r", encoding="utf-8").read()
    rows = []
    ch = 0
    for m in re.finditer(r'<ChannelData([^>]*)>([^<]*)</ChannelData>', text):
        attrs, body = m.group(1), m.group(2)
        toks = [int(float(t)) for t in body.split()]
        if "countedzero" in attrs.lower():
            out = []
            i = 0
            while i < len(toks):
                v = toks[i]
                i += 1
                if v == 0:
                    out.extend([0] * toks[i])
                    i += 1
                else:
                    out.append(v)
            toks = out
        rows.append(toks)
        ch = max(ch, len(toks))
    rows = [r + [0] * (ch - len(r)) for r in rows]
    return {"channels": ch}, rows


def load(path):
    if path.lower().endswith((".n42", ".xml")):
        return load_n42(path)
    buf = open(path, "rb").read()
    assert buf[:4] == b"ASWF", "не .aswf"
    hlen = struct.unpack_from("<I", buf, 4)[0]
    hdr = json.loads(buf[8:8 + hlen].decode("utf-8"))
    ch = hdr["channels"]
    stride = hdr.get("row_stride", ch * 2)
    payload = buf[8 + hlen:]
    n = len(payload) // stride
    rows = [struct.unpack_from(f"<{ch}H", payload, i * stride) for i in range(n)]
    return hdr, rows


def centroid(r, lo=100, hi=2000):
    s = w = 0
    for k in range(lo, hi):
        s += r[k] * k
        w += r[k]
    return s / w if w else 0.0


def smooth(x, w=15):
    return [sum(x[max(0, i - w):i + w + 1]) / (min(len(x), i + w + 1) - max(0, i - w))
            for i in range(len(x))]


def main(path):
    hdr, rows = load(path)
    n, ch = len(rows), hdr["channels"]
    print(f"строк={n} каналов={ch} stride={hdr.get('row_stride', ch*2)}")

    cent = [centroid(r) for r in rows]
    sm = smooth(cent)
    mean = sum(cent) / n
    sigma = (sum((c - mean) ** 2 for c in cent) / n) ** 0.5
    print(f"центроид: mean={mean:.1f}  сглаж. min={min(sm):.1f} max={max(sm):.1f} "
          f"дрейф={max(sm)-min(sm):.1f} кан.  sigma_raw={sigma:.1f}")

    res = [c - s for c, s in zip(cent, sm)]
    rv = sum(x * x for x in res) / n
    print("автокорреляция остатка (пик на 64 = остаток склейки):")
    for lag in (1, 2, 8, 16, 32, 63, 64, 65):
        acf = sum(res[i] * res[i + lag] for i in range(n - lag)) / ((n - lag) * rv)
        print(f"  lag={lag:3d}: {acf:+.3f}" + ("  ←64" if lag == 64 else ""))

    jb = [abs(cent[i] - cent[i - 1]) for i in range(1, n) if i % 64 == 0]
    ji = [abs(cent[i] - cent[i - 1]) for i in range(1, n) if i % 64 != 0]
    print(f"скачок центроида: границы avg={sum(jb)/len(jb):.2f} (n={len(jb)}) "
          f"внутри avg={sum(ji)/len(ji):.2f} (n={len(ji)})")

    print("траектория сглаж. (каждая 16-я строка):")
    print("  " + " ".join(f"{sm[i]:.0f}" for i in range(0, n, 16)))

    # пик-трекинг: позиция самого сильного пика по строкам (локальный центроид ±25 кан.)
    mean_sp = [sum(r[k] for r in rows) for k in range(ch)]
    pk = max(range(50, min(4000, ch)), key=lambda k: mean_sp[k])
    lo_p, hi_p = max(0, pk - 25), min(ch, pk + 26)
    ppos = []
    for r in rows:
        s = w = 0
        for k in range(lo_p, hi_p):
            s += r[k] * k
            w += r[k]
        ppos.append(s / w if w else float(pk))
    psm = smooth(ppos)
    pmean = sum(ppos) / n
    psig = (sum((p - pmean) ** 2 for p in ppos) / n) ** 0.5
    print(f"пик @{pk}: mean={pmean:.2f} sigma_raw={psig:.2f} "
          f"сглаж. min={min(psm):.2f} max={max(psm):.2f} дрейф={max(psm)-min(psm):.2f} кан.")
    print("траектория пика сглаж. (каждая 16-я):")
    print("  " + " ".join(f"{psm[i]:.1f}" for i in range(0, n, 16)))

    # проверка компенсации: корреляционная оценка g по блокам строк.
    # Блок B строк суммируется, для сетки g ресемплится (k → k·g) и коррелируется
    # со средним спектром; лучший g уточняется параболой; per-row g — лин. интерп. между блоками.
    B = 16

    def net_peak_pos(spec, p, half):
        """Центроид пика @p (±half) с вычитанием линейного фона по боковым окнам."""
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

    # топ-3 локальных пика в сглаженном среднем спектре (мин. разнос 3×half)
    HALF = 25
    msm = [sum(mean_sp[max(0, k - 3):k + 4]) / len(mean_sp[max(0, k - 3):k + 4])
           for k in range(ch)]
    cand = sorted(range(60, min(4000, ch - 60)), key=lambda k: -msm[k])
    peaks = []
    for k in cand:
        if all(abs(k - p) > 3 * HALF for p in peaks):
            peaks.append(k)
        if len(peaks) == 2:  # два сильнейших: третий уже тонет в шуме и портит фит
            break
    peaks.sort()
    refs = {}
    for p in peaks:
        pos, w = net_peak_pos(mean_sp, p, HALF)
        refs[p] = (pos, w)
    print(f"опорные пики: {[(p, round(refs[p][0],1)) for p in peaks]}")

    blocks = []
    traj = {p: [] for p in peaks}  # Δpos (кан.) по блокам для каждого пика — диагностика модели
    for b0 in range(0, n, B):
        spec = [0.0] * ch
        for r in rows[b0:b0 + B]:
            for k in range(ch):
                spec[k] += r[k]
        # аффинная модель дрейфа: pos_p = ref_p + (g-1)·p + b — решается точно по двум пикам
        d = {}
        for p in peaks:
            pos, w = net_peak_pos(spec, p, HALF)
            ref_pos, wm = refs[p]
            traj[p].append(pos - ref_pos)
            d[p] = pos - ref_pos
        p0, p1 = peaks
        gm1 = (d[p1] - d[p0]) / (p1 - p0)
        b = d[p0] - gm1 * p0
        blocks.append((b0 + min(B, n - b0) / 2.0, 1.0 + gm1, b))
    for p in peaks:
        print(f"Δpos пика @{p} по блокам: " + " ".join(f"{v:+.1f}" for v in traj[p]))
    # шумодав: траектория Δpos каждого пика — гладкая кривая прогрева → полином deg 4
    # по всем блокам (глобальный фит давит независимый шум ~√(N/deg), волну сохраняет)
    def polyfit_eval(xs, ys, deg):
        m = deg + 1
        A = [[sum(x ** (i + j) for x in xs) for j in range(m)] for i in range(m)]
        v = [sum(y * x ** i for x, y in zip(xs, ys)) for i in range(m)]
        for col in range(m):  # Гаусс с выбором главного элемента
            piv = max(range(col, m), key=lambda r2: abs(A[r2][col]))
            A[col], A[piv] = A[piv], A[col]
            v[col], v[piv] = v[piv], v[col]
            for r2 in range(col + 1, m):
                f = A[r2][col] / A[col][col]
                for c2 in range(col, m):
                    A[r2][c2] -= f * A[col][c2]
                v[r2] -= f * v[col]
        coef = [0.0] * m
        for r2 in range(m - 1, -1, -1):
            coef[r2] = (v[r2] - sum(A[r2][c2] * coef[c2] for c2 in range(r2 + 1, m))) / A[r2][r2]
        return lambda x: sum(coef[i] * x ** i for i in range(m))

    nb = len(blocks)
    import os
    deg = int(os.environ.get("DRIFT_DEG", "0"))  # >0 полином; 0 скольз.среднее ±2; -1 без сглаживания
    xs = [j / nb for j in range(nb)]  # нормировка x для численной устойчивости
    if deg > 0:
        fits = {p: polyfit_eval(xs, traj[p], deg) for p in peaks}
        sm_traj = {p: [fits[p](x) for x in xs] for p in peaks}
    elif deg == 0:
        sm_traj = {p: [sum(traj[p][max(0, j - 2):j + 3]) / len(traj[p][max(0, j - 2):j + 3])
                       for j in range(nb)] for p in peaks}
    else:
        sm_traj = traj
    p0_, p1_ = peaks
    shift_only = os.environ.get("DRIFT_SHIFT", "") == "1"  # чистый сдвиг: b = взвеш. среднее Δpos
    blocks = []
    for j in range(nb):
        c = (j * B + min((j + 1) * B, n) - 1) / 2.0
        d0, d1 = sm_traj[p0_][j], sm_traj[p1_][j]
        if shift_only:
            w0, w1 = refs[p0_][1], refs[p1_][1]
            blocks.append((c, 1.0, (d0 * w0 + d1 * w1) / (w0 + w1)))
        else:
            gm1 = (d1 - d0) / (p1_ - p0_)
            blocks.append((c, 1.0 + gm1, d0 - gm1 * p0_))
    print("(g,b) по блокам после poly-фита (центр: g/b):")
    print("  " + " ".join(f"{c:.0f}:{g:.4f}/{b:+.1f}" for c, g, b in blocks))

    def gb_row(i):
        if i <= blocks[0][0]:
            return blocks[0][1], blocks[0][2]
        if i >= blocks[-1][0]:
            return blocks[-1][1], blocks[-1][2]
        for t in range(1, len(blocks)):
            if i <= blocks[t][0]:
                c0, g0, b0 = blocks[t - 1]
                c1, g1, b1 = blocks[t]
                f = (i - c0) / (c1 - c0)
                return g0 * (1 - f) + g1 * f, b0 * (1 - f) + b1 * f
        return 1.0, 0.0

    fixed = []
    for i, r in enumerate(rows):
        g, b = gb_row(i)
        row = [0.0] * ch
        for k in range(ch):
            x = k * g + b
            k0 = int(x)
            f = x - k0
            if 0 <= k0 and k0 + 1 < ch:
                row[k] = r[k0] * (1 - f) + r[k0 + 1] * f
            elif 0 <= k0 < ch:
                row[k] = r[k0]
        fixed.append(row)
    # честная метрика: чистая позиция каждого опорного пика по блокам до/после
    def block_positions(rr, p):
        out = []
        for b0 in range(0, n, B):
            spec = [0.0] * ch
            for r in rr[b0:b0 + B]:
                for k in range(max(0, p - 60), min(ch, p + 61)):
                    spec[k] += r[k]
            out.append(net_peak_pos(spec, p, HALF)[0])
        return out
    for p in peaks:
        pb_before = block_positions(rows, p)
        pb_after = block_positions(fixed, p)
        print(f"пик @{p} ДО:    дрейф={max(pb_before)-min(pb_before):.2f} кан.  "
              + " ".join(f"{x:.1f}" for x in pb_before))
        print(f"пик @{p} ПОСЛЕ: дрейф={max(pb_after)-min(pb_after):.2f} кан.  "
              + " ".join(f"{x:.1f}" for x in pb_after))


if __name__ == "__main__":
    main(sys.argv[1])
