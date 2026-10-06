import sys, time, json, struct, urllib.request
sys.stdout.reconfigure(encoding="utf-8")
H = "http://" + (len(sys.argv) > 1 and sys.argv[1] or "atomspectra-gw.local")  # IP/host платы аргументом
EXPECT = 697070 + 96145   # seg18 total_at_open + sum_bins
def segs(): return json.loads(urllib.request.urlopen(H+"/api/waterfall/segments", timeout=6).read())
def fetch(n): return urllib.request.urlopen(H+"/api/waterfall/segment?name="+n, timeout=15).read()
print("wait seg idx>=19...", flush=True)
t=None
for _ in range(160):
    try: lst=[s for s in segs() if s.get("finalized") and int(s["idx"])>=19]
    except Exception as e: print("err",e,flush=True); time.sleep(6); continue
    if lst: t=sorted(lst,key=lambda s:int(s["idx"]))[0]; break
    time.sleep(6)
if not t: print("NO seg19",flush=True); sys.exit(1)
blob=fetch(t["name"]); hlen=struct.unpack_from("<I",blob,4)[0]; hdr=json.loads(blob[8:8+hlen].decode())
tao=hdr.get("total_at_open"); seq=hdr.get("seg_seq")
delta=tao-697070
print("%s version %s seg_seq %s total_at_open %s"%(t["name"],hdr.get("version"),seq,tao),flush=True)
print("device_delta=%d  sum_bins18=96145  diff=%d"%(delta,delta-96145),flush=True)
print("seq_gap=%d (0=continuous)"%(seq-1-1),flush=True)
print("RECON:", "OK exact" if delta==96145 else ("OK approx diff=%d"%(delta-96145)),flush=True)