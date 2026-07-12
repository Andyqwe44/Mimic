<#
.SYNOPSIS
  Snapshot the INSTALLED GameAgentMonitor's runtime logs + version.json into
  tmp/installed-logs/ so Claude Code (running in the dev repo) can debug the
  real install without guessing where it lives.

.DESCRIPTION
  The installed app lives in a separate folder from this repo. Its prod logs go
  to either {install}\bin\log (0.3.5 and earlier) or
  %LOCALAPPDATA%\GameAgentMonitor\log (0.3.6+). This script:
    1. Locates the install dir (override -> registry -> Uninstall -> disk scan).
    2. Copies the newest N log files from BOTH candidate log dirs.
    3. Copies the install + appdata version.json (key evidence).
    4. Records the real exe ProductVersion (independent of version.json).
    5. Distills update-related log lines into update-lines.txt.
    6. Writes everything + an INDEX.txt under tmp/installed-logs/ (gitignored).

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File scripts\Read-InstalledLogs.ps1
  powershell ... -File scripts\Read-InstalledLogs.ps1 -InstallDir "D:\Apps\GameAgentMonitor" -Count 8
#>
[CmdletBinding()]
param(
    [int]$Count = 6,             # newest N log files per source dir
    [string]$InstallDir = '',    # override auto-locate
    [switch]$Clean               # wipe tmp/installed-logs before snapshot
)

$ErrorActionPreference = 'Stop'
function Step($m) { Write-Host "==> $m" -ForegroundColor Cyan }
function Note($m) { Write-Host "    $m" -ForegroundColor DarkGray }
function Warn2($m){ Write-Host "!!  $m" -ForegroundColor Yellow }

$repo = Split-Path $PSScriptRoot -Parent
$out  = Join-Path $repo 'tmp\installed-logs'

