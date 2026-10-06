#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
wf_offload_receiver.py — приёмник автономной выгрузки сегментов водопада (#REC-11-A2).

Плата (atomspectra-waterfall, модуль wf_offload) сама шлёт каждый завершённый
сегмент /storage/wf/seg_NNNNN.aswf на этот приёмник по HTTP POST. Тело запроса —
байты файла, имя — в заголовке X-Filename. Приёмник пишет тело в <dir>/<X-Filename>
и отвечает 200; плата удаляет сегмент с Flash только после 2xx.

Только стандартная библиотека — запускается на любом ПК с Python 3.7+.

Пример:
    python wf_offload_receiver.py --port 8080 --dir ./received
    python wf_offload_receiver.py --port 8080 --user gw --pass secret   # с Basic-auth

URL в конфиге платы (вкладка водопада → «Отправка»): http://<ip-этого-ПК>:8080/wf
(путь любой — приёмник принимает POST на любой путь).
"""
import argparse
import base64
import hmac
import os
import re
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# Машина оператора — cp1251; принудительный UTF-8 для кириллицы в выводе (CLAUDE.md §2).
try:
    sys.stdout.reconfigure(encoding="utf-8")
except Exception:
    pass

# Имя сегмента строго seg_NNNNN.aswf — анти-traversal: ничего другого не пишем.
NAME_RE = re.compile(r"^seg_\d{5}\.aswf$")

# Заполняются из argparse в main().
DEST_DIR = "received"
AUTH_TOKEN = None        # ожидаемый "Basic <base64>" или None (без авторизации)
MAX_BYTES = 8 * 1024 * 1024   # потолок размера сегмента (~1 МБ норма, 8 МБ с запасом)


class Handler(BaseHTTPRequestHandler):
    server_version = "wf-offload-receiver/1.0"

    def _reply(self, code, msg):
        body = msg.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _auth_ok(self):
        if AUTH_TOKEN is None:
            return True
        got = self.headers.get("Authorization", "")
        # constant-time сравнение, чтобы не утекало по таймингу
        return hmac.compare_digest(got, AUTH_TOKEN)

    def do_POST(self):
        if not self._auth_ok():
            self.send_response(401)
            self.send_header("WWW-Authenticate", 'Basic realm="wf-offload"')
            self.send_header("Content-Length", "0")
            self.end_headers()
            return

        name = self.headers.get("X-Filename", "")
        name = os.path.basename(name)              # отрезать любые путевые части
        if not NAME_RE.match(name):
            self._reply(400, "bad X-Filename")
            return

        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            self._reply(400, "bad Content-Length")
            return
        if length <= 0 or length > MAX_BYTES:
            self._reply(400, "bad length")
            return

        # Читаем тело целиком (Content-Length известна), пишем атомарно через .part.
        tmp = os.path.join(DEST_DIR, name + ".part")
        dst = os.path.join(DEST_DIR, name)
        try:
            remaining = length
            with open(tmp, "wb") as f:
                while remaining > 0:
                    chunk = self.rfile.read(min(65536, remaining))
                    if not chunk:
                        break
                    f.write(chunk)
                    remaining -= len(chunk)
            if remaining != 0:
                os.remove(tmp)
                self._reply(400, "short body")
                return
            os.replace(tmp, dst)                   # атомарная подмена
        except OSError as e:
            try:
                os.remove(tmp)
            except OSError:
                pass
            self._reply(500, "write failed: %s" % e)
            return

        print("[OK] %s  (%d bytes)  -> %s" % (name, length, dst), flush=True)
        self._reply(200, "ok")

    def do_GET(self):
        # Удобный health-check в браузере.
        self._reply(200, "wf-offload-receiver alive; POST seg_NNNNN.aswf here")

    def log_message(self, fmt, *args):
        pass   # своё лаконичное логирование в do_POST; гасим дефолтный шум


def main():
    global DEST_DIR, AUTH_TOKEN
    ap = argparse.ArgumentParser(description="Приёмник сегментов водопада (#REC-11-A2)")
    ap.add_argument("--host", default="0.0.0.0", help="интерфейс прослушивания (по умолчанию все)")
    ap.add_argument("--port", type=int, default=8080, help="порт (по умолчанию 8080)")
    ap.add_argument("--dir", default="received", help="папка назначения (по умолчанию ./received)")
    ap.add_argument("--user", default=None, help="логин Basic-auth (опционально)")
    ap.add_argument("--pass", dest="password", default=None, help="пароль Basic-auth (опционально)")
    args = ap.parse_args()

    DEST_DIR = os.path.abspath(args.dir)
    os.makedirs(DEST_DIR, exist_ok=True)
    if args.user is not None:
        token = base64.b64encode(("%s:%s" % (args.user, args.password or "")).encode()).decode()
        AUTH_TOKEN = "Basic " + token

    srv = ThreadingHTTPServer((args.host, args.port), Handler)
    print("Приёмник запущен: http://%s:%d/  → %s%s" % (
        args.host, args.port, DEST_DIR, "  (Basic-auth включён)" if AUTH_TOKEN else ""), flush=True)
    print("Ctrl+C для остановки.", flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\nОстановлено.", flush=True)
        srv.shutdown()


if __name__ == "__main__":
    main()
