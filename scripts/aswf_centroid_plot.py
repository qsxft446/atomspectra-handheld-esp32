#!/usr/bin/env python3
"""Совмещённый график: центроиды K-40 и Tl-208 + T1 vs время.

Центроид считается в ШИРОКОМ окне, охватывающем обе позиции задвоения:
  K-40:   [3100 .. 3470]   (штатно 3151, смещённо 3424)
  Tl-208: [5480 .. 6080]   (штатно 5545, смещённо 6015)
Так центроид мигрирует непрерывно между двумя пиками, показывая
смещение усиления во времени.

Использование:
  python aswf_centroid_plot.py <файл.aswf> [--block N] [--out report.html]
"""
import csv
import json
import struct
import sys
from datetime import datetime, timezone
from pathlib import Path

sys.stdout.reconfigure(encoding="utf-8")

# широкие окна, охватывающие обе позиции задвоения
K40_LO, K40_HI = 3100, 3470
TL_LO, TL_HI = 5480, 6080
# опорные позиции для аннотаций
K40_A, K40_B = 3151, 3424
TL_A, TL_B = 5545, 6015


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


def wide_centroid(spec, lo, hi, bg_pad=60):
    """Центроид спектра в окне [lo..hi] с вычитанием линейного фона по бокам."""
    ch = len(spec)
    bl_lo, bl_hi = max(0, lo - bg_pad), lo
    br_lo, br_hi = hi, min(ch, hi + bg_pad)
    bl = sum(spec[bl_lo:bl_hi]) / max(1, bl_hi - bl_lo)
    br = sum(spec[br_lo:br_hi]) / max(1, br_hi - br_lo)
    W = hi - lo
    s = w = 0.0
    for k in range(lo, hi):
        f = (k - lo) / W
        v = spec[k] - (bl * (1 - f) + br * f)
        if v > 0:
            s += v * k
            w += v
    return (s / w if w > 0 else None), w


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


def main(aswf_path, B=4, out_html=None):
    hdr, rows = load_aswf(aswf_path)
    ch = hdr["channels"]
    n = len(rows)

    base = Path(aswf_path)
    state = json.loads(base.with_suffix(base.suffix + ".state.json").read_text(encoding="utf-8"))
    started_at = state["started_at"]
    last_end = state["last_end"]
    row_dur = (last_end - started_at) / n

    temps = load_temps(base.with_suffix(base.suffix + ".temps.csv"))

    if out_html is None:
        out_html = base.parent / "bimodal_centroid_report.html"
    out_html = Path(out_html)

    print(f"строк={n} каналов={ch} блок={B}  row_dur={row_dur:.1f}s")
    print(f"K-40:  окно {K40_LO}..{K40_HI} (штатно {K40_A}, смещённо {K40_B})")
    print(f"Tl-208: окно {TL_LO}..{TL_HI} (штатно {TL_A}, смещённо {TL_B})")

    pts = []  # (ts, t1, k40_c, tl_c, k40_w, tl_w)
    for b0 in range(0, n, B):
        b1 = min(n, b0 + B)
        spec = [0.0] * ch
        for r in rows[b0:b1]:
            for k in range(ch):
                spec[k] += r[k]
        k40_c, k40_w = wide_centroid(spec, K40_LO, K40_HI)
        tl_c, tl_w = wide_centroid(spec, TL_LO, TL_HI)
        center_row = (b0 + b1 - 1) / 2.0
        ts = started_at + int(center_row * row_dur)
        t1 = nearest_t1(temps, ts)
        pts.append((ts, t1, k40_c, tl_c, k40_w, tl_w))

    ts0 = pts[0][0]
    ts1 = pts[-1][0]
    print(f"точек: {len(pts)}, "
          f"K-40 net_w mean={sum(p[4] for p in pts)/len(pts):.0f}, "
          f"Tl-208 net_w mean={sum(p[5] for p in pts)/len(pts):.0f}")

    render_html(out_html, pts, ts0, ts1, hdr, B, row_dur)
    print(f"\nОтчёт: {out_html}")


