@echo off
cd /d "%~dp0"
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >NUL 2>&1
if %ERRORLEVEL% NEQ 0 (echo vcvars failed & exit /b 1)
if not exist "build" mkdir "build"

set ROOT=%~dp0..
set CFLAGS=/EHsc /std:c++17 /I include /I "%ROOT%\common\include" /DGAM_BUILD_DLL /c /MT
set SYSLIBS=user32.lib gdi32.lib dwmapi.lib "%ROOT%\logger\build\logger.lib"
set LFLAGS=/DLL /NXCOMPAT /DYNAMICBASE

rem --- capture_common.dll ---
echo #define GAM_RC_COMMA 0,3,0,0 > build\_ver_module.h
echo #define GAM_RC_STR "0.3.0" >> build\_ver_module.h
echo #define GAM_MODULE_DESC "Capture Common Utilities" >> build\_ver_module.h
echo #define GAM_FILETYPE VFT_DLL >> build\_ver_module.h
echo === Building capture_common.dll ===
cl.exe %CFLAGS% /Fo"build\\capture_common.obj" src\capture_common.cpp || exit /b 1
rc.exe /nologo /I build /fo build\capture_common.res "%ROOT%\common\version.rc" || exit /b 1
link.exe %LFLAGS% /OUT:build\capture_common.dll build\capture_common.obj build\capture_common.res %SYSLIBS% /IMPLIB:build\capture_common.lib || exit /b 1

rem --- capture_wgc.dll ---
echo #define GAM_RC_COMMA 0,3,0,0 > build\_ver_module.h
echo #define GAM_RC_STR "0.3.0" >> build\_ver_module.h
echo #define GAM_MODULE_DESC "WGC GPU Capture Module" >> build\_ver_module.h
echo #define GAM_FILETYPE VFT_DLL >> build\_ver_module.h
echo === Building capture_wgc.dll ===
cl.exe %CFLAGS% /Fo"build\\capture_wgc.obj" src\capture_wgc.cpp || exit /b 1
cl.exe %CFLAGS% /Fo"build\\capture_wgc_ffi.obj" src\capture_wgc_ffi.cpp || exit /b 1
rc.exe /nologo /I build /fo build\capture_wgc.res "%ROOT%\common\version.rc" || exit /b 1
link.exe %LFLAGS% /OUT:build\capture_wgc.dll build\capture_wgc.obj build\capture_wgc_ffi.obj build\capture_wgc.res d3d11.lib dxgi.lib windowsapp.lib %SYSLIBS% "%ROOT%\capture\build\capture_common.lib" /IMPLIB:build\capture_wgc.lib || exit /b 1

rem --- capture_gdi.dll ---
echo #define GAM_RC_COMMA 0,3,0,0 > build\_ver_module.h
echo #define GAM_RC_STR "0.3.0" >> build\_ver_module.h
echo #define GAM_MODULE_DESC "GDI GetWindowDC Capture Module" >> build\_ver_module.h
echo #define GAM_FILETYPE VFT_DLL >> build\_ver_module.h
echo === Building capture_gdi.dll ===
cl.exe %CFLAGS% /Fo"build\\capture_gdi.obj" src\capture_gdi.cpp || exit /b 1
rc.exe /nologo /I build /fo build\capture_gdi.res "%ROOT%\common\version.rc" || exit /b 1
link.exe %LFLAGS% /OUT:build\capture_gdi.dll build\capture_gdi.obj build\capture_gdi.res %SYSLIBS% "%ROOT%\capture\build\capture_common.lib" /IMPLIB:build\capture_gdi.lib || exit /b 1

rem --- capture_pw.dll ---
echo #define GAM_RC_COMMA 0,3,0,0 > build\_ver_module.h
echo #define GAM_RC_STR "0.3.0" >> build\_ver_module.h
echo #define GAM_MODULE_DESC "PrintWindow Capture Module" >> build\_ver_module.h
echo #define GAM_FILETYPE VFT_DLL >> build\_ver_module.h
echo === Building capture_pw.dll ===
cl.exe %CFLAGS% /Fo"build\\capture_pw.obj" src\capture_pw.cpp || exit /b 1
rc.exe /nologo /I build /fo build\capture_pw.res "%ROOT%\common\version.rc" || exit /b 1
link.exe %LFLAGS% /OUT:build\capture_pw.dll build\capture_pw.obj build\capture_pw.res %SYSLIBS% "%ROOT%\capture\build\capture_common.lib" /IMPLIB:build\capture_pw.lib || exit /b 1

rem --- capture_screen.dll ---
echo #define GAM_RC_COMMA 0,3,0,0 > build\_ver_module.h
echo #define GAM_RC_STR "0.3.0" >> build\_ver_module.h
echo #define GAM_MODULE_DESC "Screen BitBlt Capture Module" >> build\_ver_module.h
echo #define GAM_FILETYPE VFT_DLL >> build\_ver_module.h
echo === Building capture_screen.dll ===
cl.exe %CFLAGS% /Fo"build\\capture_screen.obj" src\capture_screen.cpp || exit /b 1
rc.exe /nologo /I build /fo build\capture_screen.res "%ROOT%\common\version.rc" || exit /b 1
link.exe %LFLAGS% /OUT:build\capture_screen.dll build\capture_screen.obj build\capture_screen.res %SYSLIBS% "%ROOT%\capture\build\capture_common.lib" /IMPLIB:build\capture_screen.lib || exit /b 1

rem --- capture_desktop.dll ---
echo #define GAM_RC_COMMA 0,3,0,0 > build\_ver_module.h
echo #define GAM_RC_STR "0.3.0" >> build\_ver_module.h
echo #define GAM_MODULE_DESC "Desktop BitBlt Capture Module" >> build\_ver_module.h
echo #define GAM_FILETYPE VFT_DLL >> build\_ver_module.h
echo === Building capture_desktop.dll ===
cl.exe %CFLAGS% /Fo"build\\capture_desktop.obj" src\capture_desktop.cpp || exit /b 1
rc.exe /nologo /I build /fo build\capture_desktop.res "%ROOT%\common\version.rc" || exit /b 1
link.exe %LFLAGS% /OUT:build\capture_desktop.dll build\capture_desktop.obj build\capture_desktop.res %SYSLIBS% "%ROOT%\capture\build\capture_common.lib" /IMPLIB:build\capture_desktop.lib || exit /b 1

echo.
echo === All capture DLLs built ===
dir build\*.dll
dir build\*.lib
