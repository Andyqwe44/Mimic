@echo off
cd /d "%~dp0"
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >NUL 2>&1
if %ERRORLEVEL% NEQ 0 (echo vcvars failed & exit /b 1)
if not exist "build" mkdir "build"

set ROOT=%~dp0..

rem --- Extract version from single source (monitor_app/src/version.h) ---
for /f "tokens=3" %%v in ('findstr /c:"#define APP_VERSION " "%ROOT%\monitor_app\src\version.h"') do set VER=%%v
set VER=%VER:"=%
set VER_COMMA=%VER:.=,%,0

rem --- Generate per-module version header (avoids rc.exe quoting hell) ---
echo #define GAM_RC_COMMA %VER_COMMA% > build\_ver_module.h
echo #define GAM_RC_STR "%VER%" >> build\_ver_module.h
echo #define APP_VERSION "%VER%" >> build\_ver_module.h
echo #define GAM_MODULE_DESC "Unified Logging Engine" >> build\_ver_module.h
echo #define GAM_FILETYPE VFT_DLL >> build\_ver_module.h

echo === Building logger.dll (v%VER%) ===
cl.exe /EHsc /std:c++17 /I "%ROOT%\common\include" /I build /DGAM_BUILD_DLL /MT /c /Fo"build\\logger.obj" logger.cpp || exit /b 1
rc.exe /nologo /I build /fo build\logger.res "%ROOT%\common\version.rc" || exit /b 1
link.exe /DLL /NXCOMPAT /DYNAMICBASE /OUT:build\logger.dll build\logger.obj build\logger.res /IMPLIB:build\logger.lib || exit /b 1

echo.
echo === Logger DLL built ===
dir build\*.dll
dir build\*.lib
