# Download Slint C++ prebuilt binaries for Windows
# Places them in ./slint_bin/

$version = "1.17.0"
$url = "https://github.com/slint-ui/slint/releases/download/v$version/Slint-cpp-$version-win64-MSVC-AMD64.exe"
$installer = "$PSScriptRoot\slint_installer.exe"
$dest = "$PSScriptRoot\slint_bin"

Write-Host "========================================" -ForegroundColor Cyan
Write-Host "  Slint C++ Setup for Game Agent Monitor" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan
Write-Host ""
Write-Host "Version: $version"
Write-Host ""

# Remove old
if (Test-Path $dest) {
    Write-Host "Removing old installation..." -ForegroundColor Yellow
    Remove-Item -Recurse -Force $dest
}

# Download
Write-Host "Downloading Slint $version ..." -ForegroundColor Green
try {
    Invoke-WebRequest -Uri $url -OutFile $installer -UseBasicParsing
    Write-Host "  Downloaded: $installer" -ForegroundColor Green
} catch {
    Write-Host "  Download failed: $_" -ForegroundColor Red
    Write-Host ""
    Write-Host "Manual install:"
    Write-Host "  1. Open: $url"
    Write-Host "  2. Run the .exe, extract to: $dest"
    Write-Host "  3. Then: cmake -B build -DCMAKE_PREFIX_PATH=$dest"
    exit 1
}

# Extract
Write-Host "Extracting..." -ForegroundColor Green
New-Item -ItemType Directory -Path $dest -Force | Out-Null
Start-Process -FilePath $installer -ArgumentList "/S /D=$dest" -Wait -NoNewWindow

# Cleanup
Remove-Item $installer -Force

Write-Host ""
Write-Host "Done! Slint installed to: $dest" -ForegroundColor Green
Write-Host ""
Write-Host "Now build the monitor:" -ForegroundColor Cyan
Write-Host "  cd monitor_slint"
Write-Host "  cmake -B build -DCMAKE_PREFIX_PATH=$dest"
Write-Host "  cmake --build build"
Write-Host "  .\build\Debug\monitor.exe"
