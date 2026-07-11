@echo off
setlocal enabledelayedexpansion
cd /d "%~dp0"
:: git-bash exports NoDefaultCurrentDirectoryInExePath=1 into the cmd it spawns,
:: which stops cmd from searching the current dir → the relative `cmd /c build_x.cmd`
:: steps below would fail with "not recognized". Clear it so this script runs the
:: same whether launched from cmd.exe or from git-bash via `cmd /c`.
set "NoDefaultCurrentDirectoryInExePath="
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >NUL 2>&1
if %ERRORLEVEL% NEQ 0 (echo vcvars failed & exit /b 1)

:: === Read version ===
for /f "tokens=3" %%v in ('findstr /c:"#define APP_VERSION " "monitor_app\src\version.h"') do set VER=%%v
set VER=%VER:"=%
if "%VER%"=="" (echo ERROR: Cannot read version & exit /b 1)
echo.
echo ==========================================
echo   Game Agent Monitor - Release v%VER%
echo ==========================================
echo.

:: === Build logger ===
echo [1/8] logger.dll
pushd logger
cmd /c build_logger_lib.cmd >NUL 2>&1
if %ERRORLEVEL% NEQ 0 (echo   FAILED & popd & exit /b 1)
popd
echo   OK

:: === Build capture ===
echo [2/8] capture DLLs
pushd capture
cmd /c build_capture_lib.cmd >NUL 2>&1
if %ERRORLEVEL% NEQ 0 (echo   FAILED & popd & exit /b 1)
popd
echo   OK

:: === Build input ===
echo [3/8] input DLLs
pushd input
cmd /c build_input_lib.cmd >NUL 2>&1
if %ERRORLEVEL% NEQ 0 (echo   FAILED & popd & exit /b 1)
popd
echo   OK

:: === Build frontend (before monitor_app: build.cmd stages dist into build\frontend) ===
echo [4/8] frontend (npm)
pushd monitor_web
call npm run build >NUL 2>&1
if %ERRORLEVEL% NEQ 0 (echo   FAILED & popd & exit /b 1)
popd
echo   OK

:: === Build updater (before monitor_app: build.cmd copies updater.exe into build\bin) ===
echo [5/8] updater.exe
pushd updater
cmd /c build.cmd >NUL 2>&1
if %ERRORLEVEL% NEQ 0 (echo   FAILED & popd & exit /b 1)
popd
echo   OK

:: === Build monitor_app (stages bin+frontend+config into package layout) ===
echo [6/8] monitor_app.exe (prod)
pushd monitor_app
cmd /c build.cmd >NUL 2>&1
if %ERRORLEVEL% NEQ 0 (echo   FAILED & popd & exit /b 1)
popd
echo   OK

:: === Assemble release dir ===
:: monitor_app\build.cmd now emits build\{bin,frontend,config} in the exact
:: package layout, so the release dir is a straight copy of that plus updater.
echo [7/8] Assemble release
set REL=release\GameAgentMonitor
if exist "%REL%" rmdir /s /q "%REL%"
mkdir "%REL%\bin" 2>NUL
mkdir "%REL%\frontend" 2>NUL
mkdir "%REL%\config" 2>NUL

xcopy /y /e /q monitor_app\build\bin\*      %REL%\bin\ >NUL
xcopy /y /e /q monitor_app\build\frontend\* %REL%\frontend\ >NUL
copy /y updater\build\updater.exe           %REL%\bin\ >NUL
copy /y config\settings.default.json        %REL%\config\ >NUL
echo   OK

:: === version.json + installer ===
echo [8/8] version.json + installer
node tools\gen_version.mjs "%REL%" "%VER%" >NUL 2>&1
if %ERRORLEVEL% NEQ 0 (echo   WARNING: gen_version.mjs failed)

set "ISCC=C:\Program Files\Inno Setup 6\ISCC.exe"
if exist "%ISCC%" (
  "%ISCC%" /DMyAppVersion="%VER%" installer\setup.iss >NUL 2>&1
  if !ERRORLEVEL! EQU 0 (echo   Installer: OK) else (echo   WARNING: ISCC failed)
) else (
  echo   SKIP: Inno Setup 6 not found
)

:: === Isolated verification gate (reproduces the fresh-user experience) ===
:: Copies the assembled package OUTSIDE the repo and launches it. Publishing is
:: blocked unless this passes — this is the step that would have caught the
:: white screen before it reached Gitee.
:: Pass VERIFY_MODE=--auto for non-interactive log-based check (CI/headless);
:: default (empty) prompts Y/N for a human visual check.
echo.
echo === Isolated verification ===
call verify_isolated.cmd %VER% %VERIFY_MODE%
if %ERRORLEVEL% NEQ 0 (
  echo.
  echo ABORT: isolated verification failed — nothing pushed, nothing published.
  exit /b 1
)

:: === Git ===
echo.
echo === Git commit + tag + push ===
git add release\GameAgentMonitor\ monitor_app\src\version.h installer\setup.iss
git commit -m "release: v%VER%"
if %ERRORLEVEL% NEQ 0 (echo   commit skipped)

git push origin :refs/tags/v%VER% >NUL 2>&1
git tag -d v%VER% >NUL 2>&1
git tag v%VER%
git push origin main
if %ERRORLEVEL% NEQ 0 (echo   push FAILED & exit /b 1)
git push origin refs/tags/v%VER%:refs/tags/v%VER%
if %ERRORLEVEL% NEQ 0 (echo   tag push FAILED — delete remote tag first & exit /b 1)

echo.
echo ==========================================
echo   Release v%VER% — DONE
echo ==========================================
echo   Installer: release\GameAgentMonitor_Setup_v%VER%.exe
echo.
echo Next: bash publish_release.sh %VER%
endlocal
