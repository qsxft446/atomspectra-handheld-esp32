#!/usr/bin/env python3
# #FW-8: проба источника голодания USB-приёма (Phase 1, systematic-debugging).
# Вопрос: коммиты свипов стоят из-за (а) WiFi/CPU-нагрузки на core 0 при любом
# HTTP-стриме, или (б) специфики flash-чтения при скачивании сегмента.
# Метод: три фазы по ~30 c, между ними пауза-контроль; в каждой фазе тянем данные
# с платы в цикле и раз в секунду снимаем hist_ok/hist_drop/usb_rx_err.
#   idle      — только опрос счётчиков (базовая скорость коммитов ~1/с);
#   ram-json  — цикл GET /api/spectrum.json (малый, из RAM);
#   ram-bin   — цикл GET /api/spectrum (32 КБ, из RAM, макс. WiFi-нагрузка);
# Если ram-bin валит коммиты так же, как скачивание сегмента, — виноват
# WiFi/CPU на core 0, фикс = перенос USB на core 1. Если нет — копать flash-путь.
#
# Запуск: python fw8_starve_probe.py [host] [фаза_сек]
import sys, json, time, urllib.request, threading

sys.stdout.reconfigure(encoding="utf-8")

HOST = sys.argv[1] if len(sys.argv) > 1 else "192.168.1.100"
PHASE_SEC = int(sys.argv[2]) if len(sys.argv) > 2 else 30
BASE = f"http://{HOST}"


def counters():
    with urllib.request.urlopen(BASE + "/api/spectrum.json", timeout=10) as r:
        j = json.loads(r.read().decode("utf-8"))
    return j.get("hist_ok", 0), j.get("hist_drop", 0), j.get("usb_rx_err", 0)


def load_loop(path, stop_evt, stats):
    while not stop_evt.is_set():
        try:
            with urllib.request.urlopen(BASE + path, timeout=15) as r:
                stats["bytes"] += len(r.read())
                stats["reqs"] += 1
        except Exception:
            stats["errs"] += 1


def run_phase(name, path):
    ok0, dr0, rx0 = counters()
    t0 = time.time()
    stats = {"bytes": 0, "reqs": 0, "errs": 0}
    stop_evt = threading.Event()
    th = None
    if path:
        th = threading.Thread(target=load_loop, args=(path, stop_evt, stats), daemon=True)
        th.start()
    time.sleep(PHASE_SEC)
    if th:
        stop_evt.set()
        th.join(timeout=20)
    dt = time.time() - t0
    ok1, dr1, rx1 = counters()
    rate = (ok1 - ok0) / dt
    print(f"[probe] {name:9s}: {dt:.0f}c  commits {ok0}->{ok1} ({rate:.2f}/c)  "
          f"drops +{dr1 - dr0}  usb_rx_err +{rx1 - rx0}  "
          f"нагрузка {stats['reqs']} req / {stats['bytes']//1024} КБ / {stats['errs']} err",
          flush=True)
    return rate


def main():
    print(f"[probe] хост {HOST}, фаза {PHASE_SEC} c", flush=True)
    r_idle = run_phase("idle", None)
    time.sleep(5)
    r_json = run_phase("ram-json", "/api/spectrum.json")
    time.sleep(5)
    r_bin = run_phase("ram-bin", "/api/spectrum")
    time.sleep(5)
    r_idle2 = run_phase("idle-2", None)
    print(f"[probe] ВЫВОД: idle={r_idle:.2f} json={r_json:.2f} bin={r_bin:.2f} idle2={r_idle2:.2f} коммит/с")
    if r_bin < 0.5 * r_idle:
        print("[probe] ram-bin ДУШИТ коммиты -> WiFi/CPU-нагрузка core 0, USB надо на core 1")
    else:
        print("[probe] ram-bin НЕ душит -> причина в flash-пути скачивания сегмента")


if __name__ == "__main__":
    main()
