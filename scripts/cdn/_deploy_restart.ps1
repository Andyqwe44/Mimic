# Persist MimicServer via Scheduled Task (survives OpenSSH disconnect).
$ErrorActionPreference = 'Stop'
$install = 'C:\mimic\server-app'
$taskName = 'MimicServer'
$node = (Get-Command node -ErrorAction Stop).Source

Set-Location $install
if (-not (Test-Path 'node_modules\ws')) {
    Write-Host 'npm install...'
    npm install --omit=dev
}

Get-CimInstance Win32_Process -Filter "Name='node.exe'" -ErrorAction SilentlyContinue |
    Where-Object { $_.CommandLine -and $_.CommandLine -match 'server\.js' } |
    ForEach-Object {
        Write-Host "stop PID $($_.ProcessId)"
        Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue
    }
Start-Sleep -Seconds 1

$wrapper = 'C:\mimic\scripts\run_mimic_server.cmd'
@"
@echo off
set MIMIC_IS_BOOTSTRAP=1
set MIMIC_PUBLIC_URL=http://47.107.43.5:8443
cd /d "$install"
"$node" server.js --host 0.0.0.0 --port 8443
"@ | Set-Content -Path $wrapper -Encoding ASCII

Unregister-ScheduledTask -TaskName $taskName -Confirm:$false -ErrorAction SilentlyContinue
$action = New-ScheduledTaskAction -Execute $wrapper
$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -RestartCount 3 -RestartInterval (New-TimeSpan -Minutes 1) -ExecutionTimeLimit ([TimeSpan]::Zero)
$principal = New-ScheduledTaskPrincipal -UserId 'SYSTEM' -LogonType ServiceAccount -RunLevel Highest
Register-ScheduledTask -TaskName $taskName -Action $action -Settings $settings -Principal $principal -Force | Out-Null
Start-ScheduledTask -TaskName $taskName
Write-Host "scheduled task '$taskName' started"
Start-Sleep -Seconds 4
netstat -an | findstr ':8443'
try {
    $h = Invoke-RestMethod 'http://127.0.0.1:8443/health' -TimeoutSec 8
    Write-Host ('health=' + ($h | ConvertTo-Json -Compress))
} catch {
    Write-Host ('health FAIL: ' + $_.Exception.Message)
}
try {
    $c = Invoke-RestMethod 'http://127.0.0.1:8443/api/cluster' -TimeoutSec 8
    Write-Host ('cluster=' + ($c | ConvertTo-Json -Compress -Depth 5))
} catch {
    Write-Host ('cluster FAIL: ' + $_.Exception.Message)
}
