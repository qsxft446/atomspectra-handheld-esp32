/* demo-shim.js — replays a real AtomSpectra ".aswf" capture into the firmware's
 * own Web UI by mocking window.fetch and window.WebSocket. No board required.
 * Data: demo/meta.json + demo/data.bin.gz (gzipped uint16 LE rows, real counts).
 * The firmware HTML pages are byte-for-byte copies; only this shim is injected.
 *
 * #DEMO-2: the capture is FINISHED — we show the real final integrated spectrum
 * (static sum of all rows) and the real final spectrogram (all backfilled rows).
 * No accumulation animation, no looping live stream. */
(function () {
  "use strict";
  var realFetch = window.fetch ? window.fetch.bind(window) : null;
  var RealWS = window.WebSocket;

  var META = null, ROWS = null, CH = 8192, IVS = 30, NROWS = 0, CAL = [];
  var startMs = (window.performance && performance.now) ? performance.now() : 0;
  var resolveReady;
  var ready = new Promise(function (r) { resolveReady = r; });

  async function gunzip(buf) {
    if (typeof DecompressionStream === "undefined")
      throw new Error("DecompressionStream unsupported (need Chrome 80+/FF113+/Safari16.4+)");
    var ds = new DecompressionStream("gzip");
    var stream = new Response(buf).body.pipeThrough(ds);
    return await new Response(stream).arrayBuffer();
  }

  async function load() {
    META = await (await realFetch("meta.json")).json();
    CH = META.channels || 8192;
    IVS = META.integration_sec || META.interval_sec || 30;
    CAL = META.calibration || [];
    NROWS = META.rows || 0;
    var ab = await (await realFetch("data.bin.gz")).arrayBuffer();
    var sig = new Uint8Array(ab.slice(0, 2));
    var raw = (sig[0] === 0x1f && sig[1] === 0x8b) ? await gunzip(ab) : ab;
    ROWS = new Uint16Array(raw);
    if (!NROWS) NROWS = Math.floor(ROWS.length / CH);
    resolveReady();
  }
  load();

  function row(i) { return ROWS.subarray(i * CH, (i + 1) * CH); }

  /* ---- integrated spectrum: REAL FINAL = static sum of ALL rows (computed once) ---- */
  var SPEC = null, lastSpectrum = null;
  function spectrumJSON() {
    if (!SPEC) {
      var acc = new Float64Array(CH);
      for (var i = 0; i < NROWS; i++) {
        var r = row(i);
        for (var c = 0; c < CH; c++) acc[c] += r[c];
      }
      var bins = new Array(CH), total = 0;
      for (var c2 = 0; c2 < CH; c2++) { bins[c2] = acc[c2]; total += acc[c2]; }
      var time = NROWS * IVS;
      // #DT-4: мёртвое время — метод BecqMoni (эталон Am6er), та же формула, что в прошивке:
      // dead = (valid+invalid импульсы) · (RISE+FALL+1)/F. Здесь lost=0, поэтому только total.
      // Для AtomSpectra RISE=11, FALL=27 → (RISE+FALL+1) = 39 отсчётов АЦП, F = 14 МГц.
      var dead = total * 39 / 14000000;
      if (dead > time) dead = time;
      SPEC = {
        calib: CAL, serial: "", bins: bins, total: total, time: time, live: time - dead,
        cps: Math.round(total / Math.max(1, time)), cpu: 12, lost: 0, t1: 24, t2: 25, t3: 25
      };
    }
    lastSpectrum = SPEC;
    return SPEC;
  }

  function uptimeSec() {
    var now = (window.performance && performance.now) ? performance.now() : startMs;
    return Math.floor((now - startMs) / 1000) + 137;
  }
  function systemJSON() {
    var ft = 12582912, fu = 2516582;
    return {
      free_heap: 207360, min_free_heap: 178200, uptime_sec: uptimeSec(),
      flash_total: ft, flash_used: fu,
      wifi_connected: true, ssid: "AtomSpectra-Demo", rssi: -58,
      usb_connected: true, tcp_client: false
    };
  }
  function deviceJSON() {
    return {
      valid: true, version: "demo", mode: 0, tc_on: false,
      t1: 24, t2: 25, t3: 25, serial: "",
      calibration: CAL, calib_order: Math.max(0, CAL.length - 1)
    };
  }
  /* capture finished: recording=false, full rectime, all rows in ring */
  function wfStatusJSON() {
    return {
      recording: false, interval_sec: IVS, started_at: 0, persist: false,
      total_rows: NROWS, ring_count: NROWS, ring_capacity: Math.max(NROWS, 2048),
      flash_rows: 0, flash_full: false
    };
  }
  /* backfill window: "ASWW" + ch + nr + first + interval + all rows (uint16 LE) */
  function windowBuffer() {
    var nr = NROWS, base = 20;
    var buf = new ArrayBuffer(base + nr * CH * 2);
    var dv = new DataView(buf);
    dv.setUint8(0, 0x41); dv.setUint8(1, 0x53); dv.setUint8(2, 0x57); dv.setUint8(3, 0x57);
    dv.setUint32(4, CH, true); dv.setUint32(8, nr, true);
    dv.setUint32(12, 0, true); dv.setUint32(16, IVS, true);
    new Uint16Array(buf, base).set(ROWS.subarray(0, nr * CH));
    return buf;
  }

  function jsonResp(o) {
    return new Response(JSON.stringify(o), { status: 200, headers: { "Content-Type": "application/json" } });
  }
  function binResp(b) {
    return new Response(b, { status: 200, headers: { "Content-Type": "application/octet-stream" } });
  }

  function route(path, init) {
    var method = (init && init.method) ? init.method.toUpperCase() : "GET";
    if (path === "/api/csrf-token") return jsonResp({ token: "demo" });
    if (path === "/api/spectrum.json") return jsonResp(spectrumJSON());
    if (path === "/api/system") return jsonResp(systemJSON());
    if (path === "/api/device") return jsonResp(deviceJSON());
    if (path === "/api/list") return jsonResp({ spectra: [] });
    if (path === "/api/waterfall/status") return jsonResp(wfStatusJSON());
    if (path === "/api/waterfall/window") return binResp(windowBuffer());
    if (path === "/api/waterfall/dose_k") return jsonResp({ dose_k: 0.0006 });
    if (path === "/api/waterfall/dose_curve") return jsonResp({ n: 0 });
    if (path === "/api/save") return jsonResp({ ok: true, index: 1 });
    if (method === "POST") return jsonResp({ ok: true });
    if (/^\/api\/saved\/\d+\/spectrum\.json$/.test(path))
      return jsonResp({ bins: spectrumJSON().bins });
    return jsonResp({ ok: true });
  }

  window.fetch = async function (input, init) {
    var url = (typeof input === "string") ? input : (input && input.url) || "";
    var path = url.replace(/^https?:\/\/[^/]+/, "");
    if (path.indexOf("/api/") === 0) { await ready; return route(path, init); }
    return realFetch(input, init);
  };

  /* ---- mock WebSocket /ws/waterfall: header ONLY (real final, no live stream) ----
   * backfill (/api/waterfall/window) already delivered every row of the finished
   * spectrogram; sending the header lets calib/interval load without simulating
   * an ongoing accumulation. No binary row frames => rows[] stays the real final. */
  function MockWS(url) {
    this.url = url; this.readyState = 0; this.binaryType = "blob";
    this.onopen = this.onmessage = this.onclose = this.onerror = null;
    var self = this;
    ready.then(function () {
      self.readyState = 1;
      if (self.onopen) self.onopen({});
      if (self.onmessage) self.onmessage({ data: JSON.stringify({ channels: CH, interval_sec: IVS, calibration: CAL, serial: "" }) });
    });
  }
  MockWS.prototype.send = function () { };
  MockWS.prototype.close = function () {
    this.readyState = 3;
    if (this.onclose) this.onclose({});
  };
  window.WebSocket = function (url, proto) {
    if (typeof url === "string" && url.indexOf("/ws/waterfall") >= 0) return new MockWS(url);
    return new RealWS(url, proto);
  };
  window.WebSocket.OPEN = 1; window.WebSocket.CLOSED = 3;

  /* ---- make Export buttons actually work (generate from real spectrum) ---- */
  function curSpectrum() { return lastSpectrum || spectrumJSON(); }
  function dl(name, text, mime) {
    var b = new Blob([text], { type: mime || "text/plain" });
    var a = document.createElement("a");
    a.href = URL.createObjectURL(b); a.download = name;
    document.body.appendChild(a); a.click();
    setTimeout(function () { URL.revokeObjectURL(a.href); a.remove(); }, 1000);
  }
  function exportCSV() {
    // #CSV-1: нативный формат Atom Spectra (как в прошивке render_spectrum_csv) —
    // "Channel,Counts (TotalTime=<realtime>.0s)" + "канал,счёт" с 0, CRLF.
    var s = curSpectrum();
    var out = "Channel,Counts (TotalTime=" + s.time.toFixed(1) + "s)\r\n";
    for (var i = 0; i < s.bins.length; i++) out += i + "," + s.bins[i] + "\r\n";
    dl("atomspectra-demo.csv", out, "text/csv");
  }
  function exportXML() {
    var s = curSpectrum();
    var data = s.bins.join(" ");
    var cal = (s.calib || []).join(" ");
    var xml = '<?xml version="1.0"?>\n<ResultDataFile><FormatVersion>120920</FormatVersion>' +
      '<ResultData><EnergySpectrum><NumberOfChannels>' + s.bins.length + '</NumberOfChannels>' +
      '<MeasurementTime>' + s.time + '</MeasurementTime>' +
      '<EnergyCalibration><Coefficients>' + cal + '</Coefficients></EnergyCalibration>' +
      '<Spectrum>' + data + '</Spectrum></EnergySpectrum></ResultData></ResultDataFile>\n';
    dl("atomspectra-demo.xml", xml, "application/xml");
  }
  function fmtG(v) {
    if (!isFinite(v)) return "0";
    var s = v.toPrecision(15);
    if (/[.]/.test(s) && !/[eE]/.test(s)) s = s.replace(/0+$/, "").replace(/[.]$/, "");
    return s;
  }
  function uuid4() {
    var h = "";
    for (var i = 0; i < 32; i++) h += Math.floor(Math.random() * 16).toString(16);
    return h.slice(0, 8) + "-" + h.slice(8, 12) + "-" + h.slice(12, 16) + "-" + h.slice(16, 20) + "-" + h.slice(20, 32);
  }
  function n42Info() {
    return "  <RadInstrumentInformation id=\"RadInstrument\">\r\n" +
      "    <RadInstrumentManufacturerName>KB Radar</RadInstrumentManufacturerName>\r\n" +
      "    <RadInstrumentModelName>ATOM Spectra</RadInstrumentModelName>\r\n" +
      "    <RadInstrumentClassCode>Radionuclide Identifier</RadInstrumentClassCode>\r\n" +
      "    <RadInstrumentVersion>\r\n      <RadInstrumentComponentName>Hardware</RadInstrumentComponentName>\r\n      <RadInstrumentComponentVersion />\r\n    </RadInstrumentVersion>\r\n" +
      "    <RadInstrumentVersion>\r\n      <RadInstrumentComponentName>SoftwareName</RadInstrumentComponentName>\r\n      <RadInstrumentComponentVersion>BecqMoni</RadInstrumentComponentVersion>\r\n    </RadInstrumentVersion>\r\n" +
      "    <RadInstrumentVersion>\r\n      <RadInstrumentComponentName>Software</RadInstrumentComponentName>\r\n      <RadInstrumentComponentVersion>2026.6.15.1</RadInstrumentComponentVersion>\r\n    </RadInstrumentVersion>\r\n" +
      "  </RadInstrumentInformation>\r\n";
  }
  function n42Cal(cal) {
    var c = "";
    for (var i = 0; i < cal.length; i++) c += fmtG(cal[i]) + " ";
    return "  <EnergyCalibration id=\"SpectrumCalibration-0\">\r\n" +
      "    <CoefficientValues>" + c + "</CoefficientValues>\r\n" +
      "  </EnergyCalibration>\r\n";
  }
  function exportN42() {
    var s = curSpectrum(), cal = s.calib || [];
    var d = new Date(Date.now() - Math.round(s.time) * 1000);
    var p2 = function (x) { return (x < 10 ? "0" : "") + x; };
    var dt = p2(d.getDate()) + "." + p2(d.getMonth() + 1) + "." + d.getFullYear() + " " +
      p2(d.getHours()) + ":" + p2(d.getMinutes()) + ":" + p2(d.getSeconds());
    var out = "﻿<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n" +
      "<RadInstrumentData xmlns:xsd=\"http://www.w3.org/2001/XMLSchema\"" +
      " xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\"" +
      " n42DocUUID=\"" + uuid4() + "\" xmlns=\"http://physics.nist.gov/N42/2011/N42\">\r\n" +
      n42Info() + (cal.length ? n42Cal(cal) : "");
    out += "  <RadMeasurement id=\"SpectrumMeasurement-0\">\r\n" +
      "    <MeasurementClassCode>Foreground</MeasurementClassCode>\r\n" +
      "    <StartDateTime>" + dt + "</StartDateTime>\r\n" +
      "    <RealTimeDuration>PT" + Math.round(s.time) + "S</RealTimeDuration>\r\n" +
      (cal.length ? "    <Spectrum id=\"SpectrumData\" radDetectorInformationReference=\"Detector\" energyCalibrationReference=\"SpectrumCalibration-0\">\r\n"
                  : "    <Spectrum id=\"SpectrumData\" radDetectorInformationReference=\"Detector\">\r\n") +
      "      <LiveTimeDuration>PT" + s.live.toFixed(1) + "S</LiveTimeDuration>\r\n" +
      "      <ChannelData compressionCode=\"None\">";
    for (var i = 0; i < s.bins.length; i++) out += (s.bins[i] >>> 0) + " ";
    out += "</ChannelData>\r\n    </Spectrum>\r\n" +
      "    <GrossCounts id=\"GrossForeground\" radDetectorInformationReference=\"Detector\">\r\n" +
      "      <TotalCounts>" + (Math.round(s.total) + (s.lost || 0)) + "</TotalCounts>\r\n" +
      "    </GrossCounts>\r\n  </RadMeasurement>\r\n</RadInstrumentData>";
    dl("atomspectra-demo.n42", out, "application/octet-stream");
  }
  function exportSPE() {
    var s = curSpectrum(), cal = s.calib || [], n = s.bins.length;
    var d = new Date(Date.now() - Math.round(s.time) * 1000);
    var p2 = function (x) { return (x < 10 ? "0" : "") + x; };
    var h = "SHIFR=" + (s.serial || "AtomSpectra") + "\r\n" +
      "CONFIGNAME=Atom Spectra\r\n" +
      "MEASBEGIN=" + p2(d.getDate()) + "-" + p2(d.getMonth() + 1) + "-" + p2(d.getFullYear() % 100) + " " +
      p2(d.getHours()) + ":" + p2(d.getMinutes()) + ":" + p2(d.getSeconds()) + ".00\r\n" +
      "TLIVE=" + s.live.toFixed(2) + "\r\n" +
      "TREAL=" + Math.round(s.time).toFixed(2) + "\r\n" +
      "DETECTOR=Atom Spectra\r\n";
    if (cal.length) {
      var emax = 0;
      for (var i = cal.length - 1; i >= 0; i--) emax = emax * (n - 1) + cal[i];
      if (emax < 0) emax = 0;
      h += "ENBOUNDS=0," + Math.round(emax) + "\r\nENERGY=" + (cal.length - 1);
      for (var j = 0; j < 7; j++) h += "," + (j < cal.length ? cal[j] : 0);
      h += "\r\n";
    }
    h += "SPECTRSIZE=" + n + "\r\nSPECTR=";
    var hb = new TextEncoder().encode(h);
    var out = new Uint8Array(hb.length + n * 4);
    out.set(hb, 0);
    var dv = new DataView(out.buffer, hb.length);
    for (var k = 0; k < n; k++) dv.setUint32(k * 4, s.bins[k] >>> 0, true);
    dl("atomspectra-demo.spe", out, "application/octet-stream");
  }
  document.addEventListener("DOMContentLoaded", function () {
    try {
      document.querySelectorAll("[onclick]").forEach(function (el) {
        var oc = el.getAttribute("onclick") || "";
        if (oc.indexOf("/api/export.csv") >= 0) el.onclick = function (e) { e.preventDefault(); exportCSV(); return false; };
        else if (oc.indexOf("/api/export.xml") >= 0) el.onclick = function (e) { e.preventDefault(); exportXML(); return false; };
        else if (oc.indexOf("export.n42") >= 0) el.onclick = function (e) { e.preventDefault(); exportN42(); return false; };
        else if (oc.indexOf("export.spe") >= 0) el.onclick = function (e) { e.preventDefault(); exportSPE(); return false; };
      });
      /* waterfall page assigns bExportN42.onclick via script (no [onclick] attr);
       * re-assign after page scripts ran so demo doesn't navigate to a 404. */
      var wfN42 = document.getElementById("bExportN42");
      if (wfN42) wfN42.onclick = function (e) { e.preventDefault(); exportN42(); return false; };
      var wfAswf = document.getElementById("bExportAswf");
      if (wfAswf) wfAswf.onclick = function (e) { e.preventDefault(); return false; };
    } catch (e) { }
  });
})();