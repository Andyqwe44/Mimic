# New-VersionJson.ps1 — generate <ReleaseDir>\version.json (schema v2/v3).
#
# Walks the release dir, computes SHA256 + size per file, writes the manifest.
# lowercase hex hashes, forward-slash paths, files sorted, no-BOM UTF-8.
# version.json itself is excluded from the walk.
#
#   powershell -File scripts\New-VersionJson.ps1 -ReleaseDir release\MimicClient -Version 0.3.32 [-Full]
#   -Schema 2  # transitional: 0.3.31 (KNOWN_SCHEMA=2) can still incremental-update
#   -Schema 3  # signs download_base + sources; requires clients ≥0.3.32

[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$ReleaseDir,
    [Parameter(Mandatory)][string]$Version,
    [ValidateSet(2, 3)]
    [int]$Schema = 2,                    # default 2 until fleet is on ≥0.3.32; then flip Release.ps1 to 3
    [switch]$Full,                       # mark as a FULL (non-incremental) update
    [string]$MinVersion = '0.3.24',      # BASELINE: clients below this can't incrementally update (must full-install).
                                         # Bump ONLY when the update mechanism itself becomes incompatible — NOT per release.
    [string]$Channel    = 'stable',      # release channel (future beta/stable split)
    [switch]$Mandatory,                  # force the update (client hides "Later")
    [string]$Message    = '',            # optional user-facing note shown in the update modal
    [string]$JumpPad    = '0.3.31'       # migration bridge version (empty = none); clients show multi-hop UI
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

# ── Manifest signature (ECDSA P-256) ──
# Canonical digest MUST match update_verify.cpp build_canon:
#   schema ≤2: <path>\n<sha256>\n ...  (files only)
#   schema ≥3: schema=/app=/download_base=/source=* + files
$sig = ''
# CDN is the sole file shelf (binaries are NOT in git). Gitee Release only hosts thin Setups.
$downloadBase = 'http://47.107.43.5/mimic/client/'
# Discovery URLs for "latest" manifest. Clients ≥0.3.32 persist & prefer these.
$sources = [System.Collections.ArrayList]@(
    'http://47.107.43.5/mimic/client/version.json'
)
$sources.Sort([System.StringComparer]::Ordinal)

$privPath = Join-Path $PSScriptRoot '.signing\ec_priv.b64'
if (Test-Path $privPath) {
    $rels = [System.Collections.ArrayList]@($entries | ForEach-Object { $_.Rel })
    $rels.Sort([System.StringComparer]::Ordinal)
    $shaMap = @{}; foreach ($e in $entries) { $shaMap[$e.Rel] = $e.Sha256 }
    $csb = New-Object System.Text.StringBuilder
    if ($Schema -ge 3) {
        [void]$csb.Append("schema=$Schema`n")
        [void]$csb.Append("app=$Version`n")
        [void]$csb.Append("download_base=$downloadBase`n")
        foreach ($src in $sources) {
            [void]$csb.Append("source=$src`n")
        }
    }
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
    Write-Host "manifest signed (ECDSA P-256 schema=$Schema, $($rels.Count) files, $($sources.Count) sources)" -ForegroundColor DarkGray
} else {
    Write-Host "no signing key ($privPath) -> manifest unsigned (sig='')" -ForegroundColor DarkYellow
}

$manifest = [ordered]@{
    schema        = $Schema
    app           = $Version
    channel       = $Channel
    released      = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ss.fffZ')
    min_version   = $MinVersion
    mandatory     = [bool]$Mandatory
    message       = $Message
    full_update   = [bool]$Full
    # Tag-pinned file download base (immutable per release).
    download_base = $downloadBase
    # Present even on schema 2: 0.3.31 ignores; ≥0.3.32 persists for multi-source.
    sources       = @($sources)
    updater       = [ordered]@{ path = 'bin/updater.exe' }
    sig           = $sig
    files         = $files
}
# Migration bridge — clients ≥0.3.32 show 老版本→跳板→最新; older clients still show message.
if (-not [string]::IsNullOrWhiteSpace($JumpPad)) {
    $manifest['jump_pad'] = $JumpPad
}

$json = $manifest | ConvertTo-Json -Depth 6
$outPath = Join-Path $root 'version.json'
[System.IO.File]::WriteAllText($outPath, $json, [System.Text.UTF8Encoding]::new($false))

Write-Host "version.json written (schema=$Schema, $($entries.Count) files)" -ForegroundColor Green
