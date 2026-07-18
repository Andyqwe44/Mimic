# Build thin MimicAndroid Setup + Client (shared/web embedded).
#
#   powershell -File scripts\Build-Android.ps1
#
# Client APK embeds shared/web dist (same UI as Windows). Setup skips CDN
# download when com.mimic.client is already >= CDN version.

[CmdletBinding()]
param(
    [string]$Configuration = 'debug'
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

$ver = '0.1.1'
$aj = Join-Path $root 'android\version.json'
if (Test-Path $aj) {
    $jv = Get-Content -Raw $aj | ConvertFrom-Json
    if ($jv.app) { $ver = [string]$jv.app }
}

# ── shared/web → android assets ──
Write-Step "frontend (shared/web → assets/www) v$ver"
$web = Join-Path $root 'shared\web'
Push-Location $web
try {
    $env:VITE_APP_VERSION = $ver
    $eap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    cmd /c "npm run build"
    $ErrorActionPreference = $eap
    if ($LASTEXITCODE) { throw 'npm build failed' }
} finally { Pop-Location }

$www = Join-Path $proj 'client\src\main\assets\www'
if (Test-Path $www) { Remove-Item $www -Recurse -Force }
New-Item -ItemType Directory -Force -Path $www | Out-Null
Copy-Item (Join-Path $web 'dist\*') $www -Recurse -Force
Write-Ok "assets/www ($((Get-ChildItem $www -Recurse -File).Count) files)"

$lp = Join-Path $proj 'local.properties'
Set-Content -Path $lp -Value "sdk.dir=$($sdk -replace '\\','\\')" -Encoding ascii

$gradleBat = Get-ChildItem "$env:USERPROFILE\.gradle\wrapper\dists\gradle-8.7-bin" -Recurse -Filter 'gradle.bat' -ErrorAction SilentlyContinue |
    Select-Object -First 1 -ExpandProperty FullName
if (-not $gradleBat) { throw 'Gradle 8.7 not found under ~/.gradle/wrapper/dists' }

$taskSetup = if ($Configuration -eq 'release') { ':setup:assembleRelease' } else { ':setup:assembleDebug' }
$taskClient = if ($Configuration -eq 'release') { ':client:assembleRelease' } else { ':client:assembleDebug' }

Write-Step "gradle $Configuration"
Push-Location $proj
try {
    & $gradleBat $taskSetup $taskClient --no-daemon
    if ($LASTEXITCODE) { throw 'gradle build failed' }
} finally { Pop-Location }

$setupApk = Get-ChildItem (Join-Path $proj "setup\build\outputs\apk\$Configuration\*.apk") | Select-Object -First 1
$clientApk = Get-ChildItem (Join-Path $proj "client\build\outputs\apk\$Configuration\*.apk") | Select-Object -First 1
if (-not $setupApk -or -not $clientApk) { throw 'APK outputs missing' }

$out = Join-Path $root 'release\MimicAndroid'
New-Item -ItemType Directory -Force -Path $out | Out-Null
$setupName = "MimicAndroid_Setup_v$ver.apk"
$clientName = "MimicClient_Android_v$ver.apk"
Copy-Item $setupApk.FullName (Join-Path $out $setupName) -Force
Copy-Item $clientApk.FullName (Join-Path $out $clientName) -Force
Copy-Item $setupApk.FullName (Join-Path $root "release\$setupName") -Force

$clientApkPath = Join-Path $out $clientName
$clientSha = (Get-FileHash -Algorithm SHA256 -Path $clientApkPath).Hash.ToLowerInvariant()
$clientSize = (Get-Item $clientApkPath).Length
$manifest = [ordered]@{
    schema         = '1'
    app            = $ver
    platform       = 'android'
    channel        = 'stable'
    # Android installs one signed APK — full replace only (not PC multi-file incremental).
    full_update    = $true
    download_base  = 'http://47.107.43.5/mimic/android/'
    setup_apk      = $setupName
    client_apk     = $clientName
    apk            = $clientName
    client_sha256  = $clientSha
    client_size    = $clientSize
    has_apk        = $true
    has_client_apk = $true
    message        = 'Full APK update (Android PackageInstaller). SHA-256 verified before install.'
}
$utf8 = New-Object System.Text.UTF8Encoding $false
$jsonText = ($manifest | ConvertTo-Json -Depth 6) + "`n"
[IO.File]::WriteAllText((Join-Path $out 'version.json'), $jsonText, $utf8)
[IO.File]::WriteAllText($aj, $jsonText, $utf8)

Write-Ok $setupName
Write-Ok $clientName
Write-Note 'Publish CDN: scp release\MimicAndroid\* aliyun:C:/mimic/cdn/android/'
Write-Note "Gitee: release\$setupName"
