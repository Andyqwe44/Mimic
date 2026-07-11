@echo off
setlocal enabledelayedexpansion
cd /d "%~dp0"
:: ============================================================================
:: verify_isolated.cmd — Launch the assembled release package from a directory
:: that has NO relationship to this repo, to reproduce exactly what a fresh
:: user experiences after installing from Gitee.
::
:: Why this exists:
::   - White screen only happened on real installs, never in local prod runs.
::   - Two masking factors made local testing lie:
::       1. Stale HKLM InstallPath redirected the exe to a PREVIOUS install's
::          frontend (fixed in paths.cpp — now exe-relative first).
::       2. Running inside the repo/build tree meant frontend paths and WebView2
::          data folder happened to resolve/write fine.
::   - Copying the package OUTSIDE the repo removes those coincidences, so any
::     packaging or path bug surfaces here instead of on the user's machine.
::
:: Safety: touches only %TEMP%\GAM_verify and %LOCALAPPDATA% (user-writable).
::   Never writes HKLM, never touches C:\Program Files, never runs the installer.
:: ============================================================================

set REL=release\GameAgentMonitor
if not exist "%REL%\bin\monitor_app.exe" (
  echo ERROR: %REL%\bin\monitor_app.exe not found. Run build_release.cmd first.
  exit /b 1
)

:: Isolated location: system TEMP, far from the repo tree.
set "ISO=%TEMP%\GAM_verify"
echo.
echo ==========================================
echo   Isolated package verification
echo ==========================================
echo   Source : %CD%\%REL%
echo   Isolate: %ISO%
echo.

:: Kill any pre-existing instance BEFORE touching the isolated dir. A running
:: monitor_app.exe (and its msedgewebview2.exe children) hold open handles to the
:: bin DLLs / log, so rmdir + xcopy would fail with "access denied". Killing
:: first releases those locks. It also frees the single-instance mutex so our
:: launch below owns it, opens the window, and writes the session log (a second
:: instance would exit code 2 with no log — a false "no log produced" failure).
taskkill /IM monitor_app.exe /F >NUL 2>&1
taskkill /IM msedgewebview2.exe /F >NUL 2>&1
timeout /t 2 /nobreak >NUL

if exist "%ISO%" rmdir /s /q "%ISO%"
mkdir "%ISO%" 2>NUL
xcopy /y /e /q /i "%REL%\*" "%ISO%\" >NUL
if %ERRORLEVEL% NEQ 0 (echo ERROR: copy to isolated dir failed & exit /b 1)

echo Package copied to isolated dir. Structure:
dir /b "%ISO%"
echo.
echo Launching monitor_app.exe from isolated dir ...
echo   ^>^> Verify: window renders the UI (NOT a white screen).
echo   ^>^> Verify: version shown = %~1
echo.

:: (stale instances were already killed above, before the isolated-dir copy)
pushd "%ISO%\bin"
start "" monitor_app.exe
popd

:: --auto : non-interactive smoke check (for CI / headless). Inspects the log
:: for success markers instead of asking Y/N. Interactive Y/N is the default.
if /i "%~2"=="--auto" goto :auto_check

echo Waiting for you to inspect the window...
echo.
choice /c YN /m "Did the app render correctly (no white screen)"
set VERIFY_RESULT=!ERRORLEVEL!
goto :finish

:auto_check
:: Poll the session log for a definitive outcome marker instead of one fixed-time
:: check. First-run WebView2 env creation timing is highly variable (observed
:: anywhere from <1s to ~25s), so a fixed wait races the marker. Poll every 3s
:: for up to 90s: pass as soon as "prod: frontend served" appears, fail at once
:: on an env/controller create failure, fail if neither shows within the window.
set "LOGDIR=%ISO%\bin\log"
set VERIFY_RESULT=2
for /L %%i in (1,1,30) do (
  timeout /t 3 /nobreak >NUL
  set "LATEST="
  for /f "delims=" %%L in ('dir /b /o-d "%LOGDIR%\*.log" 2^>NUL') do (
    if not defined LATEST set "LATEST=%LOGDIR%\%%L"
  )
  if defined LATEST (
    findstr /c:"env create failed" /c:"controller create failed" "!LATEST!" >NUL 2>&1
    if not errorlevel 1 (
      echo   AUTO: WebView2 create failure found in log ^(iter %%i^).
      set VERIFY_RESULT=2
      goto :finish
    )
    findstr /c:"prod: frontend served" "!LATEST!" >NUL 2>&1
    if not errorlevel 1 (
      echo   AUTO: prod frontend served OK ^(iter %%i, ~%%i x3s^).
      set VERIFY_RESULT=1
      goto :finish
    )
  )
)
echo   AUTO: no frontend-served marker after 90s — treating as failure.
goto :finish

:finish
:: Stop the isolated instance regardless of outcome. Kill the WebView2 children
:: too so they don't hold the user-data folder / bin DLLs for the next run.
taskkill /IM monitor_app.exe /F >NUL 2>&1
taskkill /IM msedgewebview2.exe /F >NUL 2>&1

if !VERIFY_RESULT! EQU 1 (
  echo.
  echo   VERIFICATION PASSED.
  rmdir /s /q "%ISO%" >NUL 2>&1
  endlocal
  exit /b 0
) else (
  echo.
  echo   VERIFICATION FAILED — do NOT publish this build.
  echo   Isolated dir kept for inspection: %ISO%
  echo   Log: %ISO%\bin\log\
  endlocal
  exit /b 1
)
