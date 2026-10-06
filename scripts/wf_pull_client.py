#!/usr/bin/env python3
# -*- coding: utf-8 -*-
r"""
#REC-11/#REC-12 pull-модель: PC-клиент забирает завершённые сегменты водопада с
платы и дозаписывает их строки в ЕДИНЫЙ .aswf-файл спектрограммы (шов на лету).

БЕЗОПАСНОСТЬ (зачем pull, а не push): все соединения — ИСХОДЯЩИЕ с ПК. На рабочем
компе НЕ открывается ни одного входящего порта и не создаётся ни одного правила
firewall. Плата ничего не инициирует; ПК сам опрашивает её и забирает данные.

Цикл одного прохода (#REC-12, режим по умолчанию):
  1. GET  /api/csrf-token                       -> токен для мутирующего запроса
  2. GET  /api/status                           -> t1/t2/t3 -> дозапись в temps.csv
  3. GET  /api/waterfall/segments               -> список сегментов (JSON)
  4. для каждого finalized-сегмента:
       GET  /api/waterfall/segment?name=...      -> сырой .aswf в память
       строки сегмента дозаписываются в единый spectrogram.aswf (fsync),
       сегмент отмечается в state.json; только после этого:
       POST /api/waterfall/segment/delete?name=  -> плата стирает сегмент с Flash
     (X-CSRF-Token в заголовке; удаляем ТОЛЬКО после успешной записи на диск)

Единый файл: шапка = шапка ПЕРВОГО сегмента (saved_rows=0 -> строки считаются из
размера файла, конвенция #FW-14), payload = конкатенация строк всех сегментов.
Абсолютных таймстампов у строк нет (формат v2) — пауза платы между сегментами
детектируется по started_at и печатается предупреждением, но в файл не попадает.

Температура: у платы нет per-row температуры (прибор отдаёт T1/T2/T3 ответом на
-inf раз в 30 мин, #FW-13). Клиент логирует её рядом: <stitch>.temps.csv
(unix_ts;iso;t1;t2;t3) — по строке на проход; мерж со строками — по времени.

Только стандартная библиотека (urllib). mDNS-хост atomspectra.local обходит
VPN-прокси, который может отдавать 503 на прямых LAN-адресах.

Примеры:
  python wf_pull_client.py --once
  python wf_pull_client.py --interval 60 --stitch D:\wf\night_run.aswf
  python wf_pull_client.py --no-stitch --out D:\wf_segments   # старый режим: пофайлово
"""
import argparse
import datetime
import json
import os
import struct
import sys
import time
import urllib.request
import urllib.error
import zlib
from array import array

if sys.stdout is not None:
    sys.stdout.reconfigure(encoding="utf-8")
if sys.stderr is not None:
    sys.stderr.reconfigure(encoding="utf-8")

HTTP_TIMEOUT = 30  # сек на запрос; сегмент ~1 МБ по LAN укладывается с запасом


def http_get(url, headers=None, binary=False):
    req = urllib.request.Request(url, headers=headers or {}, method="GET")
    with urllib.request.urlopen(req, timeout=HTTP_TIMEOUT) as r:
        data = r.read()
    return data if binary else data.decode("utf-8")


def http_post(url, headers=None):
    # тело пустое: имя сегмента идёт в query-строке (симметрично GET /segment)
    req = urllib.request.Request(url, data=b"", headers=headers or {}, method="POST")
    with urllib.request.urlopen(req, timeout=HTTP_TIMEOUT) as r:
        return r.status, r.read().decode("utf-8", "replace")


def get_csrf(host):
    tok = json.loads(http_get(host + "/api/csrf-token")).get("token", "")
    if len(tok) != 32:
        raise RuntimeError(f"плата вернула CSRF-токен неожиданной длины: {len(tok)}")
    return tok


def list_segments(host):
    return json.loads(http_get(host + "/api/waterfall/segments"))


# ---------------------------------------------------------------- .aswf разбор

