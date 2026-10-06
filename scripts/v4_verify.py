import sys, time, json, struct, zlib, urllib.request
sys.stdout.reconfigure(encoding="utf-8")
H = "http://" + (len(sys.argv) > 1 and sys.argv[1] or "atomspectra-gw.local")  # IP/host платы аргументом
MIN_IDX = int(sys.argv[2]) if len(sys.argv) > 2 else 0  # минимальный idx finalized-сегмента (дефолт 0; было хардкод 18 из v3→v4 перехода)

def segs():
    return json.loads(urllib.request.urlopen(H+"/api/waterfall/segments", timeout=6).read())

def fetch(name):
    return urllib.request.urlopen(H+"/api/waterfall/segment?name="+name, timeout=15).read()

print("wait first v4 seg (idx>=%d)..." % MIN_IDX, flush=True)
target = None
for _ in range(160):
    try:
        lst = [s for s in segs() if s.get("finalized") and int(s["idx"]) >= MIN_IDX]
    except Exception as e:
        print("poll err", e, flush=True); time.sleep(6); continue
    if lst:
        target = sorted(lst, key=lambda s: int(s["idx"]))[0]
        break
    time.sleep(6)

if not target:
    print("NO v4 seg in time", flush=True); sys.exit(1)

name = target["name"]
blob = fetch(name)
hlen = struct.unpack_from("<I", blob, 4)[0]
hdr = json.loads(blob[8:8+hlen].decode())
ver = hdr.get("version"); stride = hdr.get("row_stride"); ch = hdr["channels"]
print("=== %s %dB ===" % (name, len(blob)), flush=True)
print("version", ver, "stride", stride, "seg_seq", hdr.get("seg_seq"),
      "total_at_open", hdr.get("total_at_open"), flush=True)
fields = {f["name"]: f for f in hdr.get("row_fields", [])}
print("row_fields", list(fields), flush=True)

base = 0
if "baseline" in hdr:
    base = hdr["baseline"].get("channels", hdr["baseline"].get("count", 0)) * 4
payload = blob[8+hlen+base:]
n = len(payload) // stride
print("rows", n, "rem", len(payload) % stride, flush=True)

crc_off = fields.get("crc32", {}).get("offset")
covers = fields.get("crc32", {}).get("covers")
spec_off = fields.get("spectrum", {}).get("offset", 0)
bad = 0; checked = 0; sbins = 0
for i in range(n):
    b = i*stride
    a = struct.unpack_from("<%dH" % ch, payload, b+spec_off)
    sbins += sum(a)
    if crc_off is not None:
        want = struct.unpack_from("<I", payload, b+crc_off)[0]
        got = zlib.crc32(payload[b:b+covers]) & 0xFFFFFFFF
        checked += 1
        if got != want:
            bad += 1
            if bad <= 3:
                print("  row %d CRC want %08x got %08x" % (i, want, got), flush=True)
print("CRC32: %d/%d OK%s" % (checked-bad, checked, ("  BAD %d"%bad) if bad else ""), flush=True)
print("sum_bins", sbins, "total_at_open", hdr.get("total_at_open"), flush=True)
print("RESULT:", "v4 END-TO-END OK" if (ver==4 and checked==n and bad==0 and n>0) else "CHECK", flush=True)