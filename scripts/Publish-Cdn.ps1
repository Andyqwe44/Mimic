# Publish-Cdn.ps1 — sync local release payloads to Aliyun Mimic CDN.
#
#   powershell -File scripts\Publish-Cdn.ps1
#   powershell -File scripts\Publish-Cdn.ps1 -Version 0.3.33
#
# Expects:
#   release\GameAgentMonitor\   (client tree + version.json)
#   release\MimicServer\        (server tree from Pack-MimicServer.ps1)
# Builds payload.zip for each and scp → aliyun:C:\mimic\cdn\{client,server}\

[CmdletBinding()]
param(
    [string]$Version = '',
    [string]$SshHost = 'aliyun',
    [string]$RemoteRoot = 'C:/mimic/cdn'
)

. "$PSScriptRoot\lib\Common.ps1"
$ErrorActionPreference = 'Stop'
$root = Get-RepoRoot
if (-not $Version) { $Version = Get-AppVersion }

$clientDir = Join-Path $root 'release\GameAgentMonitor'
$serverDir = Join-Path $root 'release\MimicServer'
if (-not (Test-Path (Join-Path $clientDir 'version.json'))) {
    throw "missing $clientDir\version.json — run New-VersionJson.ps1 first"
}
if (-not (Test-Path (Join-Path $serverDir 'server.js'))) {
    Write-Step 'pack MimicServer (missing local tree)'
    & "$PSScriptRoot\Pack-MimicServer.ps1" -Version $Version
}

function New-PayloadZip([string]$SrcDir, [string]$ZipPath) {
    if (Test-Path $ZipPath) { Remove-Item $ZipPath -Force }
    # Compress contents (not the parent folder name) so Expand-Archive lands files in {app}
    $tmp = Join-Path $env:TEMP ("mimic_payload_" + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Force -Path $tmp | Out-Null
    try {
        Copy-Item "$SrcDir\*" $tmp -Recurse -Force
        Compress-Archive -Path "$tmp\*" -DestinationPath $ZipPath -Force
    } finally {
        Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
    }
}

Write-Step "CDN publish v$Version → ${SshHost}:$RemoteRoot"
$clientZip = Join-Path $clientDir 'payload.zip'
$serverZip = Join-Path $serverDir 'payload.zip'
Write-Step 'zip client payload'
New-PayloadZip $clientDir $clientZip
Write-Ok $clientZip
Write-Step 'zip server payload'
New-PayloadZip $serverDir $serverZip
Write-Ok $serverZip

# Ensure remote dirs
ssh -o BatchMode=yes $SshHost "cmd /c if not exist C:\mimic\cdn\client mkdir C:\mimic\cdn\client & if not exist C:\mimic\cdn\server mkdir C:\mimic\cdn\server"
if ($LASTEXITCODE) { throw 'ssh mkdir failed' }

Write-Step 'scp client tree'
ssh -o BatchMode=yes $SshHost "cmd /c if exist C:\mimic\cdn\client rd /s /q C:\mimic\cdn\client & mkdir C:\mimic\cdn\client"
scp -r "$clientDir\*" "${SshHost}:C:/mimic/cdn/client/"
if ($LASTEXITCODE) { throw 'scp client failed' }

Write-Step 'scp server tree'
ssh -o BatchMode=yes $SshHost "cmd /c if exist C:\mimic\cdn\server rd /s /q C:\mimic\cdn\server & mkdir C:\mimic\cdn\server"
scp -r "$serverDir\*" "${SshHost}:C:/mimic/cdn/server/"
if ($LASTEXITCODE) { throw 'scp server failed' }

Write-Step 'verify CDN HTTP'
$base = 'http://47.107.43.5/mimic'
try {
    $h = Invoke-RestMethod -Uri "$base/health.json" -Method Get -TimeoutSec 15
    Write-Ok "health: $($h | ConvertTo-Json -Compress)"
} catch {
    Write-Warn2 "health.json: $_ (ok if just deployed)"
}
$vj = Invoke-RestMethod -Uri "$base/client/version.json" -Method Get -TimeoutSec 30
if ($vj.app -ne $Version) {
    Write-Warn2 "version.json app=$($vj.app) expected $Version"
} else {
    Write-Ok "CDN version.json app=$($vj.app)"
}
Write-Host "  Client: $base/client/"
Write-Host "  Server: $base/server/"
Write-Host "  Client zip: $base/client/payload.zip"
Write-Host "  Server zip: $base/server/payload.zip"