def parse_aswf(blob, name):
    """-> (hdr dict, prefix bytes magic+hdr+baseline, payload bytes).

    v3: baseline-секция (если "baseline" в заголовке) включается в prefix,
    чтобы merged-файл оставался корректным ASWF v3.
    """
    if blob[:4] != b"ASWF":
        raise ValueError(f"{name}: bad magic {blob[:4]!r}")
    hlen = struct.unpack_from("<I", blob, 4)[0]
    hdr = json.loads(blob[8:8 + hlen].decode("utf-8"))
    baseline_bytes = 0
    if "baseline" in hdr:
        b = hdr["baseline"]
        baseline_bytes = b.get("channels", b.get("count", 0)) * 4
    payload_off = 8 + hlen + baseline_bytes
    return hdr, blob[:payload_off], blob[payload_off:]


def payload_rows_durs(payload, stride, name, hdr=None):
    """Целые строки + сумма длительностей. Некратный хвост (краш при записи)
    отбрасывается с предупреждением — в единый файл идут только целые строки.

    hdr: если передан, берёт смещение поля duration из row_fields (v3).
    v1/v2 fallback: duration в последних 2 байтах каждой строки (stride-2).
    """
    n_rows = len(payload) // stride
    rem = len(payload) % stride
    if rem:
        print(f"  ⚠ {name}: некратный хвост {rem} B отброшен (недописанная строка)")
    # v3: duration offset из row_fields; v1/v2: stride-2 (последние 2 байта строки)
    dur_off = stride - 2
    if hdr and "row_fields" in hdr:
        for f in hdr["row_fields"]:
            if f.get("name") == "duration":
                dur_off = f["offset"]
                break
    dur = 0
    for i in range(n_rows):
        dur += struct.unpack_from("<H", payload, i * stride + dur_off)[0]
    return payload[:n_rows * stride], n_rows, dur


def verify_rows(whole, stride, n_rows, hdr, name):
    """#DATA-1a: сверить per-row CRC32 и посчитать Σ bins (для reconciliation).

    -> (crc_bad, crc_checked, sum_bins). crc_checked=0 для v1..v3 (нет поля crc32) —
    целостность строк не проверяется, только Σ bins считается.
    """
    ch = hdr["channels"]
    crc_off = covers = None
    spec_off = 0
    for f in hdr.get("row_fields", []):
        nm = f.get("name")
        if nm == "crc32":
            crc_off = f["offset"]
            covers = f.get("covers", crc_off)
        elif nm == "spectrum":
            spec_off = f.get("offset", 0)
    crc_bad = crc_checked = 0
    sum_bins = 0
    for i in range(n_rows):
        base = i * stride
        a = array("H")
        a.frombytes(whole[base + spec_off: base + spec_off + ch * 2])
        if sys.byteorder != "little":
            a.byteswap()
        sum_bins += int(sum(a))
        if crc_off is not None:
            want = struct.unpack_from("<I", whole, base + crc_off)[0]
            got = zlib.crc32(whole[base: base + covers]) & 0xFFFFFFFF
            crc_checked += 1
            if got != want:
                crc_bad += 1
    return crc_bad, crc_checked, sum_bins


# ---------------------------------------------------------------- шов (#REC-12)

