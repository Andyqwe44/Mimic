@echo off
cd /d "%~dp0"

echo ========================================
echo  Game Agent Monitor — Release Build
echo ========================================
echo.

rem --- Read version ---
set VERSION=0.3.0
if exist "monitor_app\src\version.h" (
  for /f "tokens=3" %%a in ('findstr /c:"#define APP_VERSION" monitor_app\src\version.h') do set VERSION=%%a
  set VERSION=%VERSION:"=%
)
echo Version: %VERSION%

rem --- Step 1: Build all DLLs ---
echo.
echo [1/7] Building logger.dll...
cd logger && call build_logger_lib.cmd >NUL 2>&1
if %ERRORLEVEL% NEQ 0 (echo FAILED & exit /b 1)
cd ..

echo [2/7] Building capture DLLs...
cd capture && call build_capture_lib.cmd >NUL 2>&1
if %ERRORLEVEL% NEQ 0 (echo FAILED & exit /b 1)
cd ..

echo [3/7] Building input DLLs...
cd input && call build_input_lib.cmd >NUL 2>&1
if %ERRORLEVEL% NEQ 0 (echo FAILED & exit /b 1)
cd ..

rem --- Step 2: Build updater.exe ---
echo [4/7] Building updater.exe...
cd updater && call build.cmd >NUL 2>&1
if %ERRORLEVEL% NEQ 0 (echo FAILED & exit /b 1)
cd ..

rem --- Step 3: Build frontend ---
echo [5/7] Building frontend...
cd monitor_web && call npm run build >NUL 2>&1
if %ERRORLEVEL% NEQ 0 (echo FAILED & exit /b 1)
cd ..

rem --- Step 4: Build monitor_app.exe ---
echo [6/7] Building monitor_app.exe...
cd monitor_app && call build.cmd >NUL 2>&1
if %ERRORLEVEL% NEQ 0 (echo FAILED & exit /b 1)
cd ..

rem --- Step 5: Assemble release directory ---
echo [7/7] Assembling release directory...
if exist "release\GameAgentMonitor" rmdir /s /q "release\GameAgentMonitor"
mkdir "release\GameAgentMonitor\bin"
mkdir "release\GameAgentMonitor\frontend"
mkdir "release\GameAgentMonitor\config"

rem Copy binaries
copy /y "monitor_app\build\monitor_app.exe" "release\GameAgentMonitor\bin\" >NUL
copy /y "updater\build\updater.exe"          "release\GameAgentMonitor\bin\" >NUL
copy /y "logger\build\logger.dll"            "release\GameAgentMonitor\bin\" >NUL
copy /y "capture\build\capture_common.dll"   "release\GameAgentMonitor\bin\" >NUL
copy /y "capture\build\capture_wgc.dll"      "release\GameAgentMonitor\bin\" >NUL
copy /y "capture\build\capture_gdi.dll"      "release\GameAgentMonitor\bin\" >NUL
copy /y "capture\build\capture_desktop.dll"  "release\GameAgentMonitor\bin\" >NUL
copy /y "capture\build\capture_pw.dll"       "release\GameAgentMonitor\bin\" >NUL
copy /y "capture\build\capture_screen.dll"   "release\GameAgentMonitor\bin\" >NUL
copy /y "input\build\input_common.dll"       "release\GameAgentMonitor\bin\" >NUL
copy /y "input\build\input_sendinput.dll"    "release\GameAgentMonitor\bin\" >NUL
copy /y "input\build\input_postmessage.dll"  "release\GameAgentMonitor\bin\" >NUL
copy /y "input\build\input_winapi.dll"       "release\GameAgentMonitor\bin\" >NUL
copy /y "input\build\input_driver.dll"       "release\GameAgentMonitor\bin\" >NUL

rem Copy frontend
xcopy /e /y "monitor_web\dist\*" "release\GameAgentMonitor\frontend\" >NUL

rem Copy config default
copy /y "config\settings.default.json" "release\GameAgentMonitor\config\" >NUL

rem Generate version manifest
echo Generating version.json...
node tools\gen_version.mjs "release\GameAgentMonitor" "%VERSION%"

echo.
echo ========================================
echo  Release assembled: release\GameAgentMonitor\
echo ========================================
echo.
echo To create installer: run InnoSetup on installer\setup.iss
echo   "C:\Program Files\Inno Setup 6\ISCC.exe" installer\setup.iss
echo.
