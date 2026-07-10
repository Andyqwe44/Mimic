@echo off
cd /d "%~dp0"
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >NUL 2>&1
if %ERRORLEVEL% NEQ 0 (echo vcvars failed & exit /b 1)
if not exist "build" mkdir "build"

set ROOT=%~dp0..

rem --- Generate per-module version header (avoids rc.exe quoting hell) ---
echo #define GAM_RC_COMMA 0,3,4,0 > build\_ver_module.h
echo #define GAM_RC_STR "0.3.4" >> build\_ver_module.h
echo #define GAM_MODULE_DESC "Unified Logging Engine" >> build\_ver_module.h
echo #define GAM_FILETYPE VFT_DLL >> build\_ver_module.h

echo === Building logger.dll ===
cl.exe /EHsc /std:c++17 /I "%ROOT%\common\include" /DGAM_BUILD_DLL /MT /c /Fo"build\\logger.obj" logger.cpp || exit /b 1
rc.exe /nologo /I build /fo build\logger.res "%ROOT%\common\version.rc" || exit /b 1
link.exe /DLL /NXCOMPAT /DYNAMICBASE /OUT:build\logger.dll build\logger.obj build\logger.res /IMPLIB:build\logger.lib || exit /b 1

echo.
echo === Logger DLL built ===
dir build\*.dll
dir build\*.lib
