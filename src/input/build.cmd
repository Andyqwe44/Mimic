@echo off
cd /d "%~dp0"
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >NUL 2>&1
if %ERRORLEVEL% NEQ 0 (echo vcvars failed & exit /b 1)

set OUT=..\..\build\input
if not exist "%OUT%" mkdir "%OUT%"

cl.exe /EHsc /W4 /std:c++17 /Fe:"%OUT%\input_test.exe" test_input.cpp input_sendinput.cpp user32.lib
if %ERRORLEVEL% EQU 0 (echo Build OK: build\input\input_test.exe)
