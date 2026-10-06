#!/usr/bin/env python3
"""HTTP-опрос платы AtomSpectra: /api/status + /api/system -> CSV.
Для #STAB-4 (тест стабильности тракта LaBr+WT20).

Usage:
  python http_stab4_poll.py --out FILE [--url URL] [--interval SEC]

По умолчанию:
  --url      http://atomspectra.local
  --interval 600 (синхронно с интервалом водопада)
"""
import argparse
import csv
import json
import sys
import time
import urllib.error
import urllib.request
from datetime import datetime, timezone
from pathlib import Path

sys.stdout.reconfigure(encoding="utf-8")

COLS = [
    "ts_iso", "ts_unix", "uptime_sec",
    "t1", "t2", "t3",
    "total_counts", "cpu_load",
    "analyzer_connected", "wifi_connected",
    "free_heap", "min_free_heap",
    "flash_used", "rssi",
    "error",
]


def fetch(url, timeout=5.0):
    with urllib.request.urlopen(url, timeout=timeout) as r:
        return json.loads(r.read().decode("utf-8"))


def poll_once(base):
    row = {c: "" for c in COLS}
    now = datetime.now(timezone.utc)
    row["ts_iso"] = now.strftime("%Y-%m-%d %H:%M:%S")
    row["ts_unix"] = int(now.timestamp())
    try:
        st = fetch(f"{base}/api/status")
        row["t1"] = st.get("t1", "")
        row["t2"] = st.get("t2", "")
        row["t3"] = st.get("t3", "")
        row["total_counts"] = st.get("total_counts", "")
        row["cpu_load"] = st.get("cpu_load", "")
        row["analyzer_connected"] = int(bool(st.get("analyzer_connected", 0)))
        row["wifi_connected"] = int(bool(st.get("wifi_connected", 0)))
    except (urllib.error.URLError, TimeoutError, ConnectionError, json.JSONDecodeError) as e:
        row["error"] = f"status:{type(e).__name__}"
        return row
    try:
        sy = fetch(f"{base}/api/system")
        row["uptime_sec"] = sy.get("uptime_sec", "")
        row["free_heap"] = sy.get("free_heap", "")
        row["min_free_heap"] = sy.get("min_free_heap", "")
        row["flash_used"] = sy.get("flash_used", "")
        row["rssi"] = sy.get("rssi", "")
    except (urllib.error.URLError, TimeoutError, ConnectionError, json.JSONDecodeError) as e:
        row["error"] = f"system:{type(e).__name__}"
    return row


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", default="http://atomspectra.local")
    ap.add_argument("--out", required=True)
    ap.add_argument("--interval", type=int, default=600)
    args = ap.parse_args()

    out = Path(args.out)
    write_header = not out.exists() or out.stat().st_size == 0
    print(f"POLL url={args.url} out={out} interval={args.interval}s", flush=True)
    with open(out, "a", encoding="utf-8", newline="") as f:
        w = csv.writer(f, delimiter=";")
        if write_header:
            w.writerow(COLS)
            f.flush()
        while True:
            row = poll_once(args.url)
            w.writerow([row[c] for c in COLS])
            f.flush()
            err = f" ERR={row['error']}" if row["error"] else ""
            print(
                f"{row['ts_iso']} t1={row['t1']} counts={row['total_counts']} "
                f"heap={row['free_heap']} up={row['uptime_sec']}{err}",
                flush=True,
            )
            time.sleep(args.interval)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("STOPPED", flush=True)
