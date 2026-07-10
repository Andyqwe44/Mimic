@echo off
cd /d "%~dp0"
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >NUL 2>&1
if %ERRORLEVEL% NEQ 0 (echo vcvars failed & exit /b 1)
if not exist "build_dev" mkdir "build_dev"

set ROOT=%~dp0..
set CFLAGS=/EHsc /std:c++17 /I src /I dep /I "%ROOT%\capture\include" /I "%ROOT%\common\include" ^
  /DDEV_MODE /Od /Zi /MT
set LFLAGS=d3d11.lib dxgi.lib windowsapp.lib user32.lib gdi32.lib ole32.lib oleaut32.lib ws2_32.lib windowscodecs.lib dwmapi.lib shell32.lib winhttp.lib
set LINKFLAGS=/DEBUG:FULL

echo === Building monitor_app.exe (DEV MODE) ===
cl.exe %CFLAGS% /Fo"build_dev\\" /Fe:build_dev\monitor_app.exe ^
  src\main.cpp src\commands.cpp src\virtual_desktop.cpp src\paths.cpp ^
  dep\WebView2LoaderStatic.lib ^
  "%ROOT%\logger\build\logger.lib" ^
  "%ROOT%\capture\build\capture_common.lib" ^
  "%ROOT%\capture\build\capture_wgc.lib" ^
  "%ROOT%\capture\build\capture_gdi.lib" ^
  "%ROOT%\capture\build\capture_pw.lib" ^
  "%ROOT%\capture\build\capture_screen.lib" ^
  "%ROOT%\capture\build\capture_desktop.lib" ^
  "%ROOT%\input\build\input_common.lib" ^
  "%ROOT%\input\build\input_sendinput.lib" ^
  "%ROOT%\input\build\input_winapi.lib" ^
  "%ROOT%\input\build\input_postmessage.lib" ^
  "%ROOT%\input\build\input_driver.lib" ^
  %LFLAGS% /link %LINKFLAGS%

if %ERRORLEVEL% EQU 0 (
  echo Build OK: monitor_app\build_dev\monitor_app.exe
  echo.
  echo ^>^> Copying DLLs to build_dev\ ...
  for %%f in (
    "%ROOT%\logger\build\logger.dll"
    "%ROOT%\capture\build\capture_common.dll"
    "%ROOT%\capture\build\capture_wgc.dll"
    "%ROOT%\capture\build\capture_gdi.dll"
    "%ROOT%\capture\build\capture_pw.dll"
    "%ROOT%\capture\build\capture_screen.dll"
    "%ROOT%\capture\build\capture_desktop.dll"
    "%ROOT%\input\build\input_common.dll"
    "%ROOT%\input\build\input_sendinput.dll"
    "%ROOT%\input\build\input_winapi.dll"
    "%ROOT%\input\build\input_postmessage.dll"
    "%ROOT%\input\build\input_driver.dll"
  ) do copy /y %%f build_dev\ >NUL
  echo All DLLs copied.
  echo Usage: build_dev\monitor_app.exe   ^(dev mode -- Vite HMR at localhost:1420^)
)
