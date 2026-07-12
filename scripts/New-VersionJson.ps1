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
    [switch]$Full   # mark as a FULL (non-incremental) update for 0.3.5+ clients
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

$manifest = [ordered]@{
    app         = $Version
    released    = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ss.fffZ')
    full_update = [bool]$Full
    files       = $files
}

$json = $manifest | ConvertTo-Json -Depth 5
$outPath = Join-Path $root 'version.json'
# No-BOM UTF-8 (Set-Content -Encoding UTF8 adds a BOM in PS 5.1; the C++ reader
# and gen_version.mjs both use plain UTF-8).
[System.IO.File]::WriteAllText($outPath, $json, [System.Text.UTF8Encoding]::new($false))

Write-Host "version.json written ($($entries.Count) files)" -ForegroundColor Green
