# Publish.ps1 — Gitee Release with thin MimicClient + MimicServer Setups only.
#
#   powershell -File scripts\Publish.ps1 -Version 0.3.33
#   powershell -File scripts\Publish.ps1 -Version 0.3.33 -DryRun

[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Version,
    [switch]$DryRun
)

. "$PSScriptRoot\lib\Common.ps1"

$token = '26b26b041e3a6ac124ed8dc7d7c71e84'
$repo = 'Andyqwe44/mimic'
$api = "https://gitee.com/api/v5/repos/$repo"
$tag = "v$Version"
$root = Get-RepoRoot
$mimicClient = Join-Path $root "release\MimicClient_Setup_v$Version.exe"
$mimicServer = Join-Path $root "release\MimicServer_Setup_v$Version.exe"

if (-not (Test-Path $mimicClient)) { throw "missing thin client setup: $mimicClient" }
if (-not (Test-Path $mimicServer)) { throw "missing thin server setup: $mimicServer" }

Write-Step "Publish $tag to Gitee$(if ($DryRun) { ' (dry-run)' })"
Write-Note "MimicClient: $mimicClient ($([math]::Round((Get-Item $mimicClient).Length / 1KB, 1)) KB)"
Write-Note "MimicServer: $mimicServer ($([math]::Round((Get-Item $mimicServer).Length / 1KB, 1)) KB)"

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
    Write-Note "would CREATE release $tag + upload MimicClient_Setup + MimicServer_Setup"
    Write-Host '  DRY-RUN OK (no changes made)' -ForegroundColor Green
    return
}

if ($existing) {
    Write-Note "deleting existing release id=$($existing.id)"
    Invoke-RestMethod -Uri "$api/releases/$($existing.id)?access_token=$token" -Method Delete | Out-Null
}

$body = @{
    tag_name         = $tag
    name             = $tag
    body             = @"
$tag — thin Setups (payload from CDN)

- MimicClient_Setup: installs peer client; downloads http://47.107.43.5/mimic/client/payload.zip
- MimicServer_Setup: installs signaling server; downloads http://47.107.43.5/mimic/server/payload.zip
- Incremental updates: http://47.107.43.5/mimic/client/version.json
"@
    prerelease       = $false
    target_commitish = 'main'
} | ConvertTo-Json
$release = Invoke-RestMethod -Uri "$api/releases?access_token=$token" -Method Post -ContentType 'application/json' -Body $body
if (-not $release.id) { throw "failed to create release: $($release | ConvertTo-Json -Compress)" }
Write-Note "release id=$($release.id)"

foreach ($f in @($mimicClient, $mimicServer)) {
    $upload = Send-GiteeAsset -Uri "$api/releases/$($release.id)/attach_files?access_token=$token" -FilePath $f
    Write-Host "  Uploaded $([IO.Path]::GetFileName($f))" -ForegroundColor Green
    if ($upload.browser_download_url) {
        Write-Host "  Download: $($upload.browser_download_url)" -ForegroundColor Green
    }
}
Write-Host "  Published $tag" -ForegroundColor Green
