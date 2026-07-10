@echo off
cd /d "%~dp0"
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >NUL 2>&1
if %ERRORLEVEL% NEQ 0 (echo vcvars failed & exit /b 1)
if not exist "build" mkdir "build"

set ROOT=%~dp0..
set CFLAGS=/EHsc /std:c++17 /I include /I "%ROOT%\common\include" /I "%ROOT%\monitor_app\src" /I "%ROOT%\capture\include" /DGAM_BUILD_DLL /MT /c
set SYSLIBS=user32.lib "%ROOT%\logger\build\logger.lib"
set LFLAGS=/DLL /NXCOMPAT /DYNAMICBASE

rem --- input_common.dll ---
echo #define GAM_RC_COMMA 0,3,3,0 > build\_ver_module.h
echo #define GAM_RC_STR "0.3.3" >> build\_ver_module.h
echo #define GAM_MODULE_DESC "Input Common Utilities" >> build\_ver_module.h
echo #define GAM_FILETYPE VFT_DLL >> build\_ver_module.h
echo === Building input_common.dll ===
cl.exe %CFLAGS% /Fo"build\\input_common.obj" src\input_common.cpp || exit /b 1
rc.exe /nologo /I build /fo build\input_common.res "%ROOT%\common\version.rc" || exit /b 1
link.exe %LFLAGS% /OUT:build\input_common.dll build\input_common.obj build\input_common.res %SYSLIBS% /IMPLIB:build\input_common.lib || exit /b 1

rem --- input_sendinput.dll ---
echo #define GAM_RC_COMMA 0,3,3,0 > build\_ver_module.h
echo #define GAM_RC_STR "0.3.3" >> build\_ver_module.h
echo #define GAM_MODULE_DESC "SendInput Module" >> build\_ver_module.h
echo #define GAM_FILETYPE VFT_DLL >> build\_ver_module.h
echo === Building input_sendinput.dll ===
cl.exe %CFLAGS% /Fo"build\\input_sendinput.obj" src\input_sendinput.cpp || exit /b 1
rc.exe /nologo /I build /fo build\input_sendinput.res "%ROOT%\common\version.rc" || exit /b 1
link.exe %LFLAGS% /OUT:build\input_sendinput.dll build\input_sendinput.obj build\input_sendinput.res %SYSLIBS% "%ROOT%\input\build\input_common.lib" /IMPLIB:build\input_sendinput.lib || exit /b 1

rem --- input_winapi.dll ---
echo #define GAM_RC_COMMA 0,3,3,0 > build\_ver_module.h
echo #define GAM_RC_STR "0.3.3" >> build\_ver_module.h
echo #define GAM_MODULE_DESC "WinAPI Input Module" >> build\_ver_module.h
echo #define GAM_FILETYPE VFT_DLL >> build\_ver_module.h
echo === Building input_winapi.dll ===
cl.exe %CFLAGS% /Fo"build\\input_winapi.obj" src\input_winapi.cpp || exit /b 1
rc.exe /nologo /I build /fo build\input_winapi.res "%ROOT%\common\version.rc" || exit /b 1
link.exe %LFLAGS% /OUT:build\input_winapi.dll build\input_winapi.obj build\input_winapi.res %SYSLIBS% "%ROOT%\input\build\input_common.lib" /IMPLIB:build\input_winapi.lib || exit /b 1

rem --- input_postmessage.dll ---
echo #define GAM_RC_COMMA 0,3,3,0 > build\_ver_module.h
echo #define GAM_RC_STR "0.3.3" >> build\_ver_module.h
echo #define GAM_MODULE_DESC "PostMessage Input Module" >> build\_ver_module.h
echo #define GAM_FILETYPE VFT_DLL >> build\_ver_module.h
echo === Building input_postmessage.dll ===
cl.exe %CFLAGS% /Fo"build\\input_postmessage.obj" src\input_postmessage.cpp || exit /b 1
rc.exe /nologo /I build /fo build\input_postmessage.res "%ROOT%\common\version.rc" || exit /b 1
link.exe %LFLAGS% /OUT:build\input_postmessage.dll build\input_postmessage.obj build\input_postmessage.res %SYSLIBS% "%ROOT%\input\build\input_common.lib" /IMPLIB:build\input_postmessage.lib || exit /b 1

rem --- input_driver.dll ---
echo #define GAM_RC_COMMA 0,3,3,0 > build\_ver_module.h
echo #define GAM_RC_STR "0.3.3" >> build\_ver_module.h
echo #define GAM_MODULE_DESC "Driver Input Stub" >> build\_ver_module.h
echo #define GAM_FILETYPE VFT_DLL >> build\_ver_module.h
echo === Building input_driver.dll ===
cl.exe %CFLAGS% /Fo"build\\input_driver.obj" src\input_driver.cpp || exit /b 1
rc.exe /nologo /I build /fo build\input_driver.res "%ROOT%\common\version.rc" || exit /b 1
link.exe %LFLAGS% /OUT:build\input_driver.dll build\input_driver.obj build\input_driver.res %SYSLIBS% "%ROOT%\input\build\input_common.lib" /IMPLIB:build\input_driver.lib || exit /b 1

echo.
echo === All input DLLs built ===
dir build\*.dll
dir build\*.lib
