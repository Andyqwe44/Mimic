@echo off
cd /d "%~dp0"
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >NUL 2>&1
if %ERRORLEVEL% NEQ 0 (echo vcvars failed & exit /b 1)
if not exist "build" mkdir "build"
cl.exe /EHsc /W4 /std:c++17 /I include /Fo"build\\" /Fe:input_test.exe src\test_input.cpp src\input_sendinput.cpp user32.lib
if %ERRORLEVEL% EQU 0 (echo Build OK: input\input_test.exe)