def render_html(out, pts, ts0, ts1, hdr, B, row_dur):
    # SVG-размеры
    W, H = 1400, 640
    L, R, T, Bp = 80, 90, 60, 60  # padding
    plot_w = W - L - R
    plot_h = H - T - Bp

    # оси
    x_span = ts1 - ts0 if ts1 > ts0 else 1

    def x_of(ts):
        return L + (ts - ts0) / x_span * plot_w

    # раздельные Y-масштабы: K-40 слева (3100..3470), Tl-208 справа (5480..6080)
    def y_k40(c):
        return T + (K40_HI - c) / (K40_HI - K40_LO) * plot_h

    def y_tl(c):
        return T + (TL_HI - c) / (TL_HI - TL_LO) * plot_h

    # T1: диапазон реальный
    t1_vals = [p[1] for p in pts]
    t1_min = min(t1_vals) - 0.3
    t1_max = max(t1_vals) + 0.3

    def y_t1(t):
        return T + (t1_max - t) / (t1_max - t1_min) * plot_h

    # линии
    def polyline(color, pts_xy, dash=None, w=1.5, opacity=1.0):
        pt_str = " ".join(f"{x:.1f},{y:.1f}" for x, y in pts_xy)
        d = f' stroke-dasharray="{dash}"' if dash else ""
        return (f'<polyline points="{pt_str}" fill="none" stroke="{color}" '
                f'stroke-width="{w}" opacity="{opacity}"{d}/>')

    # K-40 центроид
    k40_line = [(x_of(p[0]), y_k40(p[2])) for p in pts if p[2] is not None]
    # Tl-208 центроид
    tl_line = [(x_of(p[0]), y_tl(p[3])) for p in pts if p[3] is not None]
    # T1
    t1_line = [(x_of(p[0]), y_t1(p[1])) for p in pts]

    # опорные линии (штатно/смещённо)
    def hline_k40(c, txt, color):
        y = y_k40(c)
        return (f'<line x1="{L}" y1="{y:.1f}" x2="{L+plot_w}" y2="{y:.1f}" '
                f'stroke="{color}" stroke-width="0.8" opacity="0.4" stroke-dasharray="4 4"/>'
                f'<text x="{L-5}" y="{y+4:.1f}" font-size="10" fill="{color}" text-anchor="end">'
                f'{txt} ({c})</text>')

    def hline_tl(c, txt, color):
        y = y_tl(c)
        return (f'<line x1="{L}" y1="{y:.1f}" x2="{L+plot_w}" y2="{y:.1f}" '
                f'stroke="{color}" stroke-width="0.8" opacity="0.4" stroke-dasharray="4 4"/>'
                f'<text x="{L+plot_w+5}" y="{y+4:.1f}" font-size="10" fill="{color}" text-anchor="start">'
                f'{txt} ({c})</text>')

    ref_lines = [
        hline_k40(K40_A, "K-40 штат", "#0aa"),
        hline_k40(K40_B, "K-40 смещ", "#a30"),
        hline_tl(TL_A, "Tl-208 штат", "#48c"),
        hline_tl(TL_B, "Tl-208 смещ", "#c40"),
    ]

    # тики оси X по часам
    from datetime import timedelta
    ticks_x = []
    dt0 = datetime.fromtimestamp(ts0, timezone.utc)
    # округлить вниз к часу
    hr0 = dt0.replace(minute=0, second=0, microsecond=0)
    if hr0 < dt0:
        hr0 += timedelta(hours=1)
    t = hr0
    while t.timestamp() <= ts1:
        ts_tick = int(t.timestamp())
        xt = x_of(ts_tick)
        lbl = t.strftime("%H:%M")
        ticks_x.append((xt, lbl))
        t += timedelta(hours=1)

    tick_lines = []
    for xt, lbl in ticks_x:
        tick_lines.append(
            f'<line x1="{xt:.1f}" y1="{T+plot_h}" x2="{xt:.1f}" y2="{T+plot_h+5}" stroke="#666"/>'
            f'<line x1="{xt:.1f}" y1="{T}" x2="{xt:.1f}" y2="{T+plot_h}" stroke="#eee" stroke-width="0.5"/>'
            f'<text x="{xt:.1f}" y="{T+plot_h+18}" font-size="11" fill="#333" text-anchor="middle">{lbl}</text>'
        )

    # тики Y (K-40 слева)
    ytk_k40 = []
    for c in [3100, 3151, 3200, 3300, 3424, 3470]:
        y = y_k40(c)
        ytk_k40.append(
            f'<text x="{L-8}" y="{y+3:.1f}" font-size="10" fill="#046" text-anchor="end">{c}</text>'
        )

    # тики Y (Tl-208 справа)
    ytk_tl = []
    for c in [5480, 5545, 5700, 5900, 6015, 6080]:
        y = y_tl(c)
        ytk_tl.append(
            f'<text x="{L+plot_w+8}" y="{y+3:.1f}" font-size="10" fill="#640" text-anchor="start">{c}</text>'
        )

    # T1 тики (наверху, отдельная плашка)
    t1_tick_str = " · ".join(f"{v:.1f}°C" for v in sorted(set(t1_vals)))

    # SVG
    svg_parts = [
        f'<svg viewBox="0 0 {W} {H}" xmlns="http://www.w3.org/2000/svg" '
        f'style="width:100%;height:auto;background:#fafafa;border:1px solid #ccc">',
        # рамка
        f'<rect x="{L}" y="{T}" width="{plot_w}" height="{plot_h}" fill="white" stroke="#999"/>',
        *ref_lines,
        *tick_lines,
        *ytk_k40,
        *ytk_tl,
        # T1 линия (толстая, приглушённая) — на фоне
        polyline("#c8b100", t1_line, w=2.5, opacity=0.35),
        # K-40 центроид (жирная)
        polyline("#046", k40_line, w=2.2, opacity=0.95),
        # Tl-208 центроид (жирная)
        polyline("#640", tl_line, w=2.2, opacity=0.95),
        # подписи осей
        f'<text x="{W/2}" y="{H-15}" font-size="13" fill="#222" text-anchor="middle">UTC время</text>',
        f'<text x="20" y="{T+plot_h/2}" font-size="13" fill="#046" text-anchor="middle" '
        f'transform="rotate(-90 20,{T+plot_h/2})">канал K-40</text>',
        f'<text x="{W-20}" y="{T+plot_h/2}" font-size="13" fill="#640" text-anchor="middle" '
        f'transform="rotate(-90 {W-20},{T+plot_h/2})">канал Tl-208</text>',
        # легенда
        f'<g transform="translate({L+20},{T+15})">',
        '<rect x="0" y="0" width="360" height="72" fill="white" stroke="#999" opacity="0.95"/>',
        '<line x1="12" y1="16" x2="42" y2="16" stroke="#046" stroke-width="2.2"/>',
        f'<text x="50" y="20" font-size="12" fill="#046">центроид K-40 (окно {K40_LO}..{K40_HI})</text>',
        '<line x1="12" y1="36" x2="42" y2="36" stroke="#640" stroke-width="2.2"/>',
        f'<text x="50" y="40" font-size="12" fill="#640">центроид Tl-208 (окно {TL_LO}..{TL_HI})</text>',
        '<line x1="12" y1="56" x2="42" y2="56" stroke="#c8b100" stroke-width="2.5" opacity="0.6"/>',
        f'<text x="50" y="60" font-size="12" fill="#987">T1 детектора ({t1_min:.1f}..{t1_max:.1f}°C)</text>',
        '</g>',
        '</svg>'
    ]

    # компактная сводка
    k40_min = min((p[2] for p in pts if p[2] is not None), default=0)
    k40_max = max((p[2] for p in pts if p[2] is not None), default=0)
    tl_min = min((p[3] for p in pts if p[3] is not None), default=0)
    tl_max = max((p[3] for p in pts if p[3] is not None), default=0)

    start_iso = datetime.fromtimestamp(ts0, timezone.utc).strftime("%Y-%m-%d %H:%M:%S UTC")
    end_iso = datetime.fromtimestamp(ts1, timezone.utc).strftime("%Y-%m-%d %H:%M:%S UTC")
    dur_h = (ts1 - ts0) / 3600.0

    html = f"""<!doctype html>
<html lang="ru"><head><meta charset="utf-8">
<title>Совмещённый график: центроиды K-40, Tl-208, T1</title>
<style>
body{{font-family:-apple-system,Segoe UI,Roboto,sans-serif;margin:24px;color:#222;max-width:1500px}}
h1{{margin:0 0 6px;font-size:20px}}
.sub{{color:#666;font-size:13px;margin-bottom:16px}}
.summary{{display:grid;grid-template-columns:repeat(4,1fr);gap:12px;margin:12px 0 20px}}
.card{{border:1px solid #ddd;padding:10px 12px;border-radius:6px;background:#f8f8f8}}
.card .lbl{{font-size:11px;color:#666;text-transform:uppercase}}
.card .val{{font-size:18px;font-weight:600;margin-top:2px}}
.note{{background:#fff8e1;border-left:4px solid #f9a825;padding:10px 14px;margin:16px 0;font-size:14px}}
</style></head>
<body>
<h1>Задвоение пиков γ-спектра: центроиды K-40 и Tl-208 vs T1 детектора</h1>
<div class="sub">Файл: <code>spectrogram.aswf</code> · старт {start_iso} · конец {end_iso} · длительность {dur_h:.2f} ч ·
блок агрегации: {B} строк ≈ {int(B*row_dur)}с · всего строк: {hdr.get('channels',8192) and len(pts)*B}</div>

<div class="summary">
  <div class="card"><div class="lbl">K-40 центроид, min→max</div><div class="val">{k40_min:.1f} → {k40_max:.1f}</div></div>
  <div class="card"><div class="lbl">размах K-40</div><div class="val">{k40_max-k40_min:.0f} каналов</div></div>
  <div class="card"><div class="lbl">Tl-208 центроид, min→max</div><div class="val">{tl_min:.1f} → {tl_max:.1f}</div></div>
  <div class="card"><div class="lbl">размах Tl-208</div><div class="val">{tl_max-tl_min:.0f} каналов</div></div>
</div>

{"".join(svg_parts)}

<div class="note">
<b>Как читать:</b> синяя линия — центроид K-40 (шкала слева, штатно {K40_A}, смещённо {K40_B}, задвоение = скачок между уровнями).
Коричневая — центроид Tl-208 (шкала справа, штатно {TL_A}, смещённо {TL_B}).
Жёлтая приглушённая — T1 детектора (26..28°C). Пунктирные линии — опорные позиции штатного и смещённого пика.
Если центроид «прилипает» к штатному уровню — режим A; если к смещённому — режим B; посередине — смесь / переход.
Синхронность K-40 и Tl-208 доказывает: сдвиг общий (gain-drift ~8.5%), а не аномалия одной линии.
</div>

</body></html>"""
    out.write_text(html, encoding="utf-8")


if __name__ == "__main__":
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        sys.exit(1)
    aswf = args[0]
    B = 4
    out = None
    i = 1
    while i < len(args):
        a = args[i]
        if a == "--block": B = int(args[i+1]); i += 2
        elif a == "--out": out = args[i+1]; i += 2
        else: i += 1
    main(aswf, B, out)
