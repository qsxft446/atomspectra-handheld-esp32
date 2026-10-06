# Стресс-тест Ctrl+F5 / Ctrl+F5 stress test

Документация и сырые UART-логи стресс-теста жёсткой перезагрузки страницы
(Ctrl+F5 / hard reload), относящегося к багу **BUG-AS-05** (см.
[`../../KNOWN_ISSUES.md`](../../KNOWN_ISSUES.md)).

---

## 🇷🇺 Методика

**Цель:** проверить устойчивость Web UI и прошивки к многократной жёсткой
перезагрузке страницы водопада (Ctrl+F5), при которой браузер не использует кэш и
шлёт полный набор HTTP-заголовков.

**Стенд:**
- Плата: ESP32-S3-DevKitC-1 (N16R8), прошивка AtomSpectra Gateway.
- Захват UART: `scripts/serial_capture.ps1` — открывает COM-порт **без** DTR/RTS
  (плата не перезагружается при подключении), пишет строки с таймстампами.
- Браузер: Chrome, страница `/waterfall`, многократный Ctrl+F5 подряд.

**Что отслеживается в логе:**
- `httpd_resp_send_err: 431` / `parse_block: request URI/header too long` —
  отказ httpd из-за переполнения буфера заголовков.
- `rst:0x…`, `boot:`, `abort()`, `Guru Meditation` — перезагрузка/паника прошивки.
- `main: USB:OK WiFi:OK … cpu:…%` — heartbeat (живость и загрузка CPU).
- `wf_web: ws client … connected` — успешные WebSocket-реконнекты после F5.

---

## 🇷🇺 Результаты

| Метрика | До фикса ([`f5-stress-before.log`](f5-stress-before.log)) | После фикса ([`f5-stress-after.log`](f5-stress-after.log)) |
|---|---|---|
| Ответы `431 Request Header Fields Too Large` | присутствуют на каждый Ctrl+F5 | **0** |
| `request URI/header too long` | да | **0** |
| Перезагрузки / паники прошивки (`rst:`/`abort()`/Guru) | 0 | **0** |
| Успешные WS-реконнекты | — | **7** |
| Heartbeat в конце прогона | `USB:OK WiFi:OK cpu:52%` | `USB:OK WiFi:OK cpu:52%` |

**Вывод:** прошивка не падала ни в одном из прогонов — обрушивался только UI в
браузере из-за недогруженных ассетов (часть запросов получала `431`). После
поднятия лимита заголовков httpd (512 → 1024 Б) ответы `431` исчезли, UI
переживает многократный Ctrl+F5 без обрушения.

---

## 🇬🇧 Method

**Goal:** verify Web UI / firmware resilience to repeated hard reload (Ctrl+F5) of
the waterfall page, where the browser bypasses cache and sends the full set of HTTP
headers.

**Rig:**
- Board: ESP32-S3-DevKitC-1 (N16R8), AtomSpectra Gateway firmware.
- UART capture: `scripts/serial_capture.ps1` — opens the COM port **without**
  DTR/RTS (the board is not reset on connect), writes timestamped lines.
- Browser: Chrome, `/waterfall` page, repeated Ctrl+F5 in a row.

**Watched in the log:**
- `httpd_resp_send_err: 431` / `parse_block: request URI/header too long` — httpd
  rejection due to header buffer overflow.
- `rst:0x…`, `boot:`, `abort()`, `Guru Meditation` — firmware reboot/panic.
- `main: USB:OK WiFi:OK … cpu:…%` — heartbeat (liveness and CPU load).
- `wf_web: ws client … connected` — successful WebSocket reconnects after F5.

---

## 🇬🇧 Results

| Metric | Before fix ([`f5-stress-before.log`](f5-stress-before.log)) | After fix ([`f5-stress-after.log`](f5-stress-after.log)) |
|---|---|---|
| `431 Request Header Fields Too Large` responses | present on every Ctrl+F5 | **0** |
| `request URI/header too long` | yes | **0** |
| Firmware reboots / panics (`rst:`/`abort()`/Guru) | 0 | **0** |
| Successful WS reconnects | — | **7** |
| Heartbeat at end of run | `USB:OK WiFi:OK cpu:52%` | `USB:OK WiFi:OK cpu:52%` |

**Conclusion:** the firmware never crashed in either run — only the browser UI broke
because of un-loaded assets (some requests got `431`). After raising the httpd header
limit (512 → 1024 B) the `431` responses are gone and the UI survives repeated
Ctrl+F5 without breaking.

---

## Воспроизведение / Reproduce

```powershell
# 1. Запустить захват UART (плата не ребутится) / start UART capture (no board reset)
pwsh scripts/serial_capture.ps1 -Port COM16 -Out tests/stress/run.log -Seconds 0

# 2. В браузере открыть /waterfall и нажать Ctrl+F5 ~10-20 раз подряд
#    In the browser open /waterfall and hit Ctrl+F5 ~10-20 times in a row

# 3. Остановить захват, проверить лог / stop capture, inspect the log
Select-String -Path tests/stress/run.log -Pattern "431|too long|rst:0x|abort\(\)"
```

Логи в этой папке — сырой UART-вывод без правок (только ANSI-escape удалены
захватчиком). Серийный вывод не содержит IP/MAC/учётных данных.
The logs in this folder are raw UART output (only ANSI escapes stripped by the
capturer). The serial output contains no IP/MAC/credentials.
