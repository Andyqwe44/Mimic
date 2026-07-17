# Start-MimicServer.ps1 — start Mimic signaling (node) in a dedicated window / process.
#   powershell -ExecutionPolicy Bypass -File C:\mimic\scripts\Start-MimicServer.ps1
#   powershell -ExecutionPolicy Bypass -File C:\mimic\scripts\Start-MimicServer.ps1 -Port 8443 -Restart

[CmdletBinding()]
param(
    [string]$InstallDir = 'C:\mimic\server-app',
    [string]$HostBind = '0.0.0.0',
    [int]$Port = 8443,
    [switch]$Restart,
    [switch]$Background
)

$ErrorActionPreference = 'Stop'
$serverJs = Join-Path $InstallDir 'server.js'
if (-not (Test-Path $serverJs)) {
    throw "server.js not found at $InstallDir — run Install-MimicServer.ps1 first"
}

function Get-MimicServerProcs {
    Get-CimInstance Win32_Process -Filter "Name='node.exe'" -ErrorAction SilentlyContinue |
        Where-Object { $_.CommandLine -and $_.CommandLine -match 'server\.js' -and $_.CommandLine -match [regex]::Escape($InstallDir.Replace('\', '\\')) }
}

if ($Restart) {
    Get-MimicServerProcs | ForEach-Object {
        Write-Host "    stop PID $($_.ProcessId)"
        Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue
    }
    Start-Sleep -Seconds 1
}

$existing = @(Get-MimicServerProcs)
if ($existing.Count -gt 0 -and -not $Restart) {
    Write-Host "MimicServer already running (PID $($existing[0].ProcessId)). Use -Restart to relaunch."
    try {
        $h = Invoke-RestMethod -Uri "http://127.0.0.1:$Port/health" -TimeoutSec 5
        Write-Host "    health: $($h | ConvertTo-Json -Compress)"
    } catch {
        Write-Host "    health check failed: $_"
    }
    return
}

$nodeCmd = Get-Command node -ErrorAction SilentlyContinue
if (-not $nodeCmd) { throw 'node.exe not found in PATH' }
$node = $nodeCmd.Source

$arg = "`"$serverJs`" --host $HostBind --port $Port"
Write-Host "==> Start MimicServer: node $arg"
Write-Host "    cwd=$InstallDir"

if ($Background -or $env:SESSIONNAME -eq 'RDP-Tcp' -or -not $env:SESSIONNAME) {
    # Headless / SSH-friendly: no new console window
    $p = Start-Process -FilePath $node -ArgumentList @(
        'server.js', '--host', $HostBind, '--port', "$Port"
    ) -WorkingDirectory $InstallDir -WindowStyle Hidden -PassThru
    Write-Host "    started PID $($p.Id) (background)"
} else {
    # Visible console so logs are easy to watch on interactive desktop
    Start-Process -FilePath 'powershell.exe' -ArgumentList @(
        '-NoExit', '-NoProfile', '-Command',
        "Set-Location '$InstallDir'; Write-Host 'MimicServer on ${HostBind}:$Port'; node server.js --host $HostBind --port $Port"
    )
}

Start-Sleep -Seconds 2
try {
    $h = Invoke-RestMethod -Uri "http://127.0.0.1:$Port/health" -TimeoutSec 8
    Write-Host "    OK health: $($h | ConvertTo-Json -Compress)"
    Write-Host "    Clients use: http://47.107.43.5:$Port  (demo/demo)"
} catch {
    Write-Host "    WARN: process started but health not ready yet: $_"
}
