#!/usr/bin/env python3
# Build the static waterfall demo data from a real .aswf capture.
#
# Reads a real AtomSpectra ".aswf" file (produced by the /waterfall page
# "Stream to disk" button), downsamples rows by summing groups of ROW_GROUP
# consecutive rows (longer integration -> cleaner image), and writes:
#   demo/data.bin.gz  - gzipped uint16 LE rows (rows x channels)
#   demo/meta.json    - channels, calibration, interval, row count, etc.
#
# The demo HTML (demo/index.html) fetches both and renders them with the
# firmware's own waterfall UI code. No live board required.
#
# Regenerate:  python scripts/build_demo.py <path-to.aswf>
#
# NOTE: the source .aswf lives only on the operator PC (its FILENAME may
# contain a local IP); the payload itself carries no IP/MAC/credentials.
# Nothing from the source path is written into the published artifacts.

import sys, os, json, gzip, struct
import numpy as np

DEFAULT_SRC = r"waterfall_capture.aswf"  # local .aswf capture (operator PC); pass an explicit path as argv[1]
ROW_GROUP = 3  # sum N consecutive rows -> one demo row (1120 rows: > waterfall viewport, scrollbar is meaningful)

HERE = os.path.dirname(os.path.abspath(__file__))
OUTDIR = os.path.join(os.path.dirname(HERE), "demo")


def parse_header(blob):
    assert blob[:4] == b"ASWF", "bad magic (not an .aswf file)"
    reserve = struct.unpack("<I", blob[4:8])[0]
    hdr_raw = blob[8:8 + reserve].decode("utf-8", "ignore")
    start = hdr_raw.find("{")
    if start < 0:
        raise ValueError("no JSON header found")
    obj, _ = json.JSONDecoder().raw_decode(hdr_raw, start)
    return reserve, obj


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_SRC
    if not os.path.isfile(src):
        sys.exit("source not found: " + src)

    with open(src, "rb") as f:
        head = f.read(8)
        reserve = struct.unpack("<I", head[4:8])[0]
        full = head + f.read(reserve)
        _, hdr = parse_header(full)
        ch = int(hdr["channels"])
        rows_total = int(hdr.get("saved_rows", 0))
        data = np.fromfile(f, dtype="<u2")

    nfull = data.size // ch
    data = data[:nfull * ch].reshape(nfull, ch)
    if rows_total and rows_total <= nfull:
        data = data[:rows_total]
    nr = data.shape[0]

    ng = nr // ROW_GROUP
    grouped = data[:ng * ROW_GROUP].reshape(ng, ROW_GROUP, ch).sum(axis=1, dtype=np.int64)
    clamped = int(np.count_nonzero(grouped > 65535))
    np.clip(grouped, 0, 65535, out=grouped)
    out = grouped.astype("<u2")
    raw = out.tobytes()

    os.makedirs(OUTDIR, exist_ok=True)
    gz_path = os.path.join(OUTDIR, "data.bin.gz")
    with gzip.open(gz_path, "wb", compresslevel=9) as g:
        g.write(raw)

    iv = int(hdr.get("interval_sec", 10))
    meta = {
        "format": "atomspectra-waterfall-demo",
        "channels": ch,
        "interval_sec": iv,
        "row_group": ROW_GROUP,
        "integration_sec": ROW_GROUP * iv,
        "rows": int(ng),
        "calibration": hdr.get("calibration", []),
        "source": "real AtomSpectra measurement, browser /waterfall WS stream -> .aswf",
        "note": ("Rows summed in groups of %d (%d s integration each) and gzip-packed. "
                 "Channel counts are the real measured values; no IP/MAC/credentials."
                 % (ROW_GROUP, ROW_GROUP * iv)),
    }
    with open(os.path.join(OUTDIR, "meta.json"), "w", encoding="utf-8") as m:
        json.dump(meta, m, ensure_ascii=False, indent=1)

    gz_size = os.path.getsize(gz_path)
    print("source rows x channels : %d x %d" % (nr, ch))
    print("demo rows (group %d)    : %d" % (ROW_GROUP, ng))
    print("raw bytes              : %d (%.2f MB)" % (len(raw), len(raw) / 1048576))
    print("gz bytes               : %d (%.2f MB)" % (gz_size, gz_size / 1048576))
    print("max count after sum    : %d  (clamped: %d)" % (int(out.max()), clamped))
    print("calibration            : %s" % (meta["calibration"],))
    print("wrote: %s" % gz_path)
    print("wrote: %s" % os.path.join(OUTDIR, "meta.json"))


if __name__ == "__main__":
    main()