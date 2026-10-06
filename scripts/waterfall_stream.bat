@echo off
rem AtomSpectra waterfall stream receiver launcher.
rem Double-click to start; extra args pass through, e.g.:
rem   waterfall_stream.bat --board http://192.168.x.x
setlocal
cd /d "%~dp0"
set PYTHONIOENCODING=utf-8
set PYTHONUTF8=1

where python >nul 2>nul
if %errorlevel%==0 (
    python waterfall_stream_app.py %*
) else (
    py -3 waterfall_stream_app.py %*
)

if errorlevel 1 (
    echo.
    echo [ERROR] app exited with code %errorlevel%
    pause
)
endlocal
