# Verify.ps1 — isolated package smoke test. Replaces verify_isolated.cmd.
#
# Copies the assembled release package OUTSIDE the repo and launches it, to
# reproduce exactly what a fresh install does (white-screen bugs only show there).
# Passes as soon as the log shows 'prod: frontend served' (frontend loaded, no
# white screen); fails on a WebView2 create failure or timeout.
#
#   powershell -File scripts\Verify.ps1 -Version 0.3.6              # auto (poll log)
#   powershell -File scripts\Verify.ps1 -Version 0.3.6 -Interactive # human Y/N
#
# PowerShell's Start-Sleep works under redirected stdin (a background/CI run),
# unlike cmd's `timeout` — which is why the old script needed a `ping` hack.

[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Version,
    [switch]$Interactive,
    [int]$TimeoutSec = 90
)

. "$PSScriptRoot\lib\Common.ps1"

$root = Get-RepoRoot
$rel = Join-Path $root 'release\GameAgentMonitor'
$exe = Join-Path $rel 'bin\monitor_app.exe'
if (-not (Test-Path $exe)) { throw "monitor_app.exe not found: $exe — build + assemble the release first" }

$iso = Join-Path $env:TEMP 'GAM_verify'
$logDir = Join-Path $env:LOCALAPPDATA 'GameAgentMonitor\log'

Write-Step "Isolated verify (v$Version)"
Write-Note "isolate: $iso"

function Stop-App {
    Get-Process monitor_app, msedgewebview2 -ErrorAction SilentlyContinue |
        Stop-Process -Force -ErrorAction SilentlyContinue
}

# Kill any running instance first — it holds DLL/log locks + the single-instance
# mutex (a second instance would exit 2 with no log = false "no log" failure).
Stop-App
Start-Sleep -Seconds 2

# Fresh isolated copy, far from the repo tree.
if (Test-Path $iso) { Remove-Item -Recurse -Force $iso }
New-Item -ItemType Directory -Force -Path $iso | Out-Null
Copy-Item "$rel\*" $iso -Recurse -Force

# Baseline: newest existing prod log. The poll only accepts a NEWER session file
# this launch creates — appdata\log keeps history, so a stale 'frontend served'
# from a previous run must not false-pass.
$baseline = $null
if (Test-Path $logDir) {
    $b = Get-ChildItem "$logDir\*.log" -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($b) { $baseline = $b.Name }
}

Write-Note 'launching monitor_app.exe from isolated dir...'
Start-Process -FilePath (Join-Path $iso 'bin\monitor_app.exe') -WorkingDirectory (Join-Path $iso 'bin')

$passed = $false
if ($Interactive) {
    $ans = Read-Host 'Did the app render correctly, no white screen? [y/N]'
    $passed = $ans -match '^[Yy]'
}
else {
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Seconds 3
        $latest = Get-ChildItem "$logDir\*.log" -ErrorAction SilentlyContinue |
            Sort-Object LastWriteTime -Descending | Select-Object -First 1
        if ($latest -and $latest.Name -ne $baseline) {
            $txt = Get-Content $latest.FullName -Raw -ErrorAction SilentlyContinue
            if ($txt -match 'env create failed|controller create failed') {
                Write-Warn2 'WebView2 create failure found in log'
                break
            }
            if ($txt -match 'prod: frontend served') {
                Write-Note "frontend served OK (log: $($latest.Name))"
                $passed = $true
                break
            }
        }
    }
}

Stop-App
if ($passed) {
    Write-Host '  VERIFICATION PASSED.' -ForegroundColor Green
    Remove-Item -Recurse -Force $iso -ErrorAction SilentlyContinue
    exit 0
}
else {
    Write-Host "  VERIFICATION FAILED - isolated dir kept for inspection: $iso" -ForegroundColor Red
    exit 1
}
