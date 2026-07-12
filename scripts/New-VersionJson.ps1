# New-VersionJson.ps1 — generate <ReleaseDir>\version.json.
#
# Replaces tools/gen_version.mjs (no more node). Walks the release dir, computes
# SHA256 + size per file, writes the manifest. Matches gen_version.mjs output:
# lowercase hex hashes (Get-FileHash is uppercase), forward-slash paths, files
# sorted, no-BOM UTF-8, and version.json itself excluded from the walk.
#
#   powershell -File scripts\New-VersionJson.ps1 -ReleaseDir release\GameAgentMonitor -Version 0.3.6 [-Full]

[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$ReleaseDir,
    [Parameter(Mandatory)][string]$Version,
    [switch]$Full,                       # mark as a FULL (non-incremental) update
    [string]$MinVersion = $Version,      # clients below this must full-install (default: conservative = this version)
    [string]$Channel    = 'stable',      # release channel (future beta/stable split)
    [switch]$Mandatory,                  # force the update (client hides "Later")
    [string]$Message    = ''             # optional user-facing note shown in the update modal
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$root = (Resolve-Path $ReleaseDir).Path

$entries = Get-ChildItem -Path $root -Recurse -File |
    Where-Object { $_.Name -ne 'version.json' } |
    ForEach-Object {
        [pscustomobject]@{
            Rel    = $_.FullName.Substring($root.Length + 1) -replace '\\', '/'
            Sha256 = (Get-FileHash -Path $_.FullName -Algorithm SHA256).Hash.ToLower()
            Size   = $_.Length
        }
    } | Sort-Object Rel

$files = [ordered]@{}
foreach ($e in $entries) {
    $files[$e.Rel] = [ordered]@{ v = $Version; sha256 = $e.Sha256; size = $e.Size }
}

$tag = "v$Version"
$manifest = [ordered]@{
    schema        = 2
    app           = $Version
    channel       = $Channel
    released      = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ss.fffZ')
    min_version   = $MinVersion
    mandatory     = [bool]$Mandatory
    message       = $Message
    full_update   = [bool]$Full
    # Server-controllable download source. The client builds each file's URL from
    # this base — move host/repo/CDN by editing a future manifest, no client rebuild.
    download_base = "https://gitee.com/Andyqwe44/tictactoe/raw/$tag/release/GameAgentMonitor/"
    updater       = [ordered]@{ path = 'bin/updater.exe' }
    sig           = ''            # reserved: Ed25519 manifest signature (verify added 0.3.8+)
    files         = $files
}

$json = $manifest | ConvertTo-Json -Depth 6
$outPath = Join-Path $root 'version.json'
# No-BOM UTF-8 (Set-Content -Encoding UTF8 adds a BOM in PS 5.1; the C++ reader
# and gen_version.mjs both use plain UTF-8).
[System.IO.File]::WriteAllText($outPath, $json, [System.Text.UTF8Encoding]::new($false))

Write-Host "version.json written ($($entries.Count) files)" -ForegroundColor Green
