@echo off
echo ============================================
echo  Slint Setup for Game Agent Monitor
echo ============================================
echo.

REM Check for vcpkg
where vcpkg >nul 2>&1
if %ERRORLEVEL% EQU 0 (
    echo [1] Installing Slint via vcpkg...
    vcpkg install slint:x64-windows
    if %ERRORLEVEL% EQU 0 goto :done
    echo vcpkg install failed, trying prebuilt...
)

REM Prebuilt binary download
echo [2] Downloading Slint prebuilt binary...
set SLINT_VER=1.10.0
set SLINT_URL=https://github.com/slint-ui/slint/releases/download/v%SLINT_VER%/Slint-cpp-%SLINT_VER%-win64-MSVC.exe
set SLINT_DIR=%CD%\slint_bin

if not exist "%SLINT_DIR%" mkdir "%SLINT_DIR%"

echo.
echo Download URL: %SLINT_URL%
echo.
echo This is a self-extracting installer. Please download manually:
echo   1. Open: %SLINT_URL%
echo   2. Run the .exe and extract to: %SLINT_DIR%
echo   3. Then run: cmake -B build -DCMAKE_PREFIX_PATH=%SLINT_DIR%
echo.
echo Or use vcpkg:
echo   vcpkg install slint:x64-windows
echo   cmake -B build -DCMAKE_TOOLCHAIN_FILE=C:/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake
echo.

:done
echo.
echo Setup complete! Now build:
echo   cmake -B build
echo   cmake --build build
echo   .\build\Debug\monitor.exe
