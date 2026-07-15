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
    [string]$MinVersion = '0.3.24',      # BASELINE: clients below this can't incrementally update (must full-install).
                                         # Bump ONLY when the update mechanism itself becomes incompatible — NOT per release.
                                         # (Was $Version, which forced a full download every release — the "21 files" bug.)
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

# ── Manifest signature (ECDSA P-256, P2) ──
# Sign SHA256 of a canonical digest: for each file ORDINAL-sorted by path,
# "<path>`n<sha256>`n" concatenated. update_verify.cpp rebuilds this byte-for-byte
# (ordinal sort + LF separators + lowercase sha). Uses an independent ordinal sort
# here (NOT the culture-aware Sort-Object above) so both ends agree. Skipped
# (empty sig) when the private key is absent (dev build / key not yet generated).
$sig = ''
$privPath = Join-Path $PSScriptRoot '.signing\ec_priv.b64'
if (Test-Path $privPath) {
    $rels = [System.Collections.ArrayList]@($entries | ForEach-Object { $_.Rel })
    $rels.Sort([System.StringComparer]::Ordinal)
    $shaMap = @{}; foreach ($e in $entries) { $shaMap[$e.Rel] = $e.Sha256 }
    $csb = New-Object System.Text.StringBuilder
    foreach ($r in $rels) {
        [void]$csb.Append($r);          [void]$csb.Append("`n")
        [void]$csb.Append($shaMap[$r]); [void]$csb.Append("`n")
    }
    $payload = [System.Text.Encoding]::UTF8.GetBytes($csb.ToString())
    $hash    = [System.Security.Cryptography.SHA256]::Create().ComputeHash($payload)
    $priv = [Convert]::FromBase64String([IO.File]::ReadAllText($privPath).Trim())
    $ecp = New-Object System.Security.Cryptography.ECParameters
    $ecp.Curve = [System.Security.Cryptography.ECCurve+NamedCurves]::nistP256
    $ecp.D = [byte[]]($priv[0..31])
    $pt = New-Object System.Security.Cryptography.ECPoint
    $pt.X = [byte[]]($priv[32..63])
    $pt.Y = [byte[]]($priv[64..95])
    $ecp.Q = $pt
    $signer = [System.Security.Cryptography.ECDsa]::Create($ecp)
    $sig = [Convert]::ToBase64String($signer.SignHash($hash))   # IEEE P1363 r||s (64B)
    Write-Host "manifest signed (ECDSA P-256, $($rels.Count) files)" -ForegroundColor DarkGray
} else {
    Write-Host "no signing key ($privPath) -> manifest unsigned (sig='')" -ForegroundColor DarkYellow
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
    download_base = "https://gitee.com/Andyqwe44/mimic/raw/$tag/release/GameAgentMonitor/"
    updater       = [ordered]@{ path = 'bin/updater.exe' }
    sig           = $sig          # ECDSA P-256 signature (base64) over the files digest; '' if unsigned
    files         = $files
}

$json = $manifest | ConvertTo-Json -Depth 6
$outPath = Join-Path $root 'version.json'
# No-BOM UTF-8 (Set-Content -Encoding UTF8 adds a BOM in PS 5.1; the C++ reader
# and gen_version.mjs both use plain UTF-8).
[System.IO.File]::WriteAllText($outPath, $json, [System.Text.UTF8Encoding]::new($false))

Write-Host "version.json written ($($entries.Count) files)" -ForegroundColor Green
