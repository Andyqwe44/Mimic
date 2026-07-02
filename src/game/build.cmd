@echo off
cd /d "%~dp0"
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >NUL 2>&1
if %ERRORLEVEL% NEQ 0 (echo vcvars failed & exit /b 1)

set OUT=..\..\build\game
if not exist "%OUT%" mkdir "%OUT%"

cl.exe /EHsc /W4 /std:c++17 /I.. /Fe:"%OUT%\main.exe" main.cpp config.cpp board.cpp network.cpp tui.cpp ../common/signals.cpp ws2_32.lib
if %ERRORLEVEL% EQU 0 (echo Build OK: build\game\main.exe)