# ── 1. Resolve install dir ───────────────────────────────────────────────────
function Resolve-InstallDir {
    param([string]$Override)

    if ($Override) {
        if (Test-Path $Override) { Note "override: $Override"; return $Override }
        Warn2 "override not found: $Override"
    }

    # a) registry InstallPath (installer sets HKLM\SOFTWARE\GameAgentMonitor)
    foreach ($hive in @('HKLM:\SOFTWARE\GameAgentMonitor','HKLM:\SOFTWARE\WOW6432Node\GameAgentMonitor')) {
        try {
            $p = (Get-ItemProperty -Path $hive -Name InstallPath -ErrorAction Stop).InstallPath
            if ($p -and (Test-Path $p)) { Note "registry: $p"; return $p }
        } catch {}
    }

    # b) Uninstall entries -> InstallLocation
    $unRoots = @(
        'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall',
        'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall',
        'HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall'
    )
    foreach ($root in $unRoots) {
        try {
            Get-ChildItem $root -ErrorAction Stop | ForEach-Object {
                $props = Get-ItemProperty $_.PSPath -ErrorAction SilentlyContinue
                if ($props.DisplayName -match 'Game Agent Monitor' -and $props.InstallLocation -and (Test-Path $props.InstallLocation)) {
                    Note "uninstall entry: $($props.InstallLocation)"
                    return $props.InstallLocation
                }
            } | Where-Object { $_ } | Select-Object -First 1 | ForEach-Object { return $_ }
        } catch {}
    }

    # c) disk scan for monitor_app.exe sitting in a \bin\ folder
    Note "scanning disk for monitor_app.exe (registry/uninstall empty)..."
    $roots = @($env:ProgramFiles, ${env:ProgramFiles(x86)}, 'D:\', 'D:\Program Files', 'E:\') |
             Where-Object { $_ -and (Test-Path $_) } | Select-Object -Unique
    foreach ($r in $roots) {
        try {
            $hit = Get-ChildItem -Path $r -Recurse -Filter monitor_app.exe -ErrorAction SilentlyContinue -File |
                   Where-Object { $_.FullName -notmatch '\\codes\\' -and (Split-Path $_.Directory -Leaf) -eq 'bin' } |
                   Select-Object -First 1
            if ($hit) { $inst = Split-Path $hit.Directory -Parent; Note "disk scan: $inst"; return $inst }
        } catch {}
    }
    return $null
}

Step "Locating install dir"
$install = Resolve-InstallDir -Override $InstallDir
if (-not $install) {
    Warn2 "install dir not found. Pass -InstallDir <path> explicitly."
    Warn2 "(app installs to {install}\bin\monitor_app.exe; give me the folder that CONTAINS bin\)"
    exit 3
}

# ── 2. Candidate sources ─────────────────────────────────────────────────────
$appdataRoot = Join-Path $env:LOCALAPPDATA 'GameAgentMonitor'
$sources = @(
    @{ tag = 'install-bin-log'; dir = (Join-Path $install 'bin\log') },
    @{ tag = 'appdata-log';     dir = (Join-Path $appdataRoot 'log') }
)
$verFiles = @(
    @{ tag = 'install-version.json'; path = (Join-Path $install 'version.json') },
    @{ tag = 'appdata-version.json'; path = (Join-Path $appdataRoot 'version.json') }
)

# ── 3. Prepare output ────────────────────────────────────────────────────────
if ($Clean -and (Test-Path $out)) { Remove-Item $out -Recurse -Force }
New-Item -ItemType Directory -Force -Path $out | Out-Null

$index = New-Object System.Collections.Generic.List[string]
$index.Add("# Installed GameAgentMonitor log snapshot")
$index.Add("install dir : $install")

# real exe ProductVersion (independent of version.json — distinguishes H2)
$exe = Join-Path $install 'bin\monitor_app.exe'
if (Test-Path $exe) {
    $vi = (Get-Item $exe).VersionInfo
    $index.Add("exe version : $($vi.ProductVersion)  (file $($vi.FileVersion))")
} else {
    $index.Add("exe version : <monitor_app.exe not found at $exe>")
}
$index.Add("captured    : (Date.Now unavailable in this note)")
$index.Add("")

# ── 4. Copy version.json evidence ────────────────────────────────────────────
Step "Copying version.json evidence"
foreach ($v in $verFiles) {
    if (Test-Path $v.path) {
        Copy-Item $v.path (Join-Path $out $v.tag) -Force
        $app = (Select-String -Path $v.path -Pattern '"app"\s*:\s*"([^"]+)"' -List).Matches.Groups[1].Value
        $index.Add("version.json [$($v.tag)] -> app=$app")
        Note "$($v.tag): app=$app"
    } else {
        $index.Add("version.json [$($v.tag)] -> MISSING ($($v.path))")
        Note "$($v.tag): MISSING"
    }
}
$index.Add("")

# ── 5. Copy newest N logs from each source ───────────────────────────────────
Step "Copying newest $Count logs per source"
$allCopied = @()
foreach ($s in $sources) {
    if (-not (Test-Path $s.dir)) { $index.Add("log source [$($s.tag)] -> MISSING ($($s.dir))"); Note "$($s.tag): MISSING"; continue }
    $logs = Get-ChildItem -Path $s.dir -Filter '*.log' -File -ErrorAction SilentlyContinue |
            Sort-Object LastWriteTime -Descending | Select-Object -First $Count
    if (-not $logs) { $index.Add("log source [$($s.tag)] -> no *.log in $($s.dir)"); Note "$($s.tag): empty"; continue }
    $sub = Join-Path $out $s.tag
    New-Item -ItemType Directory -Force -Path $sub | Out-Null
    $index.Add("log source [$($s.tag)] -> $($s.dir)")
    foreach ($l in $logs) {
        Copy-Item $l.FullName (Join-Path $sub $l.Name) -Force
        $index.Add(("    {0}  {1}  {2} bytes" -f $l.LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss'), $l.Name, $l.Length))
        $allCopied += (Join-Path $sub $l.Name)
        Note "$($s.tag)/$($l.Name)  ($($l.Length)B)"
    }
}
$index.Add("")

# ── 6. Distill update-related lines from every copied log ─────────────────────
Step "Distilling update lines"
$distill = Join-Path $out 'update-lines.txt'
$pattern = 'check_update|download_update|update:|InstallPath|version\.json|frontend served|env create|controller'
$hits = @()
foreach ($f in $allCopied) {
    $m = Select-String -Path $f -Pattern $pattern -ErrorAction SilentlyContinue
    foreach ($h in $m) { $hits += ("[{0}] {1}" -f (Split-Path $f -Leaf), $h.Line) }
}
if ($hits.Count) {
    Set-Content -Path $distill -Value $hits -Encoding UTF8
    $index.Add("update-lines.txt : $($hits.Count) matching lines")
    Note "$($hits.Count) update-related lines -> update-lines.txt"
} else {
    Set-Content -Path $distill -Value '(no update-related lines found in captured logs)' -Encoding UTF8
    $index.Add("update-lines.txt : NONE — did you reproduce (Check Update) before running this?")
    Warn2 "no update lines. Reproduce in the app (Settings -> Check Update -> Download), THEN rerun."
}

Set-Content -Path (Join-Path $out 'INDEX.txt') -Value $index -Encoding UTF8

Step "Done"
Note "snapshot -> $out"
Note "read INDEX.txt + update-lines.txt first"
Write-Host ""
Get-Content (Join-Path $out 'INDEX.txt') | Write-Host
exit 0
