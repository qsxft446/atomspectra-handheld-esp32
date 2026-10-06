#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""AtomSpectra Waterfall -> ANSI N42.42 (IEC 62755 / ANSI N42.42-2012) exporter.

Every waterfall row is a delta of the cumulative spectrum over one interval, i.e.
a finished counts-per-period spectrum (8192 channels). Each row is written as one
<RadMeasurement>/<Spectrum>/<ChannelData>, all sharing one <EnergyCalibration>.
Sparse delta rows use N42 'CountedZeroes' compression.

Modes:
  window  <host>      one-shot snapshot of the on-board PSRAM ring (GET /api/waterfall/window)
  stream  <host>      live capture over WebSocket (/ws/waterfall) until Ctrl+C
  convert <in.aswf>   convert an existing .aswf capture (written by waterfall_client.py)

Energy calibration: the REAL 5-coefficient polynomial (E = sum c[i]*ch^i) is delivered
ONLY in the /ws/waterfall connect-header (field 'calibration'); /api/spectrum.json
t1/t2/t3 are zero on this firmware. We read the WS header first and fall back to
spectrum.json. If no calibration is found the EnergyCalibration block is omitted,
so the channel axis stays uncalibrated rather than faked.

Requires: pip install requests websocket-client   (websocket-client for stream + WS calib)
Examples:
  python waterfall_n42.py window  <board-ip> -o snapshot.n42
  python waterfall_n42.py stream  <board-ip> -o live.n42
  python waterfall_n42.py convert capture.aswf -o capture.n42
