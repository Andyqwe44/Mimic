# Publish.ps1 — create the Gitee Release + upload the installer.
#
# Replaces publish_release.sh (Invoke-RestMethod instead of curl + bash).
# Gitee won't replace a release's assets, so an existing same-tag release is
# deleted and recreated.
#
#   powershell -File scripts\Publish.ps1 -Version 0.3.6
#   powershell -File scripts\Publish.ps1 -Version 0.3.6 -DryRun   # list only, no mutation

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
$installer = Join-Path (Get-RepoRoot) "release\GameAgentMonitor_Setup_v$Version.exe"

if (-not (Test-Path $installer)) { throw "installer not found: $installer" }
Write-Step "Publish $tag to Gitee$(if ($DryRun) { ' (dry-run)' })"
Write-Note "installer: $installer ($([math]::Round((Get-Item $installer).Length / 1MB, 1)) MB)"

# Manual multipart/form-data upload — Windows PowerShell 5.1 has no -Form, so
# build the body by hand (works on 5.1 and 7). Keeps this pure PowerShell.
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

# Find an existing release for this tag.
$releases = Invoke-RestMethod -Uri "$api/releases" -Method Get
$existing = $releases | Where-Object { $_.tag_name -eq $tag } | Select-Object -First 1

if ($DryRun) {
    if ($existing) { Write-Note "would DELETE existing release id=$($existing.id)" }
    Write-Note "would CREATE release $tag (target main) + upload installer"
    Write-Host '  DRY-RUN OK (no changes made)' -ForegroundColor Green
    return
}

if ($existing) {
    Write-Note "deleting existing release id=$($existing.id)"
    Invoke-RestMethod -Uri "$api/releases/$($existing.id)?access_token=$token" -Method Delete | Out-Null
}

$body = @{ tag_name = $tag; name = $tag; body = "$tag release"; prerelease = $false; target_commitish = 'main' } | ConvertTo-Json
$release = Invoke-RestMethod -Uri "$api/releases?access_token=$token" -Method Post -ContentType 'application/json' -Body $body
if (-not $release.id) { throw "failed to create release: $($release | ConvertTo-Json -Compress)" }
Write-Note "release id=$($release.id)"

$upload = Send-GiteeAsset -Uri "$api/releases/$($release.id)/attach_files?access_token=$token" -FilePath $installer
Write-Host "  Published $tag" -ForegroundColor Green
Write-Host "  Download: $($upload.browser_download_url)" -ForegroundColor Green
