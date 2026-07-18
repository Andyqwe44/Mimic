# Publish.ps1 — Gitee Release with thin MimicClient and/or MimicServer Setups.
#
#   powershell -File scripts\Publish.ps1 -Version 0.3.35
#   powershell -File scripts\Publish.ps1 -Version 0.3.35 -ClientOnly
#   powershell -File scripts\Publish.ps1 -Version 0.2.0 -ServerOnly -ServerVersion 0.2.0

[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Version,
    [string]$ServerVersion = '',
    [string]$AndroidVersion = '',
    [switch]$DryRun,
    [switch]$ClientOnly,
    [switch]$ServerOnly,
    [switch]$SkipAndroid
)

if ($ClientOnly -and $ServerOnly) { throw 'Use only one of -ClientOnly / -ServerOnly' }

. "$PSScriptRoot\lib\Common.ps1"

$token = $env:GITEE_TOKEN
if (-not $token) {
    $tokFile = Join-Path $PSScriptRoot '.signing\gitee_token.txt'
    if (Test-Path $tokFile) { $token = (Get-Content -Raw $tokFile).Trim() }
}
if (-not $token) {
    throw 'Gitee token missing. Set $env:GITEE_TOKEN or write scripts\.signing\gitee_token.txt'
}
$repo = 'Andyqwe44/mimic'
$api = "https://gitee.com/api/v5/repos/$repo"
$doClient = -not $ServerOnly
$doServer = -not $ClientOnly
$doAndroid = -not $SkipAndroid -and $doClient  # ship Android Setup APK on client-tagged releases
if (-not $ServerVersion) { $ServerVersion = if ($ServerOnly) { $Version } else { Get-ServerVersion } }
if (-not $AndroidVersion) {
    $aj = Join-Path (Get-RepoRoot) 'android\version.json'
    if (Test-Path $aj) { $AndroidVersion = [string](Get-Content -Raw $aj | ConvertFrom-Json).app }
    if (-not $AndroidVersion) { $AndroidVersion = '0.1.0' }
}

$tag = if ($doClient) { "v$Version" } else { "server-v$ServerVersion" }
$root = Get-RepoRoot
$mimicClient = Join-Path $root "release\MimicClient_Setup_v$Version.exe"
$mimicServer = Join-Path $root "release\MimicServer_Setup_v$ServerVersion.exe"
# Phone install: attach Setup APK directly (no zip — unzipping on mobile is painful).
$mimicAndroid = Join-Path $root "release\MimicAndroid_Setup_v$AndroidVersion.apk"

$assets = @()
if ($doClient) {
    if (-not (Test-Path $mimicClient)) { throw "missing thin client setup: $mimicClient" }
    $assets += $mimicClient
}
if ($doServer) {
    if (-not (Test-Path $mimicServer)) { throw "missing thin server setup: $mimicServer" }
    $assets += $mimicServer
}
if ($doAndroid) {
    if (-not (Test-Path $mimicAndroid)) {
        & "$PSScriptRoot\Pack-MimicAndroid.ps1" -Version $AndroidVersion
    }
    if (-not (Test-Path $mimicAndroid)) { throw "missing android setup apk: $mimicAndroid" }
    $assets += $mimicAndroid
}

Write-Step "Publish $tag to Gitee$(if ($DryRun) { ' (dry-run)' })"
foreach ($a in $assets) {
    Write-Note "$([IO.Path]::GetFileName($a)) ($([math]::Round((Get-Item $a).Length / 1KB, 1)) KB)"
}

function Send-GiteeAsset {
    param([string]$Uri, [string]$FilePath)
    $boundary = [Guid]::NewGuid().ToString()
    $name = [IO.Path]::GetFileName($FilePath)
    $LF = "`r`n"
    $header = "--$boundary$LF" +
        "Content-Disposition: form-data; name=`"file`"; filename=`"$name`"$LF" +
        "Content-Type: application/octet-stream$LF$LF"
    $footer = "$LF--$boundary--$LF"
    $ms = New-Object System.IO.MemoryStream
    $hb = [Text.Encoding]::UTF8.GetBytes($header)
    $fb = [IO.File]::ReadAllBytes($FilePath)
    $tb = [Text.Encoding]::UTF8.GetBytes($footer)
    $ms.Write($hb, 0, $hb.Length); $ms.Write($fb, 0, $fb.Length); $ms.Write($tb, 0, $tb.Length)
    Invoke-RestMethod -Uri $Uri -Method Post -Body $ms.ToArray() `
        -ContentType "multipart/form-data; boundary=$boundary"
}

$releases = Invoke-RestMethod -Uri "$api/releases" -Method Get
$existing = $releases | Where-Object { $_.tag_name -eq $tag } | Select-Object -First 1

if ($DryRun) {
    if ($existing) { Write-Note "would DELETE existing release id=$($existing.id)" }
    Write-Note "would CREATE release $tag + upload $(($assets | ForEach-Object { [IO.Path]::GetFileName($_) }) -join ', ')"
    Write-Host '  DRY-RUN OK (no changes made)' -ForegroundColor Green
    return
}

if ($existing) {
    Write-Note "deleting existing release id=$($existing.id)"
    Invoke-RestMethod -Uri "$api/releases/$($existing.id)?access_token=$token" -Method Delete | Out-Null
}

$bodyText = @"
$tag — installers (payload from CDN)

$(if ($doClient) { "- MimicClient_Setup (PC): http://47.107.43.5/mimic/client/`n- Incremental: http://47.107.43.5/mimic/client/version.json`n" })
$(if ($doServer) { "- MimicServer_Setup: http://47.107.43.5/mimic/server/ (signaling; auto-joins Bootstrap)`n" })
$(if ($doAndroid) { "- MimicAndroid_Setup.apk: install on phone → downloads Client from CDN`n- CDN: http://47.107.43.5/mimic/android/`n" })
"@

$body = @{
    tag_name         = $tag
    name             = $tag
    body             = $bodyText
    prerelease       = $false
    target_commitish = 'main'
} | ConvertTo-Json
$release = Invoke-RestMethod -Uri "$api/releases?access_token=$token" -Method Post -ContentType 'application/json' -Body $body
if (-not $release.id) { throw "failed to create release: $($release | ConvertTo-Json -Compress)" }
Write-Note "release id=$($release.id)"

foreach ($f in $assets) {
    $upload = Send-GiteeAsset -Uri "$api/releases/$($release.id)/attach_files?access_token=$token" -FilePath $f
    Write-Host "  Uploaded $([IO.Path]::GetFileName($f))" -ForegroundColor Green
    if ($upload.browser_download_url) {
        Write-Host "  Download: $($upload.browser_download_url)" -ForegroundColor Green
    }
}
Write-Host "  Published $tag" -ForegroundColor Green
