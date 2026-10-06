#!/usr/bin/env python3
"""Сравнение водопадов n42 vs aswf одного прогона: число строк, длительности, суммы,
детект обрыва последней строки aswf (частичная запись сегмента).

Использование:
  python n42_vs_aswf.py <file.n42> <file.aswf>
"""
import json
import re
import struct
import sys
from pathlib import Path

sys.stdout.reconfigure(encoding="utf-8")


def parse_n42(path):
    x = Path(path).read_text(encoding="utf-8")
    meas = re.findall(r"<RadMeasurement.*?</RadMeasurement>", x, re.S)
    rows = []
    for m in meas:
        sd = re.search(r"<StartDateTime>(.*?)</StartDateTime>", m)
        rt = re.search(r"<RealTimeDuration>PT([\d.]+)S", m)
        cd = re.search(r"<ChannelData([^>]*)>(.*?)</ChannelData>", m, re.S)
        tot = 0
        if cd:
            toks = cd.group(2).split()
            if "CountedZeroes" in cd.group(1):  # 0 <runlen> — runlen НЕ счёт, декодировать
                i = 0
                while i < len(toks):
                    v = int(toks[i])
                    if v == 0:
                        i += 2  # пропустить длину нулевого прогона
                    else:
                        tot += v; i += 1
            else:
                tot = sum(int(v) for v in toks)
        rows.append((sd.group(1) if sd else "?",
                     float(rt.group(1)) if rt else None, tot))
    return rows


def _crc_field(hdr):
    """(covers, offset) для поля crc32 из row_fields, либо None."""
    for f in hdr.get("row_fields", []):
        if f.get("name") == "crc32":
            return f.get("covers"), f.get("offset")
    return None


def parse_aswf(path):
    import zlib
    buf = Path(path).read_bytes()
    assert buf[:4] == b"ASWF", "bad magic"
    hlen = struct.unpack_from("<I", buf, 4)[0]
    hdr = json.loads(buf[8:8 + hlen].decode("utf-8"))
    ch = hdr["channels"]
    stride = hdr.get("row_stride", ch * 2)
    base = 0
    if "baseline" in hdr:  # v4 baseline-блок (uint32×channels) — пропустить, иначе сдвиг строк → CRC-fail
        base = hdr["baseline"].get("channels", hdr["baseline"].get("count", 0)) * 4
    payload = buf[8 + hlen + base:]
    n_full = len(payload) // stride
    rem = len(payload) % stride
    crc_fld = _crc_field(hdr)
    rows = []
    crc_ok = crc_bad = 0
    for i in range(n_full):
        off = i * stride
        spec = struct.unpack_from(f"<{ch}H", payload, off)
        dur = struct.unpack_from("<H", payload, off + ch * 2)[0]
        ts = struct.unpack_from("<I", payload, off + ch * 2 + 2)[0]
        if crc_fld:
            covers, coff = crc_fld
            want = struct.unpack_from("<I", payload, off + coff)[0]
            got = zlib.crc32(payload[off:off + covers]) & 0xFFFFFFFF
            if got == want:
                crc_ok += 1
            else:
                crc_bad += 1
        rows.append((sum(spec), dur, ts))
    return hdr, rows, rem, stride, ch, len(buf), hlen, crc_ok, crc_bad, bool(crc_fld)


def main(n42_path, aswf_path):
    print("=== n42 ===")
    n = parse_n42(n42_path)
    print(f"  строк: {len(n)}")
    durs = [d for _, d, _ in n if d is not None]
    print(f"  длительности: min={min(durs)} max={max(durs)} "
          f"уник={sorted(set(durs))[:12]}")
    print(f"  суммы: первая={n[0][2]} последняя={n[-1][2]} "
          f"min={min(t for _,_,t in n)} max={max(t for _,_,t in n)}")

    print("\n=== aswf ===")
    hdr, a, rem, stride, ch, fbytes, hlen, crc_ok, crc_bad, has_crc = parse_aswf(aswf_path)
    saved = hdr.get("saved_rows")
    print(f"  version={hdr.get('version')}  saved_rows(hdr)={saved}  row_stride={stride}  channels={ch}")
    if has_crc:
        print(f"  CRC32 per-row: OK={crc_ok}  BAD={crc_bad}  (из {len(a)} полных строк)")
    else:
        print(f"  CRC32 per-row: поле отсутствует в row_fields (формат до #DATA-1a)")
    print(f"  file_bytes={fbytes}  header_len={hlen}  полных строк={len(a)}  хвост={rem} байт")
    if rem:
        got_ch = rem // 2
        print(f"  ⚠ ОБРЫВ последней строки: {rem}/{stride} байт "
              f"(={got_ch}/{ch} каналов, хвост-поля dur/ts/gps/dose потеряны)")
    if saved:  # #FW-14: saved_rows=0 = штатная конвенция «строк — из размера файла», не ошибка
        if saved != len(a):
            print(f"  ⚠ saved_rows={saved} ≠ полных строк={len(a)} "
                  f"(разница {saved-len(a)} = недописанная строка)")
    durs_a = [d for _, d, _ in a]
    print(f"  длительности: min={min(durs_a)} max={max(durs_a)} уник={sorted(set(durs_a))[:12]}")
    print(f"  суммы: первая={a[0][0]} последняя(полн.)={a[-1][0]} "
          f"min={min(s for s,_,_ in a)} max={max(s for s,_,_ in a)}")

    print("\n=== СВЕРКА ===")
    print(f"  n42 строк={len(n)}  aswf полных строк={len(a)}  saved_rows={saved}")
    k = min(len(n), len(a))
    mism = 0
    for i in range(k):
        ns, as_ = n[i][2], a[i][0]
        if ns != as_:
            mism += 1
            if mism <= 8:
                print(f"  строка[{i}]: n42_sum={ns} != aswf_sum={as_} (Δ={ns-as_})")
    if mism == 0:
        print(f"  первые {k} строк: суммы n42==aswf совпадают ✅")
    else:
        print(f"  расхождений сумм: {mism}/{k}")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
