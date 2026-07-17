$ErrorActionPreference = 'Stop'
Set-Location 'C:\tools\nginx-1.31.1'
# Stop all nginx workers
Get-Process nginx -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 1
Start-Process -FilePath '.\nginx.exe' -WorkingDirectory 'C:\tools\nginx-1.31.1'
Start-Sleep -Seconds 1
& .\nginx.exe -t
Get-Process nginx | Format-Table Id, ProcessName
# Ensure health file
'{"ok":true,"service":"mimic-cdn"}' | Set-Content -Encoding utf8 'C:\mimic\cdn\health.json'
Write-Host 'DONE'
