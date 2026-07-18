# Pack MimicAndroid for Gitee Release + CDN (APK if present, else scaffold zip).
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
$zip = Join-Path $Root "release\MimicAndroid_Setup_v$Version.zip"
$apkName = if ($vj.apk) { [string]$vj.apk } else { "MimicClient_v$Version.apk" }

Write-Step "pack MimicAndroid v$Version"
if (Test-Path $outRoot) { Remove-Item $outRoot -Recurse -Force }
New-Item -ItemType Directory -Force -Path $outRoot | Out-Null

Copy-Item $vjPath $outRoot -Force
Copy-Item (Join-Path $androidRoot 'README.md') $outRoot -Force
Copy-Item (Join-Path $androidRoot 'package.json') $outRoot -Force
Copy-Item (Join-Path $androidRoot 'capacitor.config.ts') $outRoot -Force
if (Test-Path (Join-Path $androidRoot 'plugins')) {
    Copy-Item (Join-Path $androidRoot 'plugins') (Join-Path $outRoot 'plugins') -Recurse -Force
}

# Prefer a built APK from Capacitor/Gradle if available
$apkCandidates = @(
    (Join-Path $androidRoot "android\app\build\outputs\apk\debug\app-debug.apk"),
    (Join-Path $androidRoot "android\app\build\outputs\apk\release\app-release.apk"),
    (Join-Path $Root "release\$apkName"),
    (Join-Path $outRoot $apkName)
)
$apkSrc = $apkCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
$hasApk = $false
if ($apkSrc) {
    Copy-Item $apkSrc (Join-Path $outRoot $apkName) -Force
    $hasApk = $true
    Write-Ok "included APK: $apkName"
} else {
    Write-Warn2 'No APK found — shipping scaffold zip (open in Android Studio; see INSTALL.txt)'
}

$install = @"
# MimicAndroid v$Version

CDN: http://47.107.43.5/mimic/android/

$(if ($hasApk) {
@"
## Install (APK)
1. Enable Install unknown apps for your browser/file manager.
2. Download $apkName from this zip or CDN.
3. Open the APK and install.
"@
} else {
@"
## Install (scaffold — first APK not built on this machine)
1. On a machine with Android Studio + SDK:
   cd shared/web && npm i && npm run build
   cd android && npm i && npx cap add android
   Copy plugins/MimicHost into the generated app (see plugins/README.md)
   npx cap sync android && npx cap open android
2. Build signed/debug APK, rename to $apkName, upload to CDN android/.
"@
})

Bootstrap signaling: http://47.107.43.5:8443
Same account as PC (demo / demo after server first run).
"@
Set-Content -Path (Join-Path $outRoot 'INSTALL.txt') -Value $install -Encoding utf8

# Refresh version.json apk field + full_update
$vj | Add-Member -NotePropertyName 'has_apk' -NotePropertyValue $hasApk -Force
$vj.apk = $apkName
$vj.app = $Version
$utf8NoBom = New-Object System.Text.UTF8Encoding $false
[IO.File]::WriteAllText((Join-Path $outRoot 'version.json'), (($vj | ConvertTo-Json -Depth 6) + "`n"), $utf8NoBom)

if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path "$outRoot\*" -DestinationPath $zip -Force
Write-Ok $zip
Write-Output $zip
