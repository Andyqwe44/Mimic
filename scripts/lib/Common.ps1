# Common.ps1 — shared helpers for the PowerShell build/release pipeline.
#
# Dot-source from Build.ps1 / Verify.ps1 / Publish.ps1 / Release.ps1:
#     . "$PSScriptRoot\lib\Common.ps1"
#
# This replaces the old cmd/bash/node mix (vcvars.bat + build_*.cmd + release.sh
# + publish_release.sh + gen_version.mjs). One language, no cross-type calls.

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# Repo root = two levels up from this file (scripts/lib/Common.ps1).
function Get-RepoRoot {
    (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
}

# ── Logging (replaces the cmd `echo [n/m] ...` lines) ──
function Write-Step { param([string]$Message) Write-Host "==> $Message" -ForegroundColor Cyan }
function Write-Ok   { param([string]$Message = '') Write-Host "    OK $Message"   -ForegroundColor Green }
function Write-Note { param([string]$Message) Write-Host "    $Message" -ForegroundColor DarkGray }
function Write-Warn2{ param([string]$Message) Write-Host "    WARN: $Message" -ForegroundColor Yellow }

# Single source of truth (铁律 8): parse APP_VERSION from version.h.
function Get-AppVersion {
    $vh = Join-Path (Get-RepoRoot) 'monitor_app\src\version.h'
    $line = Select-String -Path $vh -Pattern '#define\s+APP_VERSION\s+"([^"]+)"' -List
    if (-not $line) { throw "APP_VERSION not found in $vh" }
    $line.Matches[0].Groups[1].Value
}

# Enter the MSVC x64 build environment in THIS PowerShell session — replaces
# `call vcvars64.bat`. Idempotent: no-op if cl.exe is already on PATH. Uses the
# VS-provided DevShell module so we never shell out to a .bat.
# Override the VS location with $env:GAM_VS_PATH if it differs.
function Enter-BuildShell {
    if (Get-Command cl.exe -ErrorAction SilentlyContinue) { return }
    $vsPath = $env:GAM_VS_PATH
    if (-not $vsPath) { $vsPath = 'C:\Program Files\Microsoft Visual Studio\18\Community' }
    $devShell = Join-Path $vsPath 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll'
    if (-not (Test-Path $devShell)) {
        throw "VS DevShell module not found: $devShell  (set `$env:GAM_VS_PATH)"
    }
    Import-Module $devShell
    Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation `
        -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
    if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
        throw 'Enter-BuildShell: cl.exe still not on PATH after Enter-VsDevShell'
    }
}

# Generate a module's version resource header — replaces the cmd
# `echo #define ... >> build\_ver_module.h` blocks. rc.exe includes this to stamp
# VERSIONINFO without quoting hell. $FileType: VFT_DLL for libs/DLLs, VFT_APP for exes.
# $Version here is the MODULE's own version (see $LibVer in Build.ps1), decoupled
# from APP_VERSION so a bump of the app doesn't churn every DLL's bytes. No
# APP_VERSION #define is emitted — DLL code must not embed the app version.
function New-VerModuleHeader {
    param(
        [Parameter(Mandatory)][string]$OutPath,
        [Parameter(Mandatory)][string]$Version,
        [Parameter(Mandatory)][string]$ModuleDesc,
        [ValidateSet('VFT_DLL', 'VFT_APP')][string]$FileType = 'VFT_DLL'
    )
    $comma = ($Version -replace '\.', ',') + ',0'
    $lines = @(
        "#define GAM_RC_COMMA $comma"
        "#define GAM_RC_STR `"$Version`""
        "#define GAM_MODULE_DESC `"$ModuleDesc`""
        "#define GAM_FILETYPE $FileType"
    )
    $dir = Split-Path $OutPath -Parent
    if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }
    Set-Content -Path $OutPath -Value $lines -Encoding Ascii
}

# Run a native command and throw if it exits non-zero (replaces the cmd
# `|| exit /b 1` / `if %ERRORLEVEL% NEQ 0` boilerplate).
function Invoke-Native {
    param([Parameter(Mandatory)][scriptblock]$Command, [string]$What = 'command')
    & $Command
    if ($LASTEXITCODE -ne 0) { throw "$What failed (exit $LASTEXITCODE)" }
}
