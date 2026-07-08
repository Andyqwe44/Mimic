@echo off
cd /d "%~dp0"
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >NUL 2>&1
if %ERRORLEVEL% NEQ 0 (echo vcvars failed & exit /b 1)
if not exist "build" mkdir "build"

set CFLAGS=/EHsc /std:c++17 /I include /c

echo === Building wgc.lib ===
cl.exe %CFLAGS% /Fo"build\\capture_wgc.obj" src\capture_wgc.cpp || exit /b 1
lib.exe /OUT:build\wgc.lib build\capture_wgc.obj build\capture_wgc_ffi.obj || exit /b 1
echo wgc.lib built OK
