# Pack MimicServer for Gitee Release (signaling only â€?no media).
param(
    [string]$Version = ''
)
$ErrorActionPreference = 'Stop'
$Root = Split-Path $PSScriptRoot -Parent
. "$PSScriptRoot\lib\Common.ps1"
if (-not $Version) { $Version = Get-ServerVersion }

$src = Join-Path $Root 'server'
$outRoot = Join-Path $Root "release\MimicServer"
$zip = Join-Path $Root "release\MimicServer_v$Version.zip"

if (-not (Test-Path (Join-Path $src 'server.js'))) { throw 'server/server.js missing' }

Push-Location $src
try {
    if (-not (Test-Path 'node_modules')) { npm install --omit=dev }
} finally { Pop-Location }

if (Test-Path $outRoot) { Remove-Item $outRoot -Recurse -Force }
New-Item -ItemType Directory -Force -Path $outRoot | Out-Null
Copy-Item "$src\server.js" $outRoot
Copy-Item "$src\package.json" $outRoot
Copy-Item "$src\README.md" $outRoot
if (Test-Path "$src\node_modules") {
    Copy-Item "$src\node_modules" "$outRoot\node_modules" -Recurse -Force
}

@"
# MimicServer v$Version

## Install (Linux / Windows)

``````bash
unzip MimicServer_v$Version.zip -d /opt/mimic-server
cd /opt/mimic-server
node server.js --host 0.0.0.0 --port 8443
``````

Put nginx/caddy TLS in front if needed. Clients default to Bootstrap
http://47.107.43.5:8443 â€?every MimicServer auto-joins the mesh on start.

Default account after first run: demo / demo (change in production).

Media never flows through this process.
"@ | Set-Content -Encoding utf8 (Join-Path $outRoot 'INSTALL.txt')

if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path "$outRoot\*" -DestinationPath $zip -Force
Write-Host "OK $zip"