class Stitcher:
    """Единый .aswf + state.json (идемпотентность и учёт времени).

    state: {"ingested": {name: bytes}, "rows": N, "dur_sum": сек,
            "started_at": unix первого сегмента}
    """

    def __init__(self, path):
        self._set_paths(path)
        self._load_state()

    def _set_paths(self, path):
        self.path = path
        self.state_path = path + ".state.json"
        self.temps_path = path + ".temps.csv"

    def _load_state(self):
        path = self.path
        file_ok = os.path.exists(path) and os.path.getsize(path) >= 8
        if file_ok and os.path.exists(self.state_path):
            with open(self.state_path, "r", encoding="utf-8") as f:
                self.state = json.load(f)
        else:
            if os.path.exists(self.state_path) and not file_ok:
                print(f"  ⚠ {path} отсутствует/пуст, а {self.state_path} есть — "
                      f"файл шва перемещён/удалён вручную, начинаю новый (state сброшен)")
            self.state = {"ingested": {}, "rows": 0, "dur_sum": 0, "started_at": None}

    def _rotated_path(self, stride):
        """#REC-14: путь нового файла шва при смене формата прошивки —
        <base>__s<stride><ext>. Существующий суффикс __s… снимается (не наслаивается
        при второй смене формата)."""
        d = os.path.dirname(self.path)
        root, ext = os.path.splitext(os.path.basename(self.path))
        i = root.rfind("__s")
        if i != -1 and root[i + 3:].isdigit():
            root = root[:i]
        return os.path.join(d, f"{root}__s{stride}{ext}")

    def _rotate_for_format(self, new_ch, new_stride, old_ch, old_stride):
        """Заморозить текущий файл шва и переключиться на новый (под новый формат).
        Старый .aswf/.state.json/.temps.csv остаются на диске нетронутыми."""
        old_path = self.path
        self._set_paths(self._rotated_path(new_stride))
        self._load_state()
        ch_note = "" if old_ch == new_ch else f", ch {old_ch}→{new_ch}"
        print(f"  ⟳ #REC-14 смена формата прошивки платы: stride {old_stride}→{new_stride}"
              f"{ch_note}; файл шва заморожен ({os.path.basename(old_path)}), "
              f"новые строки → {os.path.basename(self.path)}")

    def _save_state(self):
        tmp = self.state_path + ".part"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(self.state, f, ensure_ascii=False)
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, self.state_path)

    def already_ingested(self, name, want_bytes):
        return self.state["ingested"].get(name) == want_bytes

    def append_segment(self, name, blob):
        """Дозаписать строки сегмента в единый файл.
        -> (rows_added, gap_sec|None, diag dict).

        diag: {crc_bad, crc_checked, seq_gap|None, recon|None}
          seq_gap: сколько сегментов пропущено по разрыву seg_seq (#DATA-1b),
                   0 = цепочка непрерывна, None = seg_seq нет в шапке (v<4).
          recon:   (#DATA-1c) для ПРЕДЫДУЩЕГО сегмента —
                   (device_delta, sum_bins, device_delta−sum_bins) или None.
                   #DATA-2: СПРАВОЧНАЯ метрика, не вердикт. Колебания diff —
                   граничное перетекание отсчётов между сегментами (< пуассон-шума).
                   Вердикт целостности = CRC32+seq+size; вход = hist_drop платы.
        """
        hdr, prefix, payload = parse_aswf(blob, name)
        ch = hdr["channels"]
        stride = hdr.get("row_stride", ch * 2)
        whole, n_rows, dur = payload_rows_durs(payload, stride, name, hdr)

        crc_bad, crc_checked, sum_bins = verify_rows(whole, stride, n_rows, hdr, name)

        # #DATA-1b: разрыв глобального seg_seq -> потеря сегмента (кольцо стёрло).
        seq_gap = None
        seq = hdr.get("seg_seq")
        if seq is not None:
            last_seq = self.state.get("last_seg_seq")
            if last_seq is not None:
                seq_gap = seq - last_seq - 1   # 0 = непрерывно, >0 = пропуск, <0 = дубль/реордер

        # #DATA-1c: reconciliation ПРЕДЫДУЩЕГО сегмента. Прибор-дельта событий =
        # total_at_open текущего − total_at_open предыдущего; сверяем с Σ bins пред.
        recon = None
        tao = hdr.get("total_at_open")
        last_tao = self.state.get("last_total_at_open")
        last_sum = self.state.get("last_sum_bins")
        # только для непрерывной цепочки (seq_gap==0): при пропуске дельта охватывает
        # и стёртые сегменты, сравнивать с одним Σ bins некорректно.
        if tao is not None and last_tao is not None and last_sum is not None \
                and (seq_gap == 0 or seq_gap is None):
            device_delta = tao - last_tao
            recon = (device_delta, last_sum, device_delta - last_sum)

        # #REC-14: смена формата строки в прошивке платы (напр. v3 stride 16402 →
        # v5 16410) больше НЕ роняет запись. Активный файл шва замораживается, а
        # новый формат пишется в отдельный <base>__s<stride>.aswf. Мешать строки
        # разной длины в один файл нельзя — читатель раскладывает водопад по stride
        # из шапки, и весь хвост после шва стал бы кашей.
        if os.path.exists(self.path):
            _, fstride, fch = self._file_header()
            if fch != ch or fstride != stride:
                self._rotate_for_format(ch, stride, fch, fstride)
                if self.already_ingested(name, len(blob)):
                    # уже вшит в новый файл (рестарт после ротации, ack платы не
                    # прошёл) — строки не дублируем, снаружи останется только ack
                    return 0, None, None

        gap = None
        if not os.path.exists(self.path):
            # первый сегмент задаёт шапку файла (saved_rows=0 -> derive-from-size)
            with open(self.path, "wb") as f:
                f.write(prefix)
                f.write(whole)
                f.flush()
                os.fsync(f.fileno())
            self.state["started_at"] = hdr.get("started_at")
        else:
            fhdr = self._file_header()[0]
            if hdr.get("calibration") != fhdr.get("calibration"):
                print(f"  ⚠ {name}: калибровка сегмента ≠ калибровке файла шва "
                      f"(в шапке остаётся первая)")
            # детект паузы платы: цепочка от ПРЕДЫДУЩЕГО сегмента (ожидаемое начало
            # = его started_at + его длительность). started_at < 1e9 = часы платы
            # ещё не синхронизированы SNTP (наблюдался started_at=1) — не сравниваем.
            seg_start = hdr.get("started_at")
            last_end = self.state.get("last_end")
            if last_end and seg_start and seg_start > 1e9 and last_end > 1e9:
                delta = seg_start - last_end
                if abs(delta) > 2 * max(hdr.get("interval_sec", 60), 60):
                    gap = delta
            with open(self.path, "ab") as f:
                f.write(whole)
                f.flush()
                os.fsync(f.fileno())

        self.state["ingested"][name] = len(blob)
        self.state["rows"] = self.state.get("rows", 0) + n_rows
        self.state["dur_sum"] = self.state.get("dur_sum", 0) + dur
        if hdr.get("started_at"):
            self.state["last_end"] = hdr["started_at"] + dur
        # #DATA-1b/1c: якорь для проверки следующего сегмента
        if seq is not None:
            self.state["last_seg_seq"] = seq
        if tao is not None:
            self.state["last_total_at_open"] = tao
        self.state["last_sum_bins"] = sum_bins
        self.state["crc_bad_total"] = self.state.get("crc_bad_total", 0) + crc_bad
        self._save_state()
        return n_rows, gap, {"crc_bad": crc_bad, "crc_checked": crc_checked,
                             "seq_gap": seq_gap, "recon": recon}

    def _file_header(self):
        with open(self.path, "rb") as f:
            head = f.read(8)
            hlen = struct.unpack_from("<I", head, 4)[0]
            hdr = json.loads(f.read(hlen).decode("utf-8"))
        ch = hdr["channels"]
        return hdr, hdr.get("row_stride", ch * 2), ch

    def log_temps(self, host):
        """GET /api/status -> t1/t2/t3 в <stitch>.temps.csv (по строке на проход)."""
        try:
            st = json.loads(http_get(host + "/api/status"))
        except (urllib.error.URLError, urllib.error.HTTPError, OSError, ValueError) as e:
            print(f"  ⚠ температура не получена: {e}")
            return None
        if "t1" not in st:
            print("  ⚠ /api/status без t1 (прибор не отдал -inf) — пропуск температуры")
            return None
        t1, t2, t3 = st["t1"], st["t2"], st["t3"]
        if t1 == 0 and t2 == 0 and t3 == 0:
            return None  # прибор ещё не отдал температуру (~раз в 30 мин); 0 не пишем
        now = time.time()
        iso = datetime.datetime.fromtimestamp(now).isoformat(timespec="seconds")
        new = not os.path.exists(self.temps_path)
        with open(self.temps_path, "a", encoding="utf-8") as f:
            if new:
                f.write("unix_ts;iso;t1;t2;t3\n")
            f.write(f"{now:.0f};{iso};{t1};{t2};{t3}\n")
        return t1, t2, t3


