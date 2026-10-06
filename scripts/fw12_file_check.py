#!/usr/bin/env python3
# #FW-12: разбор дефектов в файлах водопада (N42 с платы / .aswf v2 с приёмника).
# Печатает per-row: dur, сумма счётов, счёты/с; флагует аномалии:
#   - dur != номинал;
#   - сумма строки отклоняется от локальной медианы (окно +-4) более чем на 25%;
#   - строки-нули.
# Запуск: python fw12_file_check.py <файл.n42|файл.aswf> [interval]
import sys, re, struct, statistics

sys.stdout.reconfigure(encoding="utf-8")

ROW_BYTES = 16384
ROW_STRIDE = 16386
CH = 8192


def decode_counted_zeroes(text):
    vals = [int(x) for x in text.split()]
    out = []
    i = 0
    while i < len(vals):
        if vals[i] == 0 and i + 1 < len(vals):
            out.extend([0] * vals[i + 1])
            i += 2
        else:
            out.append(vals[i])
            i += 1
    return out


def rows_from_n42(path):
    with open(path, "r", encoding="utf-8") as f:
        xml = f.read()
    rows = []
    for m in re.finditer(
            r'<RadMeasurement id="m-(\d+)">(.*?)</RadMeasurement>', xml, re.S):
        body = m.group(2)
        rt = re.search(r"<RealTimeDuration>PT([\d.]+)S</RealTimeDuration>", body)
        lt = re.search(r"<LiveTimeDuration>PT([\d.]+)S</LiveTimeDuration>", body)
        cd = re.search(r'<ChannelData[^>]*compressionCode="CountedZeroes"[^>]*>(.*?)</ChannelData>',
                       body, re.S)
        cd_raw = None
        if cd is None:
            cd_raw = re.search(r"<ChannelData[^>]*>(.*?)</ChannelData>", body, re.S)
        bins = decode_counted_zeroes(cd.group(1)) if cd else \
            [int(x) for x in cd_raw.group(1).split()] if cd_raw else []
        rows.append({
            "idx": int(m.group(1)),
            "dur": float(rt.group(1)) if rt else None,
            "live": float(lt.group(1)) if lt else None,
            "sum": sum(bins),
            "nbins": len(bins),
        })
    return rows


def rows_from_aswf(path):
    with open(path, "rb") as f:
        blob = f.read()
    if blob[:4] != b"ASWF":
        raise ValueError("не ASWF")
    reserve = struct.unpack("<I", blob[4:8])[0]
    body = blob[8 + reserve:]
    n = len(body) // ROW_STRIDE
    rows = []
    for i in range(n):
        off = i * ROW_STRIDE
        bins = struct.unpack(f"<{CH}H", body[off:off + ROW_BYTES])
        dur = struct.unpack("<H", body[off + ROW_BYTES:off + ROW_BYTES + 2])[0]
        rows.append({"idx": i, "dur": float(dur), "live": None,
                     "sum": sum(bins), "nbins": CH})
    return rows


def main():
    path = sys.argv[1]
    nominal = float(sys.argv[2]) if len(sys.argv) > 2 else 5.0
    rows = rows_from_n42(path) if path.lower().endswith(".n42") else rows_from_aswf(path)
    n = len(rows)
    sums = [r["sum"] for r in rows]
    print(f"[fw12] {path}: строк {n}, номинал dur={nominal}")
    med_all = statistics.median(sums)
    print(f"[fw12] медиана суммы = {med_all:.0f}, min={min(sums)}, max={max(sums)}")
    flagged = 0
    for r in rows:
        i = r["idx"]
        lo = max(0, i - 4); hi = min(n, i + 5)
        neigh = [rows[j]["sum"] for j in range(lo, hi) if j != i]
        med = statistics.median(neigh) if neigh else med_all
        dev = (r["sum"] - med) / med if med else 0.0
        marks = []
        if r["dur"] != nominal:
            marks.append(f"dur={r['dur']:g}")
        if abs(dev) > 0.25:
            marks.append(f"sum {r['sum']} ({dev:+.0%} от медианы {med:.0f})")
        if r["sum"] == 0:
            marks.append("НУЛЕВАЯ")
        if r["nbins"] != CH:
            marks.append(f"nbins={r['nbins']}")
        if marks:
            flagged += 1
            print(f"[fw12]   [{i:3d}] " + "; ".join(marks))
    print(f"[fw12] аномальных строк: {flagged}/{n}")


if __name__ == "__main__":
    main()
