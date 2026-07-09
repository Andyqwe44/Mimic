@echo off
cd /d "%~dp0"
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >NUL 2>&1
if %ERRORLEVEL% NEQ 0 (echo vcvars failed & exit /b 1)
if not exist "build" mkdir "build"

set ROOT=%~dp0..
set CFLAGS=/EHsc /std:c++17 /I src /I dep /I "%ROOT%\capture\include" ^
  /DNDEBUG /O2 /GS- /Gy /Gw /MT
set LFLAGS=d3d11.lib dxgi.lib windowsapp.lib user32.lib gdi32.lib ole32.lib oleaut32.lib ws2_32.lib windowscodecs.lib dwmapi.lib shell32.lib shlwapi.lib
set LINKFLAGS=/OPT:REF /OPT:ICF

echo === Embedding frontend assets (dist -^> embedded_assets.h) ===
node tools\gen_assets.mjs
if %ERRORLEVEL% NEQ 0 (echo gen_assets failed - run "npm run build" in monitor_web first & exit /b 1)

echo === Compiling resources (icon + version) ===
rc.exe /nologo /fo build\app.res app.rc
if %ERRORLEVEL% NEQ 0 (echo rc failed & exit /b 1)

echo === Building monitor_app.exe (PRODUCTION) ===
cl.exe %CFLAGS% /Fo"build\\" /Fe:build\monitor_app.exe ^
  src\main.cpp src\commands.cpp src\virtual_desktop.cpp ^
  build\app.res ^
  dep\WebView2LoaderStatic.lib ^
  "%ROOT%\logger\build\logger.lib" ^
  "%ROOT%\capture\build\common.lib" ^
  "%ROOT%\capture\build\wgc.lib" ^
  "%ROOT%\capture\build\gdi.lib" ^
  "%ROOT%\capture\build\pw.lib" ^
  "%ROOT%\capture\build\screen.lib" ^
  "%ROOT%\capture\build\desktop.lib" ^
  "%ROOT%\input\build\input_common.lib" ^
  "%ROOT%\input\build\input_sendinput.lib" ^
  "%ROOT%\input\build\input_winapi.lib" ^
  "%ROOT%\input\build\input_postmessage.lib" ^
  "%ROOT%\input\build\input_driver.lib" ^
  %LFLAGS% /link %LINKFLAGS%

if %ERRORLEVEL% EQU 0 (
  echo Build OK: monitor_app\build\monitor_app.exe
  echo.
  echo Usage: build\monitor_app.exe   ^(production mode^)
)
