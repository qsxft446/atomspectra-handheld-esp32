# Waterfall PC tools / Инструменты водопада для ПК

PC-side helpers for the AtomSpectra waterfall (spectrogram). See [`../WATERFALL.md`](../WATERFALL.md)
([🇬🇧 EN](../WATERFALL.en.md)) for formats (ASWW / ASWF / N42), the Web API and calibration details.

Вспомогательные инструменты для водопада AtomSpectra. Форматы, Web API и калибровка —
[`../WATERFALL.md`](../WATERFALL.md).

| File | Purpose / Назначение |
|---|---|
| `waterfall_n42.py` | Waterfall → **ANSI N42.42** (`.n42`) for InterSpec / PeakEasy / Cambio |
| `waterfall_viewer.html` | Offline 2D `.n42`/`.aswf` heatmap viewer (open in a browser, no server) |
| `waterfall_client.py` | Capture the live WS stream into an unbounded `.aswf` file |
| `wf_pull_client.py` | CLI: pull finalized segments from the board and stitch them into one `.aswf` on the fly (idempotent, resumable) |
| `wf_recorder_app.py` / `wf_recorder.bat` | Desktop GUI (#REC-12) around `wf_pull_client.py` — Start/Stop, **New file…**, live counters |
| `example-waterfall.n42` | Sample export of a real run — drag it into the viewer to try it / образец реального прогона |

> **Продвинутый вьюер / advanced viewer.** For a full native desktop viewer (3D waterfall
> render, 2D map, a slice/section/sample panel) see
> **[VibeEngineering-LLC/waterfall-viewer](https://github.com/VibeEngineering-LLC/waterfall-viewer)**
> — a separate standalone application, more capable than the single-file HTML viewer here.

```
pip install requests websocket-client    # waterfall_n42.py / waterfall_client.py
```

## waterfall_n42.py — export to ANSI N42.42

Three modes. Calibration (5-coefficient energy polynomial) is taken from the WS header
or the `.aswf` JSON header — **not** from `/api/spectrum.json` (those coefficients are zero).

```bash
# 1) window — pull the current on-board ring (ASWW) and write one .n42
python waterfall_n42.py window <board-ip> -o snapshot.n42

# 2) stream — record the live WS feed until Ctrl+C (or --seconds N), then write .n42
python waterfall_n42.py stream <board-ip> --seconds 360 -o live_soak.n42

# 3) convert — turn a previously captured .aswf into .n42
python waterfall_n42.py convert capture.aswf -o capture.n42 --host <board-ip>
```

Options: `--detector` (default `CsI`), `--seconds` (stream auto-stop), `--host`
(calibration source for `convert` when the `.aswf` header has none).

## waterfall_viewer.html — offline viewer

Open the file directly in a browser (double-click) and drag a `.n42` onto it, or use
**Открыть .n42**. Renders the waterfall as a viridis heatmap; hover shows energy in keV
(when the file carries a calibration) and the bin counts. No network, no server.

A ready-made [`example-waterfall.n42`](example-waterfall.n42) ships next to the viewer —
drag it in to see the result immediately, no board required.

![waterfall_viewer.html — offline heatmap of example-waterfall.n42](../images/waterfall-viewer.png)

## waterfall_client.py — capture to .aswf

Streams `/ws/waterfall` and writes an unbounded `.aswf` (not limited by the on-board
PSRAM ring). Stop with Ctrl+C — the header with the final row count is written on exit.

```bash
python waterfall_client.py <board-ip> -o capture.aswf
```

## wf_pull_client.py — segment pull + on-the-fly stitching (#REC-12)

Pull-based alternative to `waterfall_client.py`: instead of holding a live WS connection,
it lists **finalized** segments on the board (`/api/waterfall/segments`), fetches each one
once, appends its rows into a single growing `.aswf`, and only then deletes the segment on
the board (delete happens after the local file is fsynced to disk — a crash mid-transfer
never loses a segment). Safe against restarts: progress (which segments are already
ingested, running row/duration totals) is tracked in a sidecar `<file>.state.json`, so
stopping and re-running the same command resumes without re-ingesting or duplicating rows.

```bash
python wf_pull_client.py <board-ip> --stitch capture.aswf --interval 60
```

## wf_recorder_app.py / wf_recorder.bat — desktop recorder GUI (#REC-12)

A small tkinter GUI around `wf_pull_client.py` — no extra dependencies beyond `requests`.
Polls the board in the background and shows live counters (rows in file / duration /
instrument temperature / segments on the board), with:

- **▶ Старт / ⏸ Стоп** — start/stop the polling loop
- **Новый файл…** — stop the current recording, pick a new output path, and wipe that
  path's `.aswf` + `.state.json` + `.temps.csv` if they already exist, so the next Start
  begins a genuinely fresh recording instead of resuming/appending to old data
- **Открыть папку** — open the output folder in Explorer

Launch by double-clicking `wf_recorder.bat`, or directly:

```bash
python wf_recorder_app.py --host http://<board-ip> --stitch out.aswf --interval 60
```

### wf_recorder.exe — standalone Windows build (no Python required)

For users without Python: download the prebuilt Windows exe from the
[**wf-recorder-v0.2.1** Release](https://github.com/VibeEngineering-LLC/atomspectra-waterfall-esp32/releases/tag/wf-recorder-v0.2.1)
(~12 MB, self-contained, tkinter + `requests` + `wf_pull_client` bundled).
Double-click to launch, no install. Default output path is `received/spectrogram.aswf`
next to the exe (unlike the .py version, which defaults to `../received/`).

Скачать готовый Windows exe (без установки Python): в
[Release `wf-recorder-v0.2.1`](https://github.com/VibeEngineering-LLC/atomspectra-waterfall-esp32/releases/tag/wf-recorder-v0.2.1).
Двойной клик — GUI откроется. По умолчанию файл записи — `received/spectrogram.aswf`
рядом с exe.

> **v0.2.1 (2026-07-11):** #REC-14 — сборщик авто-ротирует файл шва при смене формата строки
> прошивки (ASWF v3→v5, `row_stride` 16402→16410): вместо потока `error:stitch` старый `.aswf`
> замораживается, новый формат пишется в `<base>__s<stride>.aswf`. Устранён клинч без ack
> (кольцо Flash больше не копится к потере данных). Commit `9d88146`.
>
> **Asset обновлён 2026-07-05:** исправлен `AttributeError: 'NoneType' object has no attribute
> 'reconfigure'` при запуске exe — в режиме `--windowed` PyInstaller обнуляет `sys.stdout`/
> `sys.stderr`; вызов `.reconfigure()` в `wf_recorder_app.py` и `wf_pull_client.py` теперь
> обёрнут guard-ом `if sys.stdout is not None`. Commit `85eadb7`.

To rebuild from source:

```bash
cd scripts
python -m PyInstaller --onefile --windowed --name wf_recorder \
  --hidden-import wf_pull_client --paths . wf_recorder_app.py
# result: scripts/dist/wf_recorder.exe
```

> `.n42` and `.aswf` captures are git-ignored — they are generated artifacts, not source.
> The only exception is the bundled `example-waterfall.n42` sample, kept for the viewer.
