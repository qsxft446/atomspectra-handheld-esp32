#!/usr/bin/env python3
"""#FW-39 verify: свежий export.aswf с платы объявляет row_stride=16402 и парсится ровно.

Ждёт reconnect после прошивки + накопления строк в ring, тянет /api/waterfall/export.aswf,
проверяет: declared row_stride == 16402 И payload % stride == 0 (нет накопительного сдвига).

Использование: python fw39_verify.py [http://atomspectra.local]
"""
import json
import struct
import sys
import time
import urllib.request

sys.stdout.reconfigure(encoding="utf-8")
BOARD = (sys.argv[1] if len(sys.argv) > 1 else "http://atomspectra.local").rstrip("/")
OPENER = urllib.request.build_opener(urllib.request.ProxyHandler({}))


def pull():
    with OPENER.open(BOARD + "/api/waterfall/export.aswf", timeout=20) as r:
        return r.read()


def main():
    buf = None
    for attempt in range(40):  # ~40×5с = 200с потолок
        try:
            buf = pull()
            if buf[:4] == b"ASWF":
                break
        except Exception as e:
            print(f"[{attempt}] ещё не готово: {e}")
        time.sleep(5)
    if not buf or buf[:4] != b"ASWF":
        print("FAIL: плата не отдала export.aswf за 200с")
        sys.exit(1)
    hlen = struct.unpack_from("<I", buf, 4)[0]
    hdr = json.loads(buf[8:8 + hlen].decode("utf-8"))
    stride = hdr.get("row_stride")
    payload = len(buf) - 8 - hlen
    rem = payload % stride if stride else -1
    rows = payload // stride if stride else -1
    ok = (stride == 16402 and rem == 0)
    print(f"declared row_stride={stride} (ждём 16402)  payload={payload}  rows={rows}  rem={rem}")
    print(f"version={hdr.get('version')} channels={hdr.get('channels')} interval={hdr.get('interval_sec')}")
    print("PASS ✅ фикс #FW-39 на плате" if ok else "FAIL ❌ stride/остаток не сошлись")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
