# Install-MimicServer.ps1 — download latest MimicServer payload from CDN and install.
# Run on the Aliyun host (Administrator):
#   powershell -ExecutionPolicy Bypass -File C:\mimic\scripts\Install-MimicServer.ps1
#   powershell -ExecutionPolicy Bypass -File C:\mimic\scripts\Install-MimicServer.ps1 -Version 0.3.34

[CmdletBinding()]
param(
    [string]$Version = '',
    [string]$CdnBase = 'http://47.107.43.5/mimic',
    [string]$InstallDir = 'C:\mimic\server-app',
    [switch]$StartAfter
)

$ErrorActionPreference = 'Stop'

if (-not $Version) {
    try {
        $vj = Invoke-RestMethod -Uri "$CdnBase/client/version.json" -TimeoutSec 30
        $Version = [string]$vj.app
    } catch {
        throw "Cannot read CDN version.json: $_"
    }
}
if (-not $Version) { throw 'Version empty' }

Write-Host "==> Install MimicServer v$Version → $InstallDir"

$zipUrl = "$CdnBase/server/payload.zip"
$tmp = Join-Path $env:TEMP "MimicServer_payload_$Version.zip"
Write-Host "    GET $zipUrl"
Invoke-WebRequest -Uri $zipUrl -OutFile $tmp -UseBasicParsing

if (Test-Path $InstallDir) {
    # Keep data/ (accounts) if present
    Get-ChildItem $InstallDir -Force | Where-Object { $_.Name -ne 'data' } | Remove-Item -Recurse -Force -ErrorAction SilentlyContinue
} else {
    New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
}

Expand-Archive -LiteralPath $tmp -DestinationPath $InstallDir -Force
Remove-Item $tmp -Force -ErrorAction SilentlyContinue

if (-not (Test-Path (Join-Path $InstallDir 'server.js'))) {
    throw "Install incomplete: server.js missing in $InstallDir"
}

Push-Location $InstallDir
try {
    if (-not (Test-Path 'node_modules')) {
        Write-Host '    npm install --omit=dev'
        npm install --omit=dev
        if ($LASTEXITCODE) { throw 'npm install failed' }
    } else {
        Write-Host '    node_modules present — skip npm install'
    }
} finally { Pop-Location }

$verFile = Join-Path $InstallDir 'INSTALLED_VERSION.txt'
Set-Content -Path $verFile -Value $Version -Encoding utf8
Write-Host "    OK installed v$Version"

if ($StartAfter) {
    & "$PSScriptRoot\Start-MimicServer.ps1" -InstallDir $InstallDir -Restart -Background
}
