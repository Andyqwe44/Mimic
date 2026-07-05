@echo off
REM run_bench.bat — WGC capture → TCP → save benchmark
REM Usage: run_bench.bat [hwnd] [--scale N] [--save-every N] [--duration S]
REM   hwnd       : window handle (required)
REM   --scale N  : max dimension (default 1280, 0 = no scale)
REM   --save N   : save every Nth frame (default 60)
REM   --time S   : run duration in seconds (default 10)

setlocal enabledelayedexpansion
cd /d "%~dp0"

set HWND=%1
set SCALE=1280
set SAVE=60
set DURATION=10

:parse
if "%~2"=="" goto :done
if "%~2"=="--scale" (set SCALE=%~3 & shift & shift & goto :parse)
if "%~2"=="--save"  (set SAVE=%~3  & shift & shift & goto :parse)
if "%~2"=="--time"  (set DURATION=%~3 & shift & shift & goto :parse)
shift
goto :parse
:done

if "%HWND%"=="" (
    echo Usage: run_bench.bat ^<hwnd^> [--scale N] [--save N] [--time S]
    echo.
    echo First, find a window with:
    echo   ..\capture\build\window_list.exe
    exit /b 1
)

echo === WGC Benchmark ===
echo HWND: %HWND%
echo Scale: %SCALE%
echo Save every: %SAVE% frames
echo Duration: %DURATION%s
echo.

REM Start receiver in background
echo [1/3] Starting receiver...
start "WGC Recv" cmd /c "rustc wgc_bench_recv.rs -o build\wgc_bench_recv.exe -C opt-level=3 2>nul && build\wgc_bench_recv.exe --port 9999 --save-every %SAVE% --out-dir wgc_frames"
timeout /t 2 /nobreak >nul

REM Run sender
echo [2/3] Starting sender...
build\wgc_bench_send.exe %HWND% --port 9999 --scale %SCALE% --no-wait
set RESULT=%ERRORLEVEL%

REM Wait for duration or until sender exits
echo [3/3] Waiting %DURATION%s...
timeout /t %DURATION% /nobreak >nul

REM Stop receiver
taskkill /FI "WINDOWTITLE eq WGC Recv" /F >nul 2>nul

echo.
echo === Done ===
echo Frames saved in: examples\wgc_frames\
exit /b %RESULT%
