#!/usr/bin/env python3
# #FW-10: карта позиций строк с dur != interval внутри сегментов.
# Вопрос: джиттер dur размазан равномерно или концентрируется у границ
# (первые/последние строки сегмента)? От ответа зависит фикс:
# у границ -> дорабатывать ролловер в прошивке; равномерно -> нормировка
# яркости на dur при отрисовке (плата + вьюер).
# Пассивно: скачивает финализированные сегменты, парсит dur каждой строки.
#
# Запуск: python fw10_dur_map.py [host]
import sys, json, struct, urllib.request

sys.stdout.reconfigure(encoding="utf-8")

HOST = sys.argv[1] if len(sys.argv) > 1 else "192.168.1.100"
BASE = f"http://{HOST}"
ROW_BYTES = 16384
ROW_STRIDE = 16386


def get(path, timeout=30):
    with urllib.request.urlopen(BASE + path, timeout=timeout) as r:
        return r.read()


def parse_durs(blob):
    if blob[:4] != b"ASWF":
        raise ValueError("не ASWF")
    reserve = struct.unpack("<I", blob[4:8])[0]
    data_off = 8 + reserve
    body = blob[data_off:]
    rows = len(body) // ROW_STRIDE
    durs = []
    for i in range(rows):
        off = i * ROW_STRIDE + ROW_BYTES
        durs.append(struct.unpack("<H", body[off:off + 2])[0])
    return durs


def main():
    iv = json.loads(get("/api/spectrum.json"))
    interval = iv.get("interval_sec", 5) or 5
    lst = json.loads(get("/api/waterfall/segments"))
    items = lst if isinstance(lst, list) else lst.get("segments", [])
    done = [s for s in items if s.get("finalized")]
    print(f"[fw10] хост {HOST}, interval={interval}c, финализировано {len(done)}")
    for s in done:
        name = s["name"]
        try:
            blob = get(f"/api/waterfall/segment?name={name}")
            durs = parse_durs(blob)
        except Exception as e:
            print(f"[fw10] {name}: ошибка {e}")
            continue
        bad = [(i, d) for i, d in enumerate(durs) if d != interval]
        n = len(durs)
        edge = sum(1 for i, _ in bad if i < 4 or i >= n - 4)
        print(f"[fw10] {name}: rows={n}, откл. {len(bad)} (у краёв {edge}): "
              + " ".join(f"[{i}]={d}" for i, d in bad))


if __name__ == "__main__":
    main()
