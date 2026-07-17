# One-shot: install latest from CDN + start signaling (background).
#   powershell -ExecutionPolicy Bypass -File C:\mimic\scripts\Install-And-Start.ps1

param([string]$Version = '')
$ErrorActionPreference = 'Stop'
$dir = Split-Path -Parent $MyInvocation.MyCommand.Path
& "$dir\Install-MimicServer.ps1" -Version $Version
& "$dir\Start-MimicServer.ps1" -Restart -Background
