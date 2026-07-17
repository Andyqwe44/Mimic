# Release.ps1 — build → CDN shelf → thin Setups → verify → git (source only) → Gitee (2 exes).
#
#   powershell -File scripts\Release.ps1
#   powershell -File scripts\Release.ps1 -DryRun
#
# Binaries live on http://47.107.43.5/mimic/ — NOT in git.
# Gitee Release attaches only MimicClient_Setup + MimicServer_Setup (thin downloaders).

[CmdletBinding()]
param(
    [switch]$DryRun,
    [switch]$Interactive,
    [switch]$SkipVerify   # skip isolated Verify.ps1 (CDN-only dry runs)
)

. "$PSScriptRoot\lib\Common.ps1"

$root = Get-RepoRoot
$ver = Get-AppVersion
$cdnBase = 'http://47.107.43.5/mimic'
Write-Step "Release v$ver$(if ($DryRun) { ' (dry-run: no git, no Gitee)' })"

# 1. Frontend
Write-Step 'frontend (npm run build)'
Push-Location (Join-Path $root 'monitor_web')
try { npm run build; if ($LASTEXITCODE) { throw 'npm build failed' } } finally { Pop-Location }
Write-Ok 'dist'

# 2. Native
& "$PSScriptRoot\Build.ps1" -Module all

# 3. Assemble local release\GameAgentMonitor (CDN source; gitignored)
Write-Step 'assemble release'
$rel = Join-Path $root 'release\GameAgentMonitor'
if (Test-Path $rel) { Remove-Item -Recurse -Force $rel }
New-Item -ItemType Directory -Force -Path $rel | Out-Null
foreach ($sub in 'bin', 'frontend', 'config') {
    Copy-Item (Join-Path $root "monitor_app\build\$sub") $rel -Recurse -Force
}
Write-Ok 'release\GameAgentMonitor'

Write-Step 'normalize release EOL (LF)'
$textExt = @('.json', '.html', '.css', '.js', '.mjs', '.cjs', '.map', '.txt', '.md', '.svg', '.xml')
$nFix = 0
Get-ChildItem -Path $rel -Recurse -File | Where-Object {
    $textExt -contains $_.Extension.ToLowerInvariant()
} | ForEach-Object {
    $bytes = [IO.File]::ReadAllBytes($_.FullName)
    $hasCr = $false
    foreach ($b in $bytes) { if ($b -eq 13) { $hasCr = $true; break } }
    if ($hasCr) {
        $out = New-Object System.Collections.Generic.List[byte] ($bytes.Length)
        foreach ($b in $bytes) { if ($b -ne 13) { [void]$out.Add($b) } }
        [IO.File]::WriteAllBytes($_.FullName, $out.ToArray())
        $script:nFix++
    }
}
Write-Ok "LF-normalized $nFix file(s)"

# 4. version.json (CDN download_base / sources)
& "$PSScriptRoot\New-VersionJson.ps1" -ReleaseDir $rel -Version $ver -Schema 3 -JumpPad '0.3.31'

# 5. Pack MimicServer tree + push both shelves to Aliyun CDN (must be live before thin Setup)
Write-Step 'pack MimicServer'
& "$PSScriptRoot\Pack-MimicServer.ps1" -Version $ver
Write-Step 'publish CDN'
& "$PSScriptRoot\Publish-Cdn.ps1" -Version $ver
Write-Ok 'CDN live'

# 6. Thin installers (download payload.zip at install time)
Write-Step 'thin installers (ISCC)'
$iscc = 'C:\Program Files\Inno Setup 6\ISCC.exe'
$clientSetup = Join-Path $root "release\MimicClient_Setup_v$ver.exe"
$serverSetup = Join-Path $root "release\MimicServer_Setup_v$ver.exe"
if (-not (Test-Path $iscc)) { throw 'Inno Setup 6 not found' }
& $iscc "/DMyAppVersion=$ver" (Join-Path $root 'installer\setup.iss') | Out-Null
if ($LASTEXITCODE -or -not (Test-Path $clientSetup)) { throw 'MimicClient_Setup ISCC failed' }
Write-Ok (Split-Path $clientSetup -Leaf)
& $iscc "/DMyAppVersion=$ver" (Join-Path $root 'installer\setup_server.iss') | Out-Null
if ($LASTEXITCODE -or -not (Test-Path $serverSetup)) { throw 'MimicServer_Setup ISCC failed' }
Write-Ok (Split-Path $serverSetup -Leaf)

# 7. Isolated verify (optional)
if (-not $SkipVerify) {
    Write-Step 'isolated verify (gate)'
    $vArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "$PSScriptRoot\Verify.ps1", '-Version', $ver)
    if ($Interactive) { $vArgs += '-Interactive' }
    & powershell.exe @vArgs
    if ($LASTEXITCODE -ne 0) { throw 'isolated verification failed - nothing pushed, nothing published' }
} else {
    Write-Warn2 'SkipVerify — isolated Verify.ps1 skipped'
}

if ($DryRun) {
    Write-Host "`n==================== dry-run $ver OK (CDN + setups; not published) ====================" -ForegroundColor Green
    Write-Host "  CDN:    $cdnBase/client/version.json"
    Write-Host "  Client: $clientSetup"
    Write-Host "  Server: $serverSetup"
    return
}

# 8. Git — source only (release/ is gitignored)
Write-Step 'git commit + tag + push (source only)'
$eap = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
try {
    git add -A
    git add -u
    git commit -m "release: v$ver (CDN shelf; thin setups)" 2>&1 | Write-Host
    git tag -f "v$ver" 2>&1 | Write-Host
    git push origin main 2>&1 | Write-Host
    if ($LASTEXITCODE) { throw "git push main failed (exit $LASTEXITCODE)" }
    git push -f origin "refs/tags/v${ver}" 2>&1 | Write-Host
    if ($LASTEXITCODE) { throw "git tag push failed (exit $LASTEXITCODE)" }

    git push github main 2>&1 | Write-Host
    if ($LASTEXITCODE) {
        Write-Warn2 "git push github main failed (exit $LASTEXITCODE) — Gitee release continues"
    } else { Write-Ok 'pushed github main' }
    git push -f github "refs/tags/v${ver}" 2>&1 | Write-Host
    if ($LASTEXITCODE) {
        Write-Warn2 "git push github tag failed (exit $LASTEXITCODE) — Gitee release continues"
    } else { Write-Ok "pushed github v$ver" }
}
finally { $ErrorActionPreference = $eap }
Write-Ok "pushed origin main + v$ver"

# 9. Gitee Release — only the two thin Setups
& "$PSScriptRoot\Publish.ps1" -Version $ver

# 10. Verify CDN manifest
Write-Step 'verify CDN version.json'
try {
    $r = Invoke-RestMethod -Uri "$cdnBase/client/version.json" -Method Get -TimeoutSec 30
    if ($r.app -eq $ver) { Write-Ok "CDN 200, app=$($r.app)" } else { Write-Warn2 "CDN app mismatch: $($r.app)" }
    if ($r.download_base -notlike '*47.107.43.5*') {
        Write-Warn2 "download_base unexpected: $($r.download_base)"
    }
}
catch { Write-Warn2 "CDN URL check failed: $_" }

Write-Host "`n==================== release $ver DONE ====================" -ForegroundColor Green
Write-Host "  CDN:    $cdnBase/client/"
Write-Host "  Client: https://gitee.com/Andyqwe44/mimic/releases/download/v$ver/MimicClient_Setup_v$ver.exe"
Write-Host "  Server: https://gitee.com/Andyqwe44/mimic/releases/download/v$ver/MimicServer_Setup_v$ver.exe"
