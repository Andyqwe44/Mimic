# Build thin MimicAndroid Setup + Client APKs (PC-style CDN install).
#
#   powershell -File scripts\Build-Android.ps1
#
# Requires: Android SDK + JDK (Android Studio JBR recommended).
# Output: release\MimicAndroid\*.apk + version.json

[CmdletBinding()]
param(
    [string]$Configuration = 'debug'  # debug | release
)

$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\lib\Common.ps1"

$root = Get-RepoRoot
$proj = Join-Path $root 'android\setup'
$sdk = $env:ANDROID_HOME
if (-not $sdk) { $sdk = Join-Path $env:LOCALAPPDATA 'Android\Sdk' }
if (-not (Test-Path $sdk)) { throw "Android SDK not found. Set ANDROID_HOME or install Android Studio." }

$jbr = 'C:\Program Files\Android\Android Studio\jbr'
if (Test-Path $jbr) { $env:JAVA_HOME = $jbr }
$env:ANDROID_HOME = $sdk
$env:ANDROID_SDK_ROOT = $sdk

$lp = Join-Path $proj 'local.properties'
$sdkProp = ($sdk -replace '\\', '\\')
Set-Content -Path $lp -Value "sdk.dir=$($sdk -replace '\\','\\')" -Encoding ascii

$gradleBat = Get-ChildItem "$env:USERPROFILE\.gradle\wrapper\dists\gradle-8.7-bin" -Recurse -Filter 'gradle.bat' -ErrorAction SilentlyContinue |
    Select-Object -First 1 -ExpandProperty FullName
if (-not $gradleBat) { throw 'Gradle 8.7 not found under ~/.gradle/wrapper/dists — open Android Studio once or install Gradle.' }

$taskSetup = if ($Configuration -eq 'release') { ':setup:assembleRelease' } else { ':setup:assembleDebug' }
$taskClient = if ($Configuration -eq 'release') { ':client:assembleRelease' } else { ':client:assembleDebug' }

Write-Step "Build Android ($Configuration)"
Push-Location $proj
try {
    & $gradleBat $taskSetup $taskClient --no-daemon
    if ($LASTEXITCODE) { throw 'gradle build failed' }
} finally { Pop-Location }

$setupApk = Get-ChildItem (Join-Path $proj "setup\build\outputs\apk\$Configuration\*.apk") | Select-Object -First 1
$clientApk = Get-ChildItem (Join-Path $proj "client\build\outputs\apk\$Configuration\*.apk") | Select-Object -First 1
if (-not $setupApk -or -not $clientApk) { throw 'APK outputs missing' }

$ver = '0.1.0'
$aj = Join-Path $root 'android\version.json'
if (Test-Path $aj) { $ver = [string](Get-Content -Raw $aj | ConvertFrom-Json).app }

$out = Join-Path $root 'release\MimicAndroid'
New-Item -ItemType Directory -Force -Path $out | Out-Null
$setupName = "MimicAndroid_Setup_v$ver.apk"
$clientName = "MimicClient_Android_v$ver.apk"
Copy-Item $setupApk.FullName (Join-Path $out $setupName) -Force
Copy-Item $clientApk.FullName (Join-Path $out $clientName) -Force
Copy-Item $setupApk.FullName (Join-Path $root "release\$setupName") -Force

$manifest = [ordered]@{
    schema           = '1'
    app              = $ver
    platform         = 'android'
    channel          = 'stable'
    full_update      = $true
    download_base    = 'http://47.107.43.5/mimic/android/'
    setup_apk        = $setupName
    client_apk       = $clientName
    apk              = $clientName
    has_apk          = $true
    has_client_apk   = $true
    message          = 'Thin Setup APK downloads Client APK from CDN (PC-style)'
}
$utf8 = New-Object System.Text.UTF8Encoding $false
$jsonText = ($manifest | ConvertTo-Json -Depth 6) + "`n"
[IO.File]::WriteAllText((Join-Path $out 'version.json'), $jsonText, $utf8)
[IO.File]::WriteAllText($aj, $jsonText, $utf8)

Write-Ok $setupName
Write-Ok $clientName
Write-Note "CDN publish: scp release\MimicAndroid\* aliyun:C:/mimic/cdn/android/"
Write-Note "Gitee attach: release\$setupName  (thin setup — like PC Setup.exe)"
