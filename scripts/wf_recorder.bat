@echo off
rem AtomSpectra spectrogram recorder launcher (#REC-12).
setlocal
cd /d "%~dp0"
set PYTHONIOENCODING=utf-8
set PYTHONUTF8=1
where python >nul 2>nul
if %errorlevel%==0 ( python wf_recorder_app.py %* ) else ( py -3 wf_recorder_app.py %* )
if errorlevel 1 ( echo. & echo [ERROR] app exited with code %errorlevel% & pause )
endlocal