# ---------------------------------------------------------------- проходы

def ack_delete(host, name, token):
    st, _ = http_post(host + "/api/waterfall/segment/delete?name=" + name,
                      headers={"X-CSRF-Token": token})
    return st == 200


def fetch_one_filemode(host, out_dir, seg, token):
    """Старый режим (--no-stitch): сегмент отдельным файлом в out_dir.
    Возвращает 'ok' | 'sizemismatch' | 'error:<...>'."""
    name = seg["name"]
    want = int(seg["bytes"])
    dst = os.path.join(out_dir, name)

    # идемпотентность: если файл уже забран целиком, повторно не качаем, но ack шлём
    have = os.path.getsize(dst) if os.path.exists(dst) else -1
    if have != want:
        try:
            blob = http_get(host + "/api/waterfall/segment?name=" + name, binary=True)
        except (urllib.error.URLError, urllib.error.HTTPError) as e:
            return f"error:get:{e}"
        if len(blob) != want:
            return "sizemismatch"                 # не удаляем на плате — заберём позже
        tmp = dst + ".part"
        with open(tmp, "wb") as f:
            f.write(blob)
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, dst)                      # атомарная публикация

    # приём подтверждён (файл на диске == листинг) -> плата освобождает Flash
    try:
        ok = ack_delete(host, name, token)
    except (urllib.error.URLError, urllib.error.HTTPError) as e:
        return f"error:del:{e}"
    return "ok" if ok else "error:del-status"


