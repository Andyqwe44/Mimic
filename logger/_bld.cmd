@echo off
cd /d "%~dp0"
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >NUL 2>&1
if %ERRORLEVEL% NEQ 0 (echo vcvars failed & exit /b 1)
if not exist "build" mkdir "build"
echo === Building logger.lib ===
cl.exe /EHsc /std:c++17 /c /Fo"build\\logger.obj" logger.cpp || exit /b 1
lib.exe /OUT:build\logger.lib build\logger.obj || exit /b 1
echo Logger static lib built OK