"""
import sys, os, json, time, struct, argparse, uuid, threading
sys.stdout.reconfigure(encoding="utf-8")

NS = "http://physics.nist.gov/N42/2011/N42"

try:
    import requests
except ImportError:
    requests = None


def iso_dt(epoch):
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(int(epoch)))

def iso_dur(sec):
    return "PT%dS" % int(sec)

def counted_zeroes(row):
    """N42 CountedZeroes: a literal 0 is followed by the run length of zeros."""
    out = []
    n = len(row); i = 0
    while i < n:
        v = row[i]
        if v != 0:
            out.append(str(v)); i += 1
        else:
            j = i
            while j < n and row[j] == 0:
                j += 1
            out.append("0"); out.append(str(j - i)); i = j
    return " ".join(out)

def fetch_calib_serial(host):
    """Returns (coeffs_or_None, serial) from /api/spectrum.json t1/t2/t3 (fallback only)."""
    if requests is None or not host:
        return None, ""
    try:
        r = requests.get("http://%s/api/spectrum.json" % host, timeout=5).json()
    except Exception:
        return None, ""
    t = [float(r.get("t1", 0) or 0), float(r.get("t2", 0) or 0), float(r.get("t3", 0) or 0)]
    coeffs = t if any(abs(x) > 0 for x in t) else None
    return coeffs, str(r.get("serial", "") or "")

def fetch_calib_ws(host, timeout=8):
    """Read the energy calibration polynomial from the /ws/waterfall connect-header.
    The REAL calibration (5-coefficient polynomial) lives only in the WS text header,
    not in /api/spectrum.json. Returns a coeffs list or None."""
    if not host:
        return None
    try:
        import websocket
    except ImportError:
        return None
    try:
        ws = websocket.create_connection("ws://%s/ws/waterfall" % host, timeout=timeout)
    except Exception:
        return None
    coeffs = None
    try:
        msg = ws.recv()
        h = json.loads(msg if isinstance(msg, str) else msg.decode("utf-8"))
        cal = h.get("calibration")
        if isinstance(cal, (list, tuple)) and any(abs(float(x)) > 0 for x in cal):
            coeffs = [float(x) for x in cal]
    except Exception:
        coeffs = None
    finally:
        try:
            ws.close()
        except Exception:
            pass
    return coeffs

def best_calib(host):
    """WS-header calibration (real) preferred; spectrum.json t1/t2/t3 as fallback."""
    coeffs, serial = fetch_calib_serial(host)
    ws_coeffs = fetch_calib_ws(host)
    if ws_coeffs:
        coeffs = ws_coeffs
    return coeffs, serial


class N42Writer:
    def __init__(self, path, channels, interval, started, coeffs, serial, detector):
        self.f = open(path, "w", encoding="utf-8", newline="\n")
        self.channels = channels
        self.interval = interval
        self.started = started if started else int(time.time())
        self.idx = 0
        f = self.f
        f.write('<?xml version="1.0" encoding="UTF-8"?>\n')
        f.write('<RadInstrumentData xmlns="%s" n42DocUUID="%s">\n' % (NS, uuid.uuid4()))
        f.write('  <RadInstrumentInformation id="instr-1">\n')
        f.write('    <RadInstrumentManufacturerName>AtomSpectra</RadInstrumentManufacturerName>\n')
        if serial:
            f.write('    <RadInstrumentIdentifier>%s</RadInstrumentIdentifier>\n' % serial)
        f.write('    <RadInstrumentModelName>AtomSpectra</RadInstrumentModelName>\n')
        f.write('    <RadInstrumentClassCode>Spectroscopic Personal Radiation Detector</RadInstrumentClassCode>\n')
        f.write('    <RadInstrumentVersion><RadInstrumentComponentName>Firmware</RadInstrumentComponentName>'
                '<RadInstrumentComponentVersion>atomspectra-waterfall</RadInstrumentComponentVersion></RadInstrumentVersion>\n')
        f.write('  </RadInstrumentInformation>\n')
        f.write('  <RadDetectorInformation id="det-1">\n')
        f.write('    <RadDetectorCategoryCode>Gamma</RadDetectorCategoryCode>\n')
        f.write('    <RadDetectorKindCode>%s</RadDetectorKindCode>\n' % detector)
        f.write('    <RadDetectorDescription>Scintillation gamma detector, %d channels</RadDetectorDescription>\n' % channels)
        f.write('  </RadDetectorInformation>\n')
        self.ecal_ref = ""
        if coeffs:
            self.ecal_ref = ' energyCalibrationReference="ecal-1"'
            f.write('  <EnergyCalibration id="ecal-1">\n')
            f.write('    <CoefficientValues>%s</CoefficientValues>\n'
                    % " ".join(repr(c) for c in coeffs))
            f.write('  </EnergyCalibration>\n')

    def add_row(self, row):
        i = self.idx
        start = iso_dt(self.started + i * self.interval)
        dur = iso_dur(self.interval)
        f = self.f
        f.write('  <RadMeasurement id="m-%d">\n' % i)
        f.write('    <MeasurementClassCode>Foreground</MeasurementClassCode>\n')
        f.write('    <StartDateTime>%s</StartDateTime>\n' % start)
        f.write('    <RealTimeDuration>%s</RealTimeDuration>\n' % dur)
        f.write('    <Spectrum id="m-%d-s-1" radDetectorInformationReference="det-1"%s>\n'
                % (i, self.ecal_ref))
        f.write('      <LiveTimeDuration>%s</LiveTimeDuration>\n' % dur)
        f.write('      <ChannelData compressionCode="CountedZeroes">%s</ChannelData>\n'
                % counted_zeroes(row))
        f.write('    </Spectrum>\n')
        f.write('  </RadMeasurement>\n')
        self.idx += 1

    def close(self):
        self.f.write('</RadInstrumentData>\n')
        self.f.close()
        return self.idx


def rows_from_payload(blob, channels, stride=None):
    rb = channels * 2               # байт спектра (первые rb каждой строки)
    step = stride if stride else rb  # v4: row_stride с хвост-полями; window/stream: rb
    n = len(blob) // step
    for i in range(n):
        off = i * step
        yield struct.unpack("<%dH" % channels, blob[off:off + rb])


def mode_window(host, out, detector):
    if requests is None:
        sys.exit("pip install requests")
    r = requests.get("http://%s/api/waterfall/window" % host, timeout=15)
    r.raise_for_status()
    d = r.content
    if d[:4] != b"ASWW":
        sys.exit("bad magic: %r" % d[:4])
    ch, rows, first, iv = struct.unpack("<IIII", d[4:20])
    payload = d[20:]
    coeffs, serial = best_calib(host)
    started = int(time.time()) - rows * iv
    w = N42Writer(out, ch, iv, started, coeffs, serial, detector)
    for row in rows_from_payload(payload, ch):
        w.add_row(row)
    n = w.close()
    print("wrote %s : %d measurements, %d channels, interval %ds, ecal=%s"
          % (out, n, ch, iv, "yes" if coeffs else "none"))

def mode_convert(infile, out, host, detector):
    with open(infile, "rb") as f:
        blob = f.read()
    if blob[:4] != b"ASWF":
        sys.exit("bad magic: %r (expected ASWF)" % blob[:4])
    (hlen,) = struct.unpack("<I", blob[4:8])
    hdr = json.loads(blob[8:8 + hlen].decode("utf-8"))
    ch = int(hdr.get("channels", 8192))
    stride = int(hdr.get("row_stride", ch * 2))  # v4: строка = спектр + хвост-поля
    base = 0
    if "baseline" in hdr:  # v4 baseline-блок (uint32×channels) между заголовком и строками — пропустить
        base = hdr["baseline"].get("channels", hdr["baseline"].get("count", 0)) * 4
    payload = blob[8 + hlen + base:]
    iv = int(hdr.get("interval_sec", 5))
    started = int(hdr.get("started_at", 0)) or int(time.time())
    serial = str(hdr.get("serial", "") or "")
    coeffs = None
    cal = hdr.get("calibration")
    if isinstance(cal, (list, tuple)) and any(abs(float(x)) > 0 for x in cal):
        coeffs = [float(x) for x in cal]
    elif host:
        coeffs, s2 = best_calib(host)
        serial = serial or s2
    w = N42Writer(out, ch, iv, started, coeffs, serial, detector)
    for row in rows_from_payload(payload, ch, stride):
        w.add_row(row)
    n = w.close()
    print("wrote %s : %d measurements, %d channels, interval %ds, ecal=%s"
          % (out, n, ch, iv, "yes" if coeffs else "none"))

def mode_stream(host, out, detector, seconds=0):
    try:
        import websocket
    except ImportError:
        sys.exit("pip install websocket-client")
    coeffs, serial = fetch_calib_serial(host)
    st = {"ch": 8192, "iv": 5, "started": int(time.time()), "w": None, "n": 0,
          "coeffs": coeffs, "serial": serial}

    def ensure_writer():
        if st["w"] is None:
            st["w"] = N42Writer(out, st["ch"], st["iv"], st["started"],
                                st["coeffs"], st["serial"], detector)

    def on_message(ws, msg):
        if isinstance(msg, str):
            try:
                h = json.loads(msg)
                if h.get("channels"): st["ch"] = int(h["channels"])
                if h.get("interval_sec"): st["iv"] = int(h["interval_sec"])
                cal = h.get("calibration")
                if isinstance(cal, (list, tuple)) and any(abs(float(x)) > 0 for x in cal):
                    st["coeffs"] = [float(x) for x in cal]
                print("header: channels=%d interval=%ds total_rows=%s ecal=%s"
                      % (st["ch"], st["iv"], h.get("total_rows"),
                         "yes" if st["coeffs"] else "none"))
            except Exception:
                pass
            return
        ensure_writer()
        row = struct.unpack("<%dH" % st["ch"], msg)
        st["w"].add_row(row)
        st["n"] += 1
        if st["n"] % 10 == 0:
            print("\rrows captured: %d" % st["n"], end="", flush=True)

    def on_error(ws, err): print("\nws error:", err)
    def on_close(ws, c, r): print("\nws closed", c, r or "")

    ws = websocket.WebSocketApp("ws://%s/ws/waterfall" % host,
                                on_message=on_message, on_error=on_error, on_close=on_close)
    print("connecting ws://%s/ws/waterfall  ->  %s   (Ctrl+C to finish)" % (host, out))
    if seconds and seconds > 0:
        threading.Timer(seconds, ws.close).start()
        print("auto-stop after %d s" % seconds)
    try:
        ws.run_forever(ping_interval=20, ping_timeout=10)
    except KeyboardInterrupt:
        print("\ninterrupted")
    finally:
        ws.close()
        if st["w"] is not None:
            n = st["w"].close()
            print("wrote %s : %d measurements, %d channels, interval %ds, ecal=%s"
                  % (out, n, st["ch"], st["iv"], "yes" if st["coeffs"] else "none"))
        else:
            print("no rows captured, nothing written")


def main():
    ap = argparse.ArgumentParser(description="AtomSpectra waterfall -> ANSI N42.42")
    ap.add_argument("mode", choices=["window", "stream", "convert"])
    ap.add_argument("source", help="board IP/mDNS (window|stream) or input .aswf (convert)")
    ap.add_argument("-o", "--out", default=None, help="output .n42 path")
    ap.add_argument("--host", default=None, help="board IP for calibration when converting")
    ap.add_argument("--detector", default="CsI",
                    help="RadDetectorKindCode (CsI, NaI, LaBr3, ...); default CsI")
    ap.add_argument("--seconds", type=int, default=0,
                    help="stream mode: auto-stop after N seconds (0 = until Ctrl+C)")
    args = ap.parse_args()
    out = args.out or time.strftime("waterfall_%Y%m%d_%H%M%S.n42")
    if args.mode == "window":
        mode_window(args.source, out, args.detector)
    elif args.mode == "stream":
        mode_stream(args.source, out, args.detector, args.seconds)
    else:
        mode_convert(args.source, out, args.host, args.detector)


if __name__ == "__main__":
    main()