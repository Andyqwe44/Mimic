# Dev.ps1 — one-click dev environment: Vite (:1420) + monitor_app (Dev build).
#
#   powershell -File scripts\Dev.ps1     # or double-click dev.bat at repo root
#
# Incremental: compares source mtimes against build artifacts per module.
#   - No C++ change            → skip the compiler entirely, just launch.
#   - A lib changed            → rebuild only that module (+ monitor_app, which
#                                re-copies the DLLs into build_dev\bin).
#   - monitor_app src changed  → rebuild monitor_app only.
# Vite is started first (async) so npm startup overlaps compilation; if :1420
# is already listening (previous run's window), it is reused as-is.
#
# Note: a logger-only change does NOT relink capture/input — they bind via
# import lib, so a fresh logger.dll swap is enough unless its exported API
# changed (then touch a file in capture/input or delete their build dirs).

. "$PSScriptRoot\lib\Common.ps1"

$Root = Get-RepoRoot

# ── helpers ──────────────────────────────────────────────────────────────────

# Newest LastWriteTimeUtc across the given repo-relative files/dirs
# (build/build_dev output dirs excluded so artifacts never count as sources).
function Get-NewestSourceTime {
    param([string[]]$Paths)
    $newest = [datetime]::MinValue
    foreach ($p in $Paths) {
        $full = Join-Path $Root $p
        if (-not (Test-Path $full)) { continue }
        $items = if ((Get-Item $full).PSIsContainer) {
            Get-ChildItem $full -Recurse -File |
                Where-Object { $_.FullName -notmatch '\\build(_dev)?\\' }
        }
        else { Get-Item $full }
        foreach ($f in $items) {
            if ($f.LastWriteTimeUtc -gt $newest) { $newest = $f.LastWriteTimeUtc }
        }
    }
    $newest
}

# Stale = any artifact missing, or newest source newer than oldest artifact.
function Test-Stale {
    param([string[]]$Sources, [string[]]$Artifacts)
    $times = @()
    foreach ($a in $Artifacts) {
        $full = Join-Path $Root $a
        if (-not (Test-Path $full)) { return $true }
        $times += (Get-Item $full).LastWriteTimeUtc
    }
    $oldestArtifact = ($times | Sort-Object)[0]
    (Get-NewestSourceTime $Sources) -gt $oldestArtifact
}

# Connect via hostname: the TcpClient(host, port) ctor tries every resolved
# address, so this works whether Vite bound 127.0.0.1 (IPv4) or ::1 (IPv6).
function Test-VitePort {
    try { (New-Object Net.Sockets.TcpClient('localhost', 1420)).Close(); $true } catch { $false }
}

# ── 1. decide what needs rebuilding (mtime diff per module) ─────────────────

$captureDlls = @('common', 'wgc', 'gdi', 'pw', 'screen', 'desktop') |
    ForEach-Object { "capture\build\capture_$_.dll" }
$inputDlls = @('common', 'sendinput', 'winapi', 'postmessage', 'driver') |
    ForEach-Object { "input\build\input_$_.dll" }

$mods = @()
if (Test-Stale @('logger', 'common') @('logger\build\logger.dll', 'logger\build\logger.lib')) { $mods += 'logger' }
if (Test-Stale @('capture\src', 'capture\include', 'common') $captureDlls) { $mods += 'capture' }
if (Test-Stale @('input\src', 'input\include', 'common') $inputDlls) { $mods += 'input' }

# monitor_app: stale on its own sources, or whenever any lib rebuilt (Build-
# MonitorApp wipes build_dev and re-copies all 12 DLLs next to the exe).
$exeRel = 'monitor_app\build_dev\bin\monitor_app.exe'
if ($mods.Count -gt 0 -or
    (Test-Stale @('monitor_app\src', 'monitor_app\app.rc', 'monitor_app\dep', 'common') @($exeRel))) {
    $mods += 'monitor_app'
}

# ── 2. start Vite (async — overlaps the compile) ────────────────────────────

$viteWasUp = Test-VitePort
if ($viteWasUp) {
    Write-Ok 'Vite already listening on :1420 — reusing'
}
else {
    Write-Step 'starting Vite (npm run dev) in its own window'
    Start-Process cmd -ArgumentList '/k', 'npm run dev' `
        -WorkingDirectory (Join-Path $Root 'monitor_web')
}

# ── 3. build only what changed ───────────────────────────────────────────────

if ($mods.Count -gt 0) {
    Write-Step "rebuild needed: $($mods -join ', ')"
    & "$PSScriptRoot\Build.ps1" -Module $mods -Dev
}
else {
    Write-Ok 'C++ unchanged — build skipped'
}

# ── 4. wait for Vite, then launch the Dev exe ───────────────────────────────

if (-not $viteWasUp) {
    Write-Step 'waiting for Vite on :1420'
    $deadline = (Get-Date).AddSeconds(90)
    while (-not (Test-VitePort)) {
        if ((Get-Date) -gt $deadline) { throw 'Vite did not come up on :1420 within 90s' }
        Start-Sleep -Milliseconds 500
    }
    Write-Ok 'Vite up'
}

$exe = Join-Path $Root $exeRel
Write-Step 'launching monitor_app (Dev)'
Start-Process $exe -WorkingDirectory (Split-Path $exe)
Write-Ok 'dev environment ready'