def fetch_one_stitch(host, stitcher, seg, token):
    """Режим шва (#REC-12): строки в единый файл, потом delete на плате.

    ВСЕГДА возвращает 4-tuple (status, rows, gap, diag):
      status: 'ok' | 'sizemismatch' | 'error:<...>'
      rows:   строк дозаписано (0 если уже вшит/ошибка)
      gap:    пауза платы в секундах или None
      diag:   dict целостности v4 {crc_bad, crc_checked, seq_gap, recon} или None
    """
    name = seg["name"]
    want = int(seg["bytes"])

    if not stitcher.already_ingested(name, want):
        try:
            blob = http_get(host + "/api/waterfall/segment?name=" + name, binary=True)
        except (urllib.error.URLError, urllib.error.HTTPError) as e:
            return f"error:get:{e}", 0, None, None
        if len(blob) != want:
            return "sizemismatch", 0, None, None   # не удаляем — заберём в след. проходе
        try:
            rows, gap, diag = stitcher.append_segment(name, blob)
        except (ValueError, OSError) as e:
            return f"error:stitch:{e}", 0, None, None
    else:
        rows, gap, diag = 0, None, None           # уже вшит; остался только ack

    try:
        ok = ack_delete(host, name, token)
    except (urllib.error.URLError, urllib.error.HTTPError) as e:
        return f"error:del:{e}", rows, gap, diag
    return ("ok" if ok else "error:del-status"), rows, gap, diag


