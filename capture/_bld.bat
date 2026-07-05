@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >NUL 2>&1
cd /d "%~dp0"
echo Building capture_h264.exe...
cl.exe /EHsc /std:c++17 /I include /Fo"build\\" /Fe:build\capture_h264.exe src\capture_h264.cpp src\mf_encoder.cpp d3d11.lib dxgi.lib dwmapi.lib mfplat.lib mf.lib mfuuid.lib user32.lib gdi32.lib windowsapp.lib ws2_32.lib 1>build\_h264_out.txt 2>&1
echo Exit code: %ERRORLEVEL%
type build\_h264_out.txt | findstr /i "error OK"
