$ErrorActionPreference = 'Continue'
Write-Host '=== node processes ==='
Get-Process node -ErrorAction SilentlyContinue | Format-Table Id, ProcessName
Write-Host '=== listen 8443 ==='
netstat -an | findstr ':8443'
Write-Host '=== restart server ==='
Get-Process node -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep 1
$install = 'C:\mimic\server-app'
$p = Start-Process -FilePath 'node' -ArgumentList @('server.js','--host','0.0.0.0','--port','8443') -WorkingDirectory $install -WindowStyle Hidden -PassThru
Write-Host "PID=$($p.Id)"
Start-Sleep 2
try {
  $h = Invoke-RestMethod 'http://127.0.0.1:8443/health'
  Write-Host ("health=" + ($h | ConvertTo-Json -Compress))
} catch {
  Write-Host ("health FAIL: " + $_.Exception.Message)
}
netstat -an | findstr ':8443'
