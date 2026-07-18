@echo off
cd /d "%~dp0"
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >NUL 2>&1
if %ERRORLEVEL% NEQ 0 (echo vcvars failed & exit /b 1)
if not exist "build" mkdir "build"
cl.exe /EHsc /std:c++17 /I include /Fo"build\\" /Fe:build\capture_h264.exe src\capture_h264.cpp src\capture_dxgi.cpp src\mf_encoder.cpp d3d11.lib dxgi.lib dwmapi.lib mfplat.lib mf.lib mfuuid.lib user32.lib gdi32.lib windowsapp.lib ws2_32.lib
if %ERRORLEVEL% EQU 0 (echo Build OK: capture\build\capture_h264.exe)

