#!/usr/bin/env python3
"""Локальное лёгкое приложение приёма стрима сегментов водопада с платы AtomSpectra.

Что делает:
  1. Поднимает HTTP-сервер СТРОГО на 127.0.0.1 (наружу из LAN не виден,
     дырок в firewall не создаёт — нулевая входящая поверхность ПК сохраняется).
  2. Отдаёт готовый waterfall_viewer.html (лежит рядом) по адресу http://127.0.0.1:<port>/
  3. Проксирует /api/* на плату → браузер работает same-origin, CORS не возникает.
  4. Сам открывает браузер. Дальше на странице: «Ждать сегменты» — авто-приём
     стрима завершённых сегментов каждые 8 с, опция «стереть на плате» (ack-delete),
     ▶/⏸ старт-стоп записи, «Скачать .aswf» — склейка одним файлом.

Запуск (stdlib, зависимостей нет):
  python waterfall_stream_app.py                          # плата http://atomspectra.local
  python waterfall_stream_app.py --board http://<IP>      # явный адрес платы
  python waterfall_stream_app.py --port 8137 --no-browser
"""
import argparse
import sys
import threading
import urllib.error
import urllib.request
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

sys.stdout.reconfigure(encoding="utf-8")
sys.stderr.reconfigure(encoding="utf-8")

HTML_PATH = Path(__file__).resolve().parent / "waterfall_viewer.html"
UPSTREAM_TIMEOUT = 25  # сек; сегмент ~1 МБ по WiFi может идти долго

# системный прокси Windows не должен перехватывать запросы к плате
OPENER = urllib.request.build_opener(urllib.request.ProxyHandler({}))


def make_handler(board: str):
    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, fmt, *args):  # тихий лог: только /api-ошибки печатаем сами
            pass

        def _send(self, code, body: bytes, ctype="text/plain; charset=utf-8"):
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def _proxy(self, method: str):
            url = board + self.path
            data = None
            if method == "POST":
                n = int(self.headers.get("Content-Length") or 0)
                data = self.rfile.read(n) if n else b""
            req = urllib.request.Request(url, data=data, method=method)
            for h in ("X-CSRF-Token", "Content-Type"):
                v = self.headers.get(h)
                if v:
                    req.add_header(h, v)
            try:
                with OPENER.open(req, timeout=UPSTREAM_TIMEOUT) as r:
                    body = r.read()
                    ctype = r.headers.get("Content-Type", "application/octet-stream")
                    self._send(r.status, body, ctype)
            except urllib.error.HTTPError as e:
                # ответ платы с кодом ошибки — отдать браузеру как есть
                body = e.read() or b""
                self._send(e.code, body, e.headers.get("Content-Type", "text/plain"))
            except (urllib.error.URLError, OSError, TimeoutError) as e:
                msg = f"плата недоступна: {e}"
                print(f"[proxy] {method} {self.path} -> {msg}")
                self._send(502, msg.encode("utf-8"))

        def do_GET(self):
            if self.path.startswith("/api/"):
                return self._proxy("GET")
            if self.path in ("/", "/index.html"):
                try:
                    html = HTML_PATH.read_bytes()
                except OSError as e:
                    return self._send(500, f"нет {HTML_PATH.name}: {e}".encode("utf-8"))
                return self._send(200, html, "text/html; charset=utf-8")
            self._send(404, b"not found")

        def do_POST(self):
            if self.path.startswith("/api/"):
                return self._proxy("POST")
            self._send(404, b"not found")

    return Handler


def main():
    ap = argparse.ArgumentParser(description="Локальный приёмник стрима водопада AtomSpectra")
    ap.add_argument("--board", default="http://atomspectra.local",
                    help="адрес платы (default: http://atomspectra.local)")
    ap.add_argument("--port", type=int, default=8137, help="локальный порт (default: 8137)")
    ap.add_argument("--no-browser", action="store_true", help="не открывать браузер")
    args = ap.parse_args()

    board = args.board.rstrip("/")
    if not board.startswith(("http://", "https://")):
        board = "http://" + board

    if not HTML_PATH.is_file():
        print(f"ОШИБКА: не найден {HTML_PATH}")
        return 1

    srv = ThreadingHTTPServer(("127.0.0.1", args.port), make_handler(board))
    url = f"http://127.0.0.1:{args.port}/"
    print(f"Приёмник стрима: {url}  (плата: {board})")
    print("Слушаю ТОЛЬКО 127.0.0.1 — из сети порт не виден. Ctrl+C — выход.")

    if not args.no_browser:
        threading.Timer(0.4, webbrowser.open, args=(url,)).start()

    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\nстоп")
    finally:
        srv.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
