@echo off
cd /d "%~dp0"
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >NUL 2>&1
if %ERRORLEVEL% NEQ 0 (echo vcvars failed & exit /b 1)

REM Compile .slint -> C++ header (if source changed)
if not exist appwindow.h (
    echo Compiling appwindow.slint...
    slint_bin\bin\slint-compiler.exe appwindow.slint -o appwindow.h
    if %ERRORLEVEL% NEQ 0 (echo Slint compile failed & exit /b 1)
)

set OUT=..\..\build\monitor
if not exist "%OUT%" mkdir "%OUT%"

cl.exe /EHsc /std:c++20 /W3 /I slint_bin/include/slint /Fe:"%OUT%\monitor.exe" main.cpp /link slint_bin/lib/slint_cpp.dll.lib user32.lib shell32.lib /SUBSYSTEM:CONSOLE
if %ERRORLEVEL% EQU 0 (
    copy /Y slint_bin\lib\slint_cpp.dll "%OUT%\" >nul 2>&1
    if exist slint_bin\bin\slint_qt.dll copy /Y slint_bin\bin\slint_qt.dll "%OUT%\" >nul 2>&1
    echo Build OK: build\monitor\monitor.exe
)
