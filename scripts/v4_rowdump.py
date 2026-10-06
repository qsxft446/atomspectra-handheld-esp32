import sys, json, struct, urllib.request
sys.stdout.reconfigure(encoding="utf-8")
H = "http://" + (len(sys.argv) > 1 and sys.argv[1] or "atomspectra-gw.local")  # IP/host платы аргументом

# restore interval 600s (CSRF)
tok = json.loads(urllib.request.urlopen(H+"/api/csrf-token", timeout=6).read())["token"]
req = urllib.request.Request(H+"/api/waterfall/config", data=json.dumps({"interval":600}).encode(),
    headers={"Content-Type":"application/json","X-CSRF-Token":tok}, method="POST")
print("restore600:", urllib.request.urlopen(req, timeout=6).read(), flush=True)

# dump per-row bin-sums seg18
blob = urllib.request.urlopen(H+"/api/waterfall/segment?name=seg_00018.aswf", timeout=15).read()
hlen = struct.unpack_from("<I", blob, 4)[0]
hdr = json.loads(blob[8:8+hlen].decode())
ch = hdr["channels"]; stride = hdr["row_stride"]
fields = {f["name"]: f for f in hdr["row_fields"]}
spec_off = fields["spectrum"]["offset"]; dur_off = fields["duration"]["offset"]
base = hdr["baseline"]["channels"]*4 if "baseline" in hdr else 0
payload = blob[8+hlen+base:]
n = len(payload)//stride
print("seg18 rows", n, "total_at_open", hdr.get("total_at_open"), flush=True)
tot = 0
for i in range(n):
    b = i*stride
    a = struct.unpack_from("<%dH" % ch, payload, b+spec_off)
    s = sum(a); dur = struct.unpack_from("<H", payload, b+dur_off)[0]
    tot += s
    mark = "  <== SPIKE" if s > 3000 else ""
    if i < 5 or s > 3000 or i >= n-2:
        print("row %2d dur=%d sum=%d%s" % (i, dur, s, mark), flush=True)
print("TOTAL sum_bins", tot, flush=True)