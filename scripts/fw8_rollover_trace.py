#!/usr/bin/env python3
# #FW-8: посекундная трасса счётчиков через ролловер сегмента.
# Цель: привязать 40-секундное окно без коммитов к конкретной фазе ролловера
# (финализация / скачивание клиентом / удаление / первые записи нового сегмента).
# Пассивно: только GET /api/spectrum.json (RAM) и /api/waterfall/segments раз в 1 c.
# Печатает строку при КАЖДОМ изменении: hist_ok застыл/пошёл, список сегментов,
# open_idx. Работает до захвата N ролловеров или таймаута.
#
# Запуск: python fw8_rollover_trace.py [host] [ролловеров] [таймаут_мин]
import sys, json, time, urllib.request

sys.stdout.reconfigure(encoding="utf-8")

HOST = sys.argv[1] if len(sys.argv) > 1 else "192.168.1.100"
N_ROLL = int(sys.argv[2]) if len(sys.argv) > 2 else 2
TIMEOUT_MIN = float(sys.argv[3]) if len(sys.argv) > 3 else 20
BASE = f"http://{HOST}"


def get_json(path, timeout=5):
    with urllib.request.urlopen(BASE + path, timeout=timeout) as r:
        return json.loads(r.read().decode("utf-8"))


def seg_state():
    lst = get_json("/api/waterfall/segments")
    items = lst if isinstance(lst, list) else lst.get("segments", [])
    return {s["name"]: (s["bytes"], s["finalized"]) for s in items}


def main():
    t_start = time.time()
    deadline = t_start + TIMEOUT_MIN * 60
    prev_ok = prev_dr = prev_rx = None
    prev_segs = {}
    stall_from = None   # момент, с которого hist_ok не растёт
    rollovers = 0

    print(f"[trace] хост {HOST}, жду {N_ROLL} ролловеров (таймаут {TIMEOUT_MIN} мин)", flush=True)
    while time.time() < deadline and rollovers < N_ROLL + 1:
        t = time.time() - t_start
        try:
            j = get_json("/api/spectrum.json")
            ok, dr, rx = j.get("hist_ok"), j.get("hist_drop"), j.get("usb_rx_err")
        except Exception as e:
            print(f"[trace] {t:7.1f}c  spectrum.json err: {e}", flush=True)
            time.sleep(1)
            continue
        try:
            segs = seg_state()
        except Exception:
            segs = prev_segs

        if prev_ok is not None:
            if ok == prev_ok:
                if stall_from is None:
                    stall_from = t
            else:
                if stall_from is not None and t - stall_from >= 3:
                    print(f"[trace] {t:7.1f}c  СТОЯЛИ {t - stall_from:.0f}c "
                          f"(ok {prev_ok}, теперь {ok}; drop +{dr - prev_dr}, rx_err +{rx - prev_rx})",
                          flush=True)
                stall_from = None

        # события списка сегментов (начальный листинг ролловером не считаем)
        first_pass = (prev_ok is None)
        for name in segs:
            if name not in prev_segs:
                print(f"[trace] {t:7.1f}c  НОВЫЙ {name} {segs[name]}", flush=True)
                if not first_pass:
                    rollovers += 1
            elif segs[name][1] != prev_segs[name][1]:
                print(f"[trace] {t:7.1f}c  ФИНАЛИЗИРОВАН {name} bytes={segs[name][0]}", flush=True)
        for name in prev_segs:
            if name not in segs:
                print(f"[trace] {t:7.1f}c  УДАЛЁН {name}", flush=True)

        prev_ok, prev_dr, prev_rx, prev_segs = ok, dr, rx, segs
        time.sleep(1)

    print(f"[trace] конец: ролловеров {rollovers}, ok={prev_ok} drop={prev_dr} rx_err={prev_rx}")


if __name__ == "__main__":
    main()
