#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""AtomSpectra Waterfall — PC capture client.

Streams waterfall rows from the ESP32 over WebSocket (/ws/waterfall) and writes
an unbounded self-describing .aswf file (same format the board exports), so the
PC capture is not limited by the on-board PSRAM ring.

.aswf layout:  "ASWF"(4) | header_len:u32 LE | JSON header (utf-8) | payload
               payload = rows * channels * uint16 LE  (one waterfall row per period)

Requires:  pip install websocket-client requests
Usage:     python waterfall_client.py <board-ip> [-o capture.aswf]
Stop with Ctrl+C — the header (with the final row count) is written on exit.
"""
import sys, os, json, time, struct, argparse, tempfile
sys.stdout.reconfigure(encoding="utf-8")

try:
    import websocket  # websocket-client
except ImportError:
    sys.exit("pip install websocket-client  (module 'websocket' not found)")
try:
    import requests
except ImportError:
    requests = None


def fetch_calib(host):
    """Calibration/serial are not in the WS header; pull them from /api/spectrum.json."""
    if requests is None:
        return None, ""
    try:
        r = requests.get("http://%s/api/spectrum.json" % host, timeout=4).json()
        return r.get("calibration"), r.get("serial_number", "")
    except Exception:
        return None, ""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("host", help="board IP or mDNS, e.g. atomspectra.local")
    ap.add_argument("-o", "--out", default=None, help="output .aswf path")
    args = ap.parse_args()

    out = args.out or time.strftime("waterfall_%Y%m%d_%H%M%S.aswf")
    calib, serial = fetch_calib(args.host)

    state = {"channels": 8192, "interval": 5, "started": int(time.time()), "rows": 0}
    tmp = tempfile.NamedTemporaryFile(prefix="aswf_", suffix=".bin", delete=False)
    tmp_path = tmp.name
    print("connecting ws://%s/ws/waterfall  ->  %s" % (args.host, out))

    def on_message(ws, msg):
        if isinstance(msg, str):
            try:
                h = json.loads(msg)
                if h.get("channels"):
                    state["channels"] = int(h["channels"])
                if h.get("interval_sec"):
                    state["interval"] = int(h["interval_sec"])
                print("header: channels=%d interval=%ds total_rows=%s"
                      % (state["channels"], state["interval"], h.get("total_rows")))
            except Exception:
                pass
            return
        # binary frame = one row (channels * uint16 LE)
        tmp.write(msg)
        state["rows"] += 1
        if state["rows"] % 10 == 0:
            print("\rrows captured: %d" % state["rows"], end="", flush=True)

    def on_error(ws, err):
        print("\nws error:", err)

    def on_close(ws, code, reason):
        print("\nws closed", code, reason or "")

    ws = websocket.WebSocketApp(
        "ws://%s/ws/waterfall" % args.host,
        on_message=on_message, on_error=on_error, on_close=on_close)
    try:
        ws.run_forever(ping_interval=20, ping_timeout=10)
    except KeyboardInterrupt:
        print("\ninterrupted")
    finally:
        ws.close()
        tmp.flush()
        tmp.close()
        finalize(out, tmp_path, state, calib, serial)


def finalize(out, tmp_path, state, calib, serial):
    payload = state["rows"] * state["channels"] * 2
    header = {
        "format": "atomspectra-waterfall",
        "channels": state["channels"],
        "rows": state["rows"],
        "interval_sec": state["interval"],
        "started_at": state["started"],
        "serial": serial or "",
        "source": "ws-stream",
    }
    if calib:
        header["calibration"] = calib
    hj = json.dumps(header, ensure_ascii=False).encode("utf-8")
    with open(out, "wb") as f:
        f.write(b"ASWF")
        f.write(struct.pack("<I", len(hj)))
        f.write(hj)
        with open(tmp_path, "rb") as src:
            while True:
                chunk = src.read(65536)
                if not chunk:
                    break
                f.write(chunk)
    try:
        os.unlink(tmp_path)
    except OSError:
        pass
    size = 8 + len(hj) + payload
    print("wrote %s : %d rows, %d channels, ~%d bytes" %
          (out, state["rows"], state["channels"], size))


if __name__ == "__main__":
    main()