def one_pass(host, out_dir, stitcher):
    token = get_csrf(host)
    if stitcher:
        t = stitcher.log_temps(host)
        if t:
            print(f"  T: t1={t[0]} t2={t[1]} t3={t[2]} -> {os.path.basename(stitcher.temps_path)}")
    segs = list_segments(host)
    pending = sorted([s for s in segs if s.get("finalized")], key=lambda s: int(s["idx"]))
    got = skipped = failed = rows_total = 0
    for seg in pending:
        if stitcher:
            r, rows, gap, diag = fetch_one_stitch(host, stitcher, seg, token)
        else:
            r = fetch_one_filemode(host, out_dir, seg, token)
            rows, gap, diag = 0, None, None
        if r == "ok":
            got += 1
            rows_total += rows
            extra = f" +{rows} строк" if rows else " (уже вшит, только ack)"
            print(f"  ✓ {seg['name']}  {seg['bytes']} B{extra}  стёрт на плате")
            if gap is not None:
                print(f"  ⚠ разрыв времени перед {seg['name']}: {gap:+.0f} с "
                      f"(пауза платы; в файле шва не отражена)")
            if diag:
                cb, cc = diag["crc_bad"], diag["crc_checked"]
                if cc:
                    print(f"    CRC32: {cc - cb}/{cc} OK"
                          + (f"  ✗ ПОРЧА {cb} строк!" if cb else ""))
                sg = diag["seq_gap"]
                if sg is not None and sg > 0:
                    print(f"    ⚠ #DATA-1b пропуск {sg} сегм. (кольцо стёрло до pull) — "
                          f"данные потеряны безвозвратно")
                elif sg is not None and sg < 0:
                    print(f"    ⚠ #DATA-1b seg_seq откат {sg} (дубль/реордер)")
                rc = diag["recon"]
                if rc is not None:
                    dev, sb, d = rc  # d = device_delta − Σbins
                    # #DATA-2 (решение оператора 2026-07-09): recon — СПРАВОЧНАЯ метрика,
                    # НЕ вердикт целостности. Гарантия транспорта = CRC32(строка) +
                    # seq(сегмент) + фикс-размер (проверки выше); входная потеря
                    # детектор→плата = счётчик hist_drop в /api/spectrum.json.
                    # Колебания d — граничное перетекание отсчётов между сегментами
                    # (зазор чтения прибор-total vs финализация; < пуассон-шума,
                    # взаимозачёт по соседям) → метки ✗/⚠ здесь были ложными тревогами.
                    print(f"    recon пред.сегм (справочно): прибор Δ={dev}, "
                          f"Σbins={sb}, d={d:+d}")
        elif r == "sizemismatch":
            skipped += 1
            print(f"  ~ {seg['name']}  размер не сошёлся — повтор в след. проходе")
        else:
            failed += 1
            print(f"  ✗ {seg['name']}  {r}")
    open_cnt = sum(1 for s in segs if not s.get("finalized"))
    tail = ""
    if stitcher:
        tail = (f", файл шва: {stitcher.state.get('rows', 0)} строк / "
                f"{stitcher.state.get('dur_sum', 0)} с")
    print(f"проход: забрано {got} (+{rows_total} строк), отложено {skipped}, "
          f"ошибок {failed}, открытых (пропущены) {open_cnt}{tail}")
    return got, failed


def main():
    ap = argparse.ArgumentParser(
        description="Pull-клиент водопада: сегменты платы -> единый .aswf (#REC-11/#REC-12)")
    ap.add_argument("--host", default="http://atomspectra.local",
                    help="базовый URL платы (default: mDNS, обходит VPN-прокси)")
    ap.add_argument("--out", default=None,
                    help="рабочая папка (default: <script>/../received)")
    ap.add_argument("--stitch", default=None, metavar="FILE",
                    help="путь единого .aswf (default: <out>/spectrogram.aswf)")
    ap.add_argument("--no-stitch", action="store_true",
                    help="старый режим: каждый сегмент отдельным файлом, без шва")
    ap.add_argument("--interval", type=int, default=60,
                    help="секунд между проходами (default 60)")
    ap.add_argument("--once", action="store_true", help="один проход и выход")
    args = ap.parse_args()

    host = args.host.rstrip("/")
    out_dir = args.out or os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                       "..", "received")
    out_dir = os.path.abspath(out_dir)
    os.makedirs(out_dir, exist_ok=True)

    stitcher = None
    if not args.no_stitch:
        stitch_path = os.path.abspath(args.stitch or os.path.join(out_dir, "spectrogram.aswf"))
        stitcher = Stitcher(stitch_path)
        print(f"host={host}  шов={stitch_path}  interval={args.interval}s  once={args.once}")
    else:
        print(f"host={host}  out={out_dir} (пофайлово)  interval={args.interval}s  once={args.once}")

    while True:
        try:
            one_pass(host, out_dir, stitcher)
        except (urllib.error.URLError, urllib.error.HTTPError, RuntimeError) as e:
            print(f"проход не удался: {e}")
        if args.once:
            break
        time.sleep(args.interval)


if __name__ == "__main__":
    main()
