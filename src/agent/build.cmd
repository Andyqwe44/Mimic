@echo off
cd /d "%~dp0"
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >NUL 2>&1
if %ERRORLEVEL% NEQ 0 (echo vcvars failed & exit /b 1)

set OUT=..\..\build\agent
if not exist "%OUT%" mkdir "%OUT%"

cl.exe /EHsc /W4 /std:c++17 /I.. /Fe:"%OUT%\agent.exe" main.cpp agent.cpp action_mapper.cpp ../common/signals.cpp ../capture/capture_dxgi.cpp ../capture/preprocess.cpp ../input/input_sendinput.cpp d3d11.lib dxgi.lib user32.lib gdi32.lib ws2_32.lib
if %ERRORLEVEL% EQU 0 (echo Build OK: build\agent\agent.exe)
