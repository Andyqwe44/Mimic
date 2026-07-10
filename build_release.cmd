@echo off
setlocal enabledelayedexpansion
cd /d "%~dp0"
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >NUL 2>&1
if %ERRORLEVEL% NEQ 0 (echo vcvars failed & exit /b 1)

:: --- Read version from single source of truth ---
for /f "tokens=3" %%v in ('findstr /c:"#define APP_VERSION " "monitor_app\src\version.h"') do set VER=%%v
set VER=%VER:"=%
if "%VER%"=="" (echo ERROR: Cannot read version from version.h & exit /b 1)
echo.
echo ==========================================
echo   Game Agent Monitor - Release v%VER%
echo ==========================================
echo.

:: --- Step 1-3: Build all libraries ---
echo [1/8] Building logger.dll ...
pushd logger
call build_logger_lib.cmd >NUL 2>&1
if %ERRORLEVEL% NEQ 0 (echo   FAILED & popd & exit /b 1)
popd
echo   OK

echo [2/8] Building capture DLLs ...
pushd capture
call build_capture_lib.cmd >NUL 2>&1
if %ERRORLEVEL% NEQ 0 (echo   FAILED & popd & exit /b 1)
popd
echo   OK

echo [3/8] Building input DLLs ...
pushd input
call build_input_lib.cmd >NUL 2>&1
if %ERRORLEVEL% NEQ 0 (echo   FAILED & popd & exit /b 1)
popd
echo   OK

:: --- Step 4: Build monitor_app (prod) ---
echo [4/8] Building monitor_app.exe (production) ...
pushd monitor_app
call build.cmd >NUL 2>&1
if %ERRORLEVEL% NEQ 0 (echo   FAILED & popd & exit /b 1)
popd
echo   OK

:: --- Step 5: Build updater ---
echo [5/8] Building updater.exe ...
pushd updater
call build.cmd >NUL 2>&1
if %ERRORLEVEL% NEQ 0 (echo   FAILED & popd & exit /b 1)
popd
echo   OK

:: --- Step 6: Build frontend ---
echo [6/8] Building frontend (npm run build) ...
pushd monitor_web
call npm run build >NUL 2>&1
if %ERRORLEVEL% NEQ 0 (echo   FAILED & popd & exit /b 1)
popd
echo   OK

:: --- Step 7: Assemble release directory ---
echo [7/8] Assembling release directory ...
set REL=release\GameAgentMonitor
if exist "%REL%" rmdir /s /q "%REL%"
mkdir "%REL%\bin" 2>NUL
mkdir "%REL%\frontend" 2>NUL
mkdir "%REL%\config" 2>NUL

copy /y logger\build\logger.dll                     %REL%\bin\ >NUL
copy /y capture\build\capture_common.dll             %REL%\bin\ >NUL
copy /y capture\build\capture_wgc.dll                %REL%\bin\ >NUL
copy /y capture\build\capture_gdi.dll                %REL%\bin\ >NUL
copy /y capture\build\capture_pw.dll                 %REL%\bin\ >NUL
copy /y capture\build\capture_screen.dll             %REL%\bin\ >NUL
copy /y capture\build\capture_desktop.dll            %REL%\bin\ >NUL
copy /y input\build\input_common.dll                 %REL%\bin\ >NUL
copy /y input\build\input_sendinput.dll              %REL%\bin\ >NUL
copy /y input\build\input_winapi.dll                 %REL%\bin\ >NUL
copy /y input\build\input_postmessage.dll            %REL%\bin\ >NUL
copy /y input\build\input_driver.dll                 %REL%\bin\ >NUL
copy /y monitor_app\build\monitor_app.exe            %REL%\bin\ >NUL
copy /y updater\build\updater.exe                    %REL%\bin\ >NUL
xcopy /y /e /q monitor_web\dist\*                    %REL%\frontend\ >NUL
copy /y config\settings.default.json                 %REL%\config\ >NUL
echo   OK

:: --- Step 8: Generate version.json + installer ---
echo [8/8] Generating version.json + installer ...
node tools\gen_version.mjs "%REL%" "%VER%" >NUL 2>&1
if %ERRORLEVEL% NEQ 0 (echo   WARNING: gen_version.mjs failed)
if exist "C:\Program Files\Inno Setup 6\ISCC.exe" (
  "C:\Program Files\Inno Setup 6\ISCC.exe" /Q installer\setup.iss >NUL 2>&1
  if !ERRORLEVEL! EQU 0 (echo   version.json + installer: OK) else echo   WARNING: ISCC failed
) else (
  echo   version.json: OK   (Installer SKIPPED - Inno Setup not found)
)

:: --- Summary ---
echo.
echo ==========================================
echo   Release v%VER% - Build Complete
echo ==========================================
echo.
echo   Release dir: %REL%\
echo   Installer:   release\GameAgentMonitor_Setup_v%VER%.exe
echo.
echo Next steps:
echo   1. git add release\GameAgentMonitor\ ^&^& git commit -m "release: v%VER%"
echo   2. git tag v%VER% ^&^& git push origin main --tags
echo   3. Create Gitee release + upload installer
echo.
endlocal
