@echo off
cd /d "%~dp0"
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >NUL 2>&1
if %ERRORLEVEL% NEQ 0 (echo vcvars failed & exit /b 1)
if not exist "build" mkdir "build"

echo === Building updater.exe ===
cl.exe /EHsc /std:c++17 /DNDEBUG /O2 /GS- /Gy /Gw /MT ^
  /Fo"build\\" /Fe:build\updater.exe ^
  updater.cpp ^
  advapi32.lib shell32.lib user32.lib kernel32.lib ^
  /link /OPT:REF /OPT:ICF /SUBSYSTEM:WINDOWS ^
  /MANIFEST:EMBED /MANIFESTUAC:"level='requireAdministrator' uiAccess='false'"

if %ERRORLEVEL% EQU 0 (
  echo Build OK: updater\build\updater.exe
)
