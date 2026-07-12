# Release.ps1 — one-command release pipeline (all PowerShell).
#
# Replaces release.sh + build_release.cmd. The version is read from version.h
# (铁律 8, single source), so bumping that file is the only manual edit.
#
#   powershell -File scripts\Release.ps1            # full: build -> verify -> push -> Gitee
#   powershell -File scripts\Release.ps1 -DryRun    # build -> verify only (no git, no publish)
#
# Chain: frontend(npm) -> native build(Build.ps1) -> assemble release\ ->
#        version.json -> installer(ISCC) -> isolated verify -> git push -> Gitee -> raw check.

[CmdletBinding()]
param(
    [switch]$DryRun,        # skip git push + Gitee publish (build + verify only)
    [switch]$Interactive    # human Y/N at the verify gate instead of log polling
)

. "$PSScriptRoot\lib\Common.ps1"

$root = Get-RepoRoot
$ver = Get-AppVersion
Write-Step "Release v$ver$(if ($DryRun) { ' (dry-run: no git, no publish)' })"

# 1. Frontend — before the native build; monitor_app stages dist into build\frontend.
Write-Step 'frontend (npm run build)'
Push-Location (Join-Path $root 'monitor_web')
try { npm run build; if ($LASTEXITCODE) { throw 'npm build failed' } } finally { Pop-Location }
Write-Ok 'dist'

# 2. Native modules — one VS Dev Shell builds libs + app and stages
#    monitor_app\build\{bin,frontend,config} (the package layout). Throws on error.
& "$PSScriptRoot\Build.ps1" -Module all

# 3. Assemble release\GameAgentMonitor from monitor_app\build\{bin,frontend,config}.
#    Copy only those three subdirs — NOT build\ root, which also holds loose
#    compile artifacts (app.res + *.obj) that must not ship in the package.
Write-Step 'assemble release'
$rel = Join-Path $root 'release\GameAgentMonitor'
if (Test-Path $rel) { Remove-Item -Recurse -Force $rel }
New-Item -ItemType Directory -Force -Path $rel | Out-Null
foreach ($sub in 'bin', 'frontend', 'config') {
    Copy-Item (Join-Path $root "monitor_app\build\$sub") $rel -Recurse -Force
}
Write-Ok 'release\GameAgentMonitor'

# 4. version.json (SHA256 manifest).
& "$PSScriptRoot\New-VersionJson.ps1" -ReleaseDir $rel -Version $ver

# 5. Installer (Inno Setup).
Write-Step 'installer (ISCC)'
$iscc = 'C:\Program Files\Inno Setup 6\ISCC.exe'
if (Test-Path $iscc) {
    & $iscc "/DMyAppVersion=$ver" (Join-Path $root 'installer\setup.iss') | Out-Null
    if ($LASTEXITCODE) { Write-Warn2 'ISCC failed' } else { Write-Ok "GameAgentMonitor_Setup_v$ver.exe" }
}
else { Write-Warn2 'Inno Setup 6 not found — installer skipped' }

# 6. Isolated verify gate — run as a CHILD process so its `exit` code is isolated
#    (a bare `exit` in the same runspace would kill this script).
Write-Step 'isolated verify (gate)'
$vArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "$PSScriptRoot\Verify.ps1", '-Version', $ver)
if ($Interactive) { $vArgs += '-Interactive' }
& powershell.exe @vArgs
if ($LASTEXITCODE -ne 0) { throw 'isolated verification failed — nothing pushed, nothing published' }

if ($DryRun) {
    Write-Host "`n==================== dry-run $ver OK (built + verified; not published) ====================" -ForegroundColor Green
    return
}

# 7. Git commit + tag + push. Native git writes progress/banners to stderr; under
#    $ErrorActionPreference='Stop' that becomes a terminating RemoteException, so
#    relax it here and check $LASTEXITCODE explicitly on the pushes that matter.
#    `git add -A` folds source + docs into the commit (the release binaries are
#    already tracked); `-f` force-adds the release payload past *.dll/*.exe ignores.
Write-Step 'git commit + tag + push'
$eap = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
try {
    git add -A
    git add -f release\GameAgentMonitor monitor_app\src\version.h installer\setup.iss
    git commit -m "release: v$ver" 2>&1 | Write-Host   # no-op if nothing staged; fine
    git tag -f "v$ver" 2>&1 | Write-Host               # (re)create local tag
    git push origin main 2>&1 | Write-Host
    if ($LASTEXITCODE) { throw "git push main failed (exit $LASTEXITCODE)" }
    git push -f origin "refs/tags/v${ver}" 2>&1 | Write-Host   # force: overwrite a stale remote tag
    if ($LASTEXITCODE) { throw "git tag push failed (exit $LASTEXITCODE)" }
}
finally { $ErrorActionPreference = $eap }
Write-Ok "pushed main + v$ver"

# 8. Publish the Gitee Release + installer.
& "$PSScriptRoot\Publish.ps1" -Version $ver

# 9. Verify the raw version.json URL resolves (302 -> 200) with the right version.
Write-Step 'verify raw version.json'
$u = "https://gitee.com/Andyqwe44/tictactoe/raw/v$ver/release/GameAgentMonitor/version.json"
try {
    $r = Invoke-RestMethod -Uri $u -Method Get
    if ($r.app -eq $ver) { Write-Ok "raw 200, app=$($r.app)" } else { Write-Warn2 "raw app mismatch: $($r.app)" }
}
catch { Write-Warn2 "raw URL check failed: $_" }

Write-Host "`n==================== release $ver DONE ====================" -ForegroundColor Green
Write-Host "  https://gitee.com/Andyqwe44/tictactoe/releases/download/v$ver/GameAgentMonitor_Setup_v$ver.exe"
