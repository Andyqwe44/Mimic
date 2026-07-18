# Pack MimicAndroid thin Setup for Gitee (+ stage CDN tree).
# Prefer Build-Android.ps1 first so APKs exist under release\MimicAndroid\.
param(
    [string]$Version = ''
)
$ErrorActionPreference = 'Stop'
$Root = Split-Path $PSScriptRoot -Parent
. "$PSScriptRoot\lib\Common.ps1"

$androidRoot = Join-Path $Root 'android'
$vjPath = Join-Path $androidRoot 'version.json'
if (-not (Test-Path $vjPath)) { throw 'android/version.json missing' }
$vj = Get-Content -Raw $vjPath | ConvertFrom-Json
if (-not $Version) { $Version = [string]$vj.app }
if (-not $Version) { throw 'android version missing' }

$outRoot = Join-Path $Root 'release\MimicAndroid'
$setupName = if ($vj.setup_apk) { [string]$vj.setup_apk } else { "MimicAndroid_Setup_v$Version.apk" }
$clientName = if ($vj.client_apk) { [string]$vj.client_apk } else { "MimicClient_Android_v$Version.apk" }
$zip = Join-Path $Root "release\MimicAndroid_Setup_v$Version.zip"
$setupApkOut = Join-Path $Root "release\$setupName"

Write-Step "pack MimicAndroid v$Version"
New-Item -ItemType Directory -Force -Path $outRoot | Out-Null

$setupSrc = @(
    (Join-Path $outRoot $setupName),
    (Join-Path $Root "android\setup\setup\build\outputs\apk\debug\setup-debug.apk"),
    (Join-Path $Root "android\setup\setup\build\outputs\apk\release\*.apk")
) | ForEach-Object { Get-Item $_ -ErrorAction SilentlyContinue } | Select-Object -First 1

$clientSrc = @(
    (Join-Path $outRoot $clientName),
    (Join-Path $Root "android\setup\client\build\outputs\apk\debug\client-debug.apk"),
    (Join-Path $Root "android\setup\client\build\outputs\apk\release\*.apk")
) | ForEach-Object { Get-Item $_ -ErrorAction SilentlyContinue } | Select-Object -First 1

if (-not $setupSrc -or -not $clientSrc) {
    Write-Warn2 'APKs missing — run: powershell -File scripts\Build-Android.ps1'
    throw 'MimicAndroid APKs not built'
}

$setupDest = Join-Path $outRoot $setupName
$clientDest = Join-Path $outRoot $clientName
if ($setupSrc.FullName -ne $setupDest) {
    Copy-Item $setupSrc.FullName $setupDest -Force
}
if ($clientSrc.FullName -ne $clientDest) {
    Copy-Item $clientSrc.FullName $clientDest -Force
}
if ($setupSrc.FullName -ne $setupApkOut) {
    Copy-Item $setupSrc.FullName $setupApkOut -Force
}

$clientApkPath = Join-Path $outRoot $clientName
$clientSha = ''
$clientSize = 0
if (Test-Path $clientApkPath) {
    $clientSha = (Get-FileHash -Algorithm SHA256 -Path $clientApkPath).Hash.ToLowerInvariant()
    $clientSize = (Get-Item $clientApkPath).Length
}
$manifest = [ordered]@{
    schema         = '1'
    app            = $Version
    platform       = 'android'
    channel        = 'stable'
    # Full APK replace only — Android has no PC-style multi-file overlay updater.
    full_update    = $true
    download_base  = 'http://47.107.43.5/mimic/android/'
    setup_apk      = $setupName
    client_apk     = $clientName
    apk            = $clientName
    client_sha256  = $clientSha
    client_size    = $clientSize
    has_apk        = $true
    has_client_apk = $true
    message        = 'Full APK update (PackageInstaller). SHA-256 verified before install.'
}
$utf8 = New-Object System.Text.UTF8Encoding $false
$jsonText = ($manifest | ConvertTo-Json -Depth 6) + "`n"
[IO.File]::WriteAllText((Join-Path $outRoot 'version.json'), $jsonText, $utf8)
[IO.File]::WriteAllText($vjPath, $jsonText, $utf8)

$install = @"
# MimicAndroid thin Setup v$Version

Same model as PC MimicClient_Setup.exe:

1. Install $setupName on the phone.
2. Open Mimic Setup — downloads $clientName from CDN.
3. System installer installs Mimic Client.

CDN: http://47.107.43.5/mimic/android/
"@
Set-Content -Path (Join-Path $outRoot 'INSTALL.txt') -Value $install -Encoding utf8

# Gitee zip = Setup APK only (thin); Client stays on CDN
$zipDir = Join-Path $env:TEMP ("mimic_android_setup_" + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $zipDir | Out-Null
try {
    Copy-Item (Join-Path $outRoot $setupName) $zipDir
    Copy-Item (Join-Path $outRoot 'INSTALL.txt') $zipDir
    Copy-Item (Join-Path $outRoot 'version.json') $zipDir
    if (Test-Path $zip) { Remove-Item $zip -Force }
    Compress-Archive -Path "$zipDir\*" -DestinationPath $zip -Force
} finally {
    Remove-Item $zipDir -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Ok $setupApkOut
Write-Ok $zip
Write-Ok "CDN client: $clientName (upload release\MimicAndroid\* to cdn/android/)"
