# Publish-Cdn.ps1 — sync local release payloads to Aliyun Mimic CDN.
#
#   powershell -File scripts\Publish-Cdn.ps1                         # PC + Server + Android
#   powershell -File scripts\Publish-Cdn.ps1 -Version 0.3.35 -ClientOnly -SkipAndroid   # PC only
#   powershell -File scripts\Publish-Cdn.ps1 -AndroidOnly            # Android only
#   powershell -File scripts\Publish-Cdn.ps1 -Version 0.2.0 -ServerOnly

[CmdletBinding()]
param(
    [string]$Version = '',
    [string]$SshHost = 'aliyun',
    [string]$RemoteRoot = 'C:/mimic/cdn',
    [switch]$ClientOnly,
    [switch]$ServerOnly,
    [switch]$AndroidOnly,
    [switch]$SkipAndroid
)

if (($ClientOnly -and $ServerOnly) -or ($ClientOnly -and $AndroidOnly) -or ($ServerOnly -and $AndroidOnly)) {
    throw 'Use only one of -ClientOnly / -ServerOnly / -AndroidOnly'
}

. "$PSScriptRoot\lib\Common.ps1"
$ErrorActionPreference = 'Stop'
$root = Get-RepoRoot

# Exclusive -*Only: one track. Bare run: all three (android omitted if -SkipAndroid).
$doClient = $false
$doServer = $false
$doAndroid = $false
if ($AndroidOnly) {
    $doAndroid = $true
} elseif ($ClientOnly) {
    $doClient = $true
    # -ClientOnly = PC tree only; pass -SkipAndroid is redundant but accepted
} elseif ($ServerOnly) {
    $doServer = $true
} else {
    $doClient = $true
    $doServer = $true
    $doAndroid = -not $SkipAndroid
}

if (-not $Version) {
    if ($doClient) {
        $Version = Get-AppVersion
    } elseif ($doServer) {
        $Version = Get-ServerVersion
    } elseif ($doAndroid) {
        $aj = Join-Path $root 'android\version.json'
        $Version = if (Test-Path $aj) { [string](Get-Content -Raw $aj | ConvertFrom-Json).app } else { '0.1.0' }
    } else {
        $Version = '0.0.0'
    }
}

$clientDir = Join-Path $root 'release\MimicClient'
$serverDir = Join-Path $root 'release\MimicServer'
$androidDir = Join-Path $root 'release\MimicAndroid'

if ($doClient -and -not (Test-Path (Join-Path $clientDir 'version.json'))) {
    throw "missing $clientDir\version.json — run New-VersionJson.ps1 first"
}
if ($doServer -and -not (Test-Path (Join-Path $serverDir 'server.js'))) {
    Write-Step 'pack MimicServer (missing local tree)'
    & "$PSScriptRoot\Pack-MimicServer.ps1" -Version $(Get-ServerVersion)
}

function New-PayloadZip([string]$SrcDir, [string]$ZipPath) {
    if (Test-Path $ZipPath) { Remove-Item $ZipPath -Force }
    $tmp = Join-Path $env:TEMP ("mimic_payload_" + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Force -Path $tmp | Out-Null
    try {
        Copy-Item "$SrcDir\*" $tmp -Recurse -Force
        Compress-Archive -Path "$tmp\*" -DestinationPath $ZipPath -Force
    } finally {
        Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
    }
}

Write-Step "CDN publish → ${SshHost}:$RemoteRoot (client=$doClient server=$doServer android=$doAndroid)"

ssh -o BatchMode=yes $SshHost "cmd /c if not exist C:\mimic\cdn\client mkdir C:\mimic\cdn\client & if not exist C:\mimic\cdn\server mkdir C:\mimic\cdn\server & if not exist C:\mimic\cdn\android mkdir C:\mimic\cdn\android"
if ($LASTEXITCODE) { throw 'ssh mkdir failed' }

if ($doClient) {
    $clientZip = Join-Path $clientDir 'payload.zip'
    Write-Step 'zip client payload'
    New-PayloadZip $clientDir $clientZip
    Write-Ok $clientZip
    Write-Step 'scp client tree'
    ssh -o BatchMode=yes $SshHost "cmd /c if exist C:\mimic\cdn\client rd /s /q C:\mimic\cdn\client & mkdir C:\mimic\cdn\client"
    scp -r "$clientDir\*" "${SshHost}:C:/mimic/cdn/client/"
    if ($LASTEXITCODE) { throw 'scp client failed' }
}

if ($doServer) {
    $serverZip = Join-Path $serverDir 'payload.zip'
    Write-Step 'zip server payload'
    New-PayloadZip $serverDir $serverZip
    Write-Ok $serverZip
    Write-Step 'scp server tree'
    ssh -o BatchMode=yes $SshHost "cmd /c if exist C:\mimic\cdn\server rd /s /q C:\mimic\cdn\server & mkdir C:\mimic\cdn\server"
    scp -r "$serverDir\*" "${SshHost}:C:/mimic/cdn/server/"
    if ($LASTEXITCODE) { throw 'scp server failed' }
}

if ($doAndroid) {
    if (-not (Test-Path (Join-Path $androidDir 'version.json'))) {
        Write-Step 'pack MimicAndroid (missing local tree)'
        & "$PSScriptRoot\Pack-MimicAndroid.ps1"
    }
    Write-Step 'scp android tree'
    ssh -o BatchMode=yes $SshHost "cmd /c if exist C:\mimic\cdn\android rd /s /q C:\mimic\cdn\android & mkdir C:\mimic\cdn\android"
    scp -r "$androidDir\*" "${SshHost}:C:/mimic/cdn/android/"
    if ($LASTEXITCODE) { throw 'scp android failed' }
    $androidZip = Join-Path $root "release\MimicAndroid_Setup_v$((Get-Content -Raw (Join-Path $androidDir 'version.json') | ConvertFrom-Json).app).zip"
    if (Test-Path $androidZip) {
        scp $androidZip "${SshHost}:C:/mimic/cdn/android/"
    }
}

Write-Step 'verify CDN HTTP'
$base = 'http://47.107.43.5/mimic'
if ($doClient) {
    $vj = Invoke-RestMethod -Uri "$base/client/version.json" -Method Get -TimeoutSec 30
    Write-Ok "CDN client version.json app=$($vj.app)"
    Write-Host "  Client: $base/client/"
}
if ($doServer) {
    Write-Host "  Server: $base/server/"
}
if ($doAndroid) {
    try {
        $aj = Invoke-RestMethod -Uri "$base/android/version.json" -Method Get -TimeoutSec 30
        Write-Ok "CDN android version.json app=$($aj.app)"
    } catch { Write-Warn2 "android version.json check: $_" }
    Write-Host "  Android: $base/android/"
}
