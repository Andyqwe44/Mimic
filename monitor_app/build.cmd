@echo off
cd /d "%~dp0"
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >NUL 2>&1
if %ERRORLEVEL% NEQ 0 (echo vcvars failed & exit /b 1)
if not exist "build" mkdir "build"

set ROOT=%~dp0..
set CFLAGS=/EHsc /std:c++17 /I src /I dep /I "%ROOT%\capture\include"
set LFLAGS=d3d11.lib dxgi.lib windowsapp.lib user32.lib gdi32.lib ole32.lib oleaut32.lib ws2_32.lib windowscodecs.lib dwmapi.lib

echo === Building monitor_app.exe ===
cl.exe %CFLAGS% /Fo"build\\" /Fe:build\monitor_app.exe ^
  src\main.cpp src\commands.cpp src\mjpeg_server.cpp ^
  dep\WebView2LoaderStatic.lib ^
  "%ROOT%\logger\build\logger.lib" ^
  "%ROOT%\capture\build\common.lib" ^
  "%ROOT%\capture\build\wgc.lib" ^
  "%ROOT%\capture\build\gdi.lib" ^
  "%ROOT%\capture\build\pw.lib" ^
  "%ROOT%\capture\build\screen.lib" ^
  "%ROOT%\capture\build\desktop.lib" ^
  %LFLAGS%

if %ERRORLEVEL% EQU 0 (
  echo Build OK: monitor_app\build\monitor_app.exe
  echo.
  echo Usage: build\monitor_app.exe --dev   ^(Vite HMR^)
  echo        build\monitor_app.exe         ^(production^)
)
