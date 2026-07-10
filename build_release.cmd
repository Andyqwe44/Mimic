@echo off
setlocal enabledelayedexpansion
cd /d "%~dp0"
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
call build_logger_lib.cmd >NUL 2>&1
if %ERRORLEVEL% NEQ 0 (echo   FAILED & popd & exit /b 1)
popd
echo   OK

:: === Build capture ===
echo [2/8] capture DLLs
pushd capture
call build_capture_lib.cmd >NUL 2>&1
if %ERRORLEVEL% NEQ 0 (echo   FAILED & popd & exit /b 1)
popd
echo   OK

:: === Build input ===
echo [3/8] input DLLs
pushd input
call build_input_lib.cmd >NUL 2>&1
if %ERRORLEVEL% NEQ 0 (echo   FAILED & popd & exit /b 1)
popd
echo   OK

:: === Build monitor_app ===
echo [4/8] monitor_app.exe (prod)
pushd monitor_app
call build.cmd >NUL 2>&1
if %ERRORLEVEL% NEQ 0 (echo   FAILED & popd & exit /b 1)
popd
echo   OK

:: === Build updater ===
echo [5/8] updater.exe
pushd updater
call build.cmd >NUL 2>&1
if %ERRORLEVEL% NEQ 0 (echo   FAILED & popd & exit /b 1)
popd
echo   OK

:: === Build frontend ===
echo [6/8] frontend (npm)
pushd monitor_web
call npm run build >NUL 2>&1
if %ERRORLEVEL% NEQ 0 (echo   FAILED & popd & exit /b 1)
popd
echo   OK

:: === Assemble release dir ===
echo [7/8] Assemble release
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
