@echo off
cd /d "%~dp0"
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >NUL 2>&1
if %ERRORLEVEL% NEQ 0 (echo vcvars failed & exit /b 1)
if not exist "build" mkdir "build"

set ROOT=%~dp0..
set CFLAGS=/EHsc /std:c++17 /I include /I "%ROOT%\monitor_app\src" /I "%ROOT%\monitor_app\dep" /I "%ROOT%\capture\include" /MT /c

echo === Building input_common.lib ===
cl.exe %CFLAGS% /Fo"build\\input_common.obj" src\input_common.cpp
lib.exe /OUT:build\input_common.lib build\input_common.obj
if %ERRORLEVEL% NEQ 0 (echo input_common.lib FAILED & exit /b 1)

echo === Building input_sendinput.lib ===
cl.exe %CFLAGS% /Fo"build\\input_sendinput.obj" src\input_sendinput.cpp
lib.exe /OUT:build\input_sendinput.lib build\input_sendinput.obj
if %ERRORLEVEL% NEQ 0 (echo input_sendinput.lib FAILED & exit /b 1)

echo === Building input_winapi.lib ===
cl.exe %CFLAGS% /Fo"build\\input_winapi.obj" src\input_winapi.cpp
lib.exe /OUT:build\input_winapi.lib build\input_winapi.obj
if %ERRORLEVEL% NEQ 0 (echo input_winapi.lib FAILED & exit /b 1)

echo === Building input_postmessage.lib ===
cl.exe %CFLAGS% /Fo"build\\input_postmessage.obj" src\input_postmessage.cpp
lib.exe /OUT:build\input_postmessage.lib build\input_postmessage.obj
if %ERRORLEVEL% NEQ 0 (echo input_postmessage.lib FAILED & exit /b 1)

echo === Building input_driver.lib ===
cl.exe %CFLAGS% /Fo"build\\input_driver.obj" src\input_driver.cpp
lib.exe /OUT:build\input_driver.lib build\input_driver.obj
if %ERRORLEVEL% NEQ 0 (echo input_driver.lib FAILED & exit /b 1)

echo.
echo === All input libs built ===
dir build\*.lib
