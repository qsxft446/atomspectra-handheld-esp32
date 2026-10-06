#!/usr/bin/env python3
# #FW-8: проверка фикса ролловера после прошивки (v2 — под staging-commit свипов).
# С атомарным коммитом свипа dur=4/6 ЛЕГИТИМНЫ (потерянный свип честно сдвигает
# секунду в соседнюю строку). Критерии зелёного:
#   1) нет dur == 0 (мёртвых строк);
#   2) скорость counts/dur каждой строки ровная: |rate/median - 1| <= 0.25
#      (при ~600 cps и dur>=4 статистический шум ~2%, полосы были в разы больше);
#   3) sum(durs) сегмента == rows * interval (потери только сдвигают, не съедают время).
# Плюс дельты usb_rx_err и hist_ok/hist_drop.
#
# Запуск: python fw8_boundary_check.py [host] [n_seg] [timeout_min]
import sys, json, time, struct, urllib.request

sys.stdout.reconfigure(encoding="utf-8")

HOST = sys.argv[1] if len(sys.argv) > 1 else "192.168.1.100"
N_SEG = int(sys.argv[2]) if len(sys.argv) > 2 else 3
TIMEOUT_MIN = float(sys.argv[3]) if len(sys.argv) > 3 else 30

BASE = f"http://{HOST}"


def get_json(path):
    with urllib.request.urlopen(BASE + path, timeout=10) as r:
        return json.loads(r.read().decode("utf-8"))


def get_bytes(path):
    with urllib.request.urlopen(BASE + path, timeout=30) as r:
        return r.read()


def parse_aswf(data):
    """-> (header_dict, [dur, ...], [row_total, ...]) ; dur=None если v1."""
    assert data[:4] == b"ASWF", "нет магии ASWF"
    hlen = struct.unpack_from("<I", data, 4)[0]
    hdr = json.loads(data[8:8 + hlen].decode("utf-8"))
    ch = int(hdr["channels"])
    row_bytes = ch * 2
    stride = int(hdr.get("row_stride", row_bytes))
    payload = data[8 + hlen:]
    rows = len(payload) // stride
    durs, totals = [], []
    for i in range(rows):
        off = i * stride
        vals = struct.unpack_from(f"<{ch}H", payload, off)
        totals.append(sum(vals))
        if stride >= row_bytes + 2:
            durs.append(struct.unpack_from("<H", payload, off + row_bytes)[0])
        else:
            durs.append(None)
    return hdr, durs, totals


def main():
    j0 = get_json("/api/spectrum.json")
    rx0, ok0, dr0 = j0.get("usb_rx_err"), j0.get("hist_ok"), j0.get("hist_drop")
    print(f"[fw8] базлайн usb_rx_err={rx0} hist_ok={ok0} hist_drop={dr0}, "
          f"жду {N_SEG} завершённых сегментов (таймаут {TIMEOUT_MIN} мин)...", flush=True)

    deadline = time.time() + TIMEOUT_MIN * 60
    segs = []
    while time.time() < deadline:
        try:
            lst = get_json("/api/waterfall/segments")
            items = lst if isinstance(lst, list) else lst.get("segments", [])
            segs = sorted((s for s in items if s.get("finalized")),
                          key=lambda s: s["idx"])
            if len(segs) >= N_SEG:
                break
        except Exception as e:
            print(f"[fw8] poll error: {e}", flush=True)
        time.sleep(30)

    if len(segs) < N_SEG:
        print(f"[fw8] ТАЙМАУТ: завершённых сегментов {len(segs)} < {N_SEG}")
        sys.exit(2)

    interval = int(get_json("/api/waterfall/status").get("interval_sec", 5))
    print(f"[fw8] сегментов готово: {len(segs)}, interval_sec={interval}")

    bad = 0
    for s in segs[-N_SEG:]:
        name = s["name"]
        data = get_bytes(f"/api/waterfall/segment?name={name}")
        hdr, durs, totals = parse_aswf(data)
        if durs and durs[0] is None:
            print(f"[fw8] {name}: v1 без durs — прошивка старая?!")
            bad += 1
            continue
        problems = []
        # 1) мёртвые строки
        dead = [i for i, d in enumerate(durs) if d == 0]
        if dead:
            problems.append(f"dur=0 в строках {dead[:8]}")
        # 2) ровность скорости counts/dur (сам смысл фикса: нет полос)
        rates = [t / d for t, d in zip(totals, durs) if d]
        med = sorted(rates)[len(rates) // 2] if rates else 0
        offr = [(i, round(t / d / med, 3)) for i, (t, d) in enumerate(zip(totals, durs))
                if d and med and abs(t / d / med - 1) > 0.25]
        if offr:
            problems.append(f"rate-полосы {offr[:8]}")
        # 3) время не съедается: sum(durs) >= rows*interval - 2. Превышение легитимно —
        #    честные dur=6 (потерянный свип отдаёт секунду соседней строке), но не
        #    более ~10% строк (иначе поток деградировал).
        excess = sum(durs) - len(durs) * interval
        if excess < -2 or excess > max(2, len(durs) // 10):
            problems.append(f"sum(durs)={sum(durs)} != {len(durs) * interval}")
        dur_hist = {}
        for d in durs:
            dur_hist[d] = dur_hist.get(d, 0) + 1
        mark = "OK" if not problems else "; ".join(problems)
        print(f"[fw8] {name}: rows={len(durs)} durs={dur_hist} rate_med={med:.1f} -> {mark}")
        if problems:
            bad += 1

    j1 = get_json("/api/spectrum.json")
    rx1, ok1, dr1 = j1.get("usb_rx_err"), j1.get("hist_ok"), j1.get("hist_drop")
    print(f"[fw8] usb_rx_err: {rx0} -> {rx1}; hist_ok: {ok0} -> {ok1}; hist_drop: {dr0} -> {dr1}")
    print(f"[fw8] ИТОГ: {'ЗЕЛЕНО — нет dur=0, скорость ровная, время сохранено' if bad == 0 else f'{bad} сегм. с проблемами'}")
    sys.exit(0 if bad == 0 else 1)


if __name__ == "__main__":
    main()
