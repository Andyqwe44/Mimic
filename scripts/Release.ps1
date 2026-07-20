# Release.ps1 — build → CDN shelf → thin Setups (local) → verify → git.
# Gitee Setup upload is OFF by default (old Setup + in-app update is enough).
# Opt in with -PublishGitee when a new thin installer must be attached on Gitee.
#
#   powershell -File scripts\Release.ps1                 # PC + Server + Android
#   powershell -File scripts\Release.ps1 -ClientOnly     # PC only (no Android)
#   powershell -File scripts\Release.ps1 -AndroidOnly    # Android only (no C++)
#   powershell -File scripts\Release.ps1 -ServerOnly     # Server only
#   powershell -File scripts\Release.ps1 -DryRun
#   powershell -File scripts\Release.ps1 -PublishGitee
#
# Binaries live on http://47.107.43.5/mimic/ — NOT in git.

[CmdletBinding()]
param(
    [switch]$DryRun,
    [switch]$Interactive,
    [switch]$SkipVerify,
    [switch]$ClientOnly,
    [switch]$AndroidOnly,
    [switch]$ServerOnly,
    [switch]$PublishGitee
)

$onlyFlags = @($ClientOnly, $AndroidOnly, $ServerOnly) | Where-Object { $_ }
if ($onlyFlags.Count -gt 1) {
    throw 'Use only one of -ClientOnly / -AndroidOnly / -ServerOnly (or neither for all three)'
}

. "$PSScriptRoot\lib\Common.ps1"

$root = Get-RepoRoot
$ver = Get-AppVersion
$serverVer = Get-ServerVersion
$cdnBase = 'http://47.107.43.5/mimic'

# Three independent product tracks. Exclusive -*Only flags; bare run = all.
$doPc = -not $AndroidOnly -and -not $ServerOnly
$doAndroid = -not $ClientOnly -and -not $ServerOnly
$doServer = -not $ClientOnly -and -not $AndroidOnly
if ($ClientOnly) { $doPc = $true; $doAndroid = $false; $doServer = $false }
if ($AndroidOnly) { $doPc = $false; $doAndroid = $true; $doServer = $false }
if ($ServerOnly) { $doPc = $false; $doAndroid = $false; $doServer = $true }

$androidVer = '0.1.0'
$ajPath = Join-Path $root 'android\version.json'
if (Test-Path $ajPath) {
    $androidVer = [string](Get-Content -Raw $ajPath | ConvertFrom-Json).app
}

Write-Step ("Release pc={0} android={1} server={2}{3}" -f `
    "$(if ($doPc) { "True(v$ver)" } else { 'False' })", `
    "$(if ($doAndroid) { "True(v$androidVer)" } else { 'False' })", `
    "$(if ($doServer) { "True(v$serverVer)" } else { 'False' })", `
    "$(if ($DryRun) { ' (dry-run)' } else { '' })")

$clientSetup = Join-Path $root "release\MimicClient_Setup_v$ver.exe"
$serverSetup = Join-Path $root "release\MimicServer_Setup_v$serverVer.exe"

# ── PC Client ──────────────────────────────────────────────────────────────
if ($doPc) {
    Write-Step 'frontend (npm run build)'
    Push-Location (Join-Path $root 'shared\web')
    try {
        # Route through cmd so npm stderr warnings do not become PS terminating errors
        cmd /c "npm run build"
        if ($LASTEXITCODE) { throw 'npm build failed' }
    } finally { Pop-Location }
    Write-Ok 'dist'

    # Product modules only (skip legacy controller_server / h264_bench in release path)
    & "$PSScriptRoot\Build.ps1" -Module logger,capture,input,updater,test_target,mimic_client

    Write-Step 'assemble release\MimicClient'
    $rel = Join-Path $root 'release\MimicClient'
    if (Test-Path $rel) { Remove-Item -Recurse -Force $rel }
    New-Item -ItemType Directory -Force -Path $rel | Out-Null
    foreach ($sub in 'bin', 'frontend', 'config') {
        Copy-Item (Join-Path $root "pc\client\build\$sub") $rel -Recurse -Force
    }
    Write-Ok 'release\MimicClient'

    Write-Step 'normalize release EOL (LF)'
    $textExt = @('.json', '.html', '.css', '.js', '.mjs', '.cjs', '.map', '.txt', '.md', '.svg', '.xml')
    $nFix = 0
    Get-ChildItem -Path $rel -Recurse -File | Where-Object {
        $textExt -contains $_.Extension.ToLowerInvariant()
    } | ForEach-Object {
        $bytes = [IO.File]::ReadAllBytes($_.FullName)
        $hasCr = $false
        foreach ($b in $bytes) { if ($b -eq 13) { $hasCr = $true; break } }
        if ($hasCr) {
            $out = New-Object System.Collections.Generic.List[byte] ($bytes.Length)
            foreach ($b in $bytes) { if ($b -ne 13) { [void]$out.Add($b) } }
            [IO.File]::WriteAllBytes($_.FullName, $out.ToArray())
            $script:nFix++
        }
    }
    Write-Ok "LF-normalized $nFix file(s)"

    # Incremental by default (0.3.37+). Pass -Full only when the update mechanism itself breaks.
    & "$PSScriptRoot\New-VersionJson.ps1" -ReleaseDir $rel -Version $ver -Schema 3 -JumpPad '0.3.31'
}

# ── Server ─────────────────────────────────────────────────────────────────
if ($doServer) {
    Write-Step 'pack MimicServer'
    & "$PSScriptRoot\Pack-MimicServer.ps1" -Version $serverVer
}

# ── Android ────────────────────────────────────────────────────────────────
if ($doAndroid) {
    Write-Step 'build MimicAndroid APKs'
    & "$PSScriptRoot\Build-Android.ps1"
    $androidVer = [string](Get-Content -Raw $ajPath | ConvertFrom-Json).app
    Write-Step 'pack MimicAndroid'
    & "$PSScriptRoot\Pack-MimicAndroid.ps1" -Version $androidVer
}

# ── CDN ────────────────────────────────────────────────────────────────────
Write-Step 'publish CDN'
if ($doPc -and $doServer -and $doAndroid) {
    & "$PSScriptRoot\Publish-Cdn.ps1" -Version $ver
} elseif ($doPc -and -not $doServer -and -not $doAndroid) {
    & "$PSScriptRoot\Publish-Cdn.ps1" -Version $ver -ClientOnly -SkipAndroid
} elseif ($doAndroid -and -not $doPc -and -not $doServer) {
    & "$PSScriptRoot\Publish-Cdn.ps1" -AndroidOnly
} elseif ($doServer -and -not $doPc -and -not $doAndroid) {
    & "$PSScriptRoot\Publish-Cdn.ps1" -Version $serverVer -ServerOnly -SkipAndroid
} else {
    # Partial combos (e.g. PC+Android without server) — publish each track explicitly
    if ($doPc) { & "$PSScriptRoot\Publish-Cdn.ps1" -Version $ver -ClientOnly -SkipAndroid }
    if ($doServer) { & "$PSScriptRoot\Publish-Cdn.ps1" -Version $serverVer -ServerOnly -SkipAndroid }
    if ($doAndroid) { & "$PSScriptRoot\Publish-Cdn.ps1" -AndroidOnly }
}
Write-Ok 'CDN live'

# ── Thin installers (PC / Server only) ─────────────────────────────────────
$iscc = 'C:\Program Files\Inno Setup 6\ISCC.exe'
if ($doPc -or $doServer) {
    Write-Step 'thin installers (ISCC)'
    if (-not (Test-Path $iscc)) { throw 'Inno Setup 6 not found' }
    if ($doPc) {
        & $iscc "/DMyAppVersion=$ver" (Join-Path $root 'installer\setup.iss') | Out-Null
        if ($LASTEXITCODE -or -not (Test-Path $clientSetup)) { throw 'MimicClient_Setup ISCC failed' }
        Write-Ok (Split-Path $clientSetup -Leaf)
    }
    if ($doServer) {
        & $iscc "/DMyAppVersion=$serverVer" (Join-Path $root 'installer\setup_server.iss') | Out-Null
        if ($LASTEXITCODE -or -not (Test-Path $serverSetup)) { throw 'MimicServer_Setup ISCC failed' }
        Write-Ok (Split-Path $serverSetup -Leaf)
    }
}

if ($doPc -and -not $SkipVerify) {
    Write-Step 'isolated verify (gate)'
    $vArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "$PSScriptRoot\Verify.ps1", '-Version', $ver)
    if ($Interactive) { $vArgs += '-Interactive' }
    & powershell.exe @vArgs
    if ($LASTEXITCODE -ne 0) { throw 'isolated verification failed - nothing pushed, nothing published' }
} elseif ($doPc -and $SkipVerify) {
    Write-Warn2 'SkipVerify — isolated Verify.ps1 skipped'
} elseif (-not $doPc) {
    Write-Note 'Verify.ps1 skipped (no PC build)'
}

if ($DryRun) {
    Write-Host "`n==================== dry-run OK (CDN published; git/Gitee skipped) ====================" -ForegroundColor Green
    if ($doPc) { Write-Host "  Client setup (local): $clientSetup" }
    if ($doServer) { Write-Host "  Server setup (local): $serverSetup" }
    if ($doAndroid) { Write-Host "  Android: v$androidVer" }
    Write-Host "  CDN:    $cdnBase/"
    return
}

# Git — source only (release/ is gitignored)
Write-Step 'git commit + tag + push (source only)'
$eap = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
try {
    git add -A
    git add -u
    $parts = @()
    if ($doPc) { $parts += "client v$ver" }
    if ($doAndroid) { $parts += "android v$androidVer" }
    if ($doServer) { $parts += "server v$serverVer" }
    $msg = "release: $($parts -join ' + ') (CDN)"
    $commitOut = git commit -m $msg 2>&1 | Out-String
    Write-Host $commitOut
    if ($LASTEXITCODE -ne 0 -and $commitOut -notmatch 'nothing to commit') {
        throw "git commit failed (exit $LASTEXITCODE)"
    }
    if ($doPc) {
        git tag -f "v$ver" 2>&1 | Write-Host
    } elseif ($doAndroid) {
        git tag -f "android-v$androidVer" 2>&1 | Write-Host
    } elseif ($doServer) {
        git tag -f "server-v$serverVer" 2>&1 | Write-Host
    }
    git push origin main 2>&1 | Write-Host
    if ($LASTEXITCODE) { throw "git push main failed (exit $LASTEXITCODE)" }
    if ($doPc) {
        git push -f origin "refs/tags/v${ver}" 2>&1 | Write-Host
        if ($LASTEXITCODE) { throw "git tag push failed (exit $LASTEXITCODE)" }
    } elseif ($doAndroid) {
        git push -f origin "refs/tags/android-v${androidVer}" 2>&1 | Write-Host
    } elseif ($doServer) {
        git push -f origin "refs/tags/server-v${serverVer}" 2>&1 | Write-Host
    }

    git push github main 2>&1 | Write-Host
    if ($LASTEXITCODE) {
        Write-Warn2 "git push github main failed (exit $LASTEXITCODE)"
    } else { Write-Ok 'pushed github main' }
    if ($doPc) {
        git push -f github "refs/tags/v${ver}" 2>&1 | Write-Host
        if ($LASTEXITCODE) {
            Write-Warn2 "git push github tag failed (exit $LASTEXITCODE)"
        } else { Write-Ok "pushed github v$ver" }
    } elseif ($doAndroid) {
        git push -f github "refs/tags/android-v${androidVer}" 2>&1 | Write-Host
    } elseif ($doServer) {
        git push -f github "refs/tags/server-v${serverVer}" 2>&1 | Write-Host
    }
}
finally { $ErrorActionPreference = $eap }
Write-Ok "pushed origin main"

# Gitee thin Setup — opt-in only (old Setup + CDN in-app update is the default path)
if ($PublishGitee) {
    Write-Step 'publish Gitee thin Setups (-PublishGitee)'
    if ($doPc -and $doServer) {
        & "$PSScriptRoot\Publish.ps1" -Version $ver -ServerVersion $serverVer
    } elseif ($doPc) {
        & "$PSScriptRoot\Publish.ps1" -Version $ver -ClientOnly
    } elseif ($doServer) {
        & "$PSScriptRoot\Publish.ps1" -Version $serverVer -ServerOnly -ServerVersion $serverVer
    } else {
        Write-Note 'Android Gitee attach: use Pack-MimicAndroid zip on release page manually if needed'
    }
} else {
    Write-Note 'Gitee Setup skipped (default). Use -PublishGitee to attach thin installers.'
}

if ($doPc) {
    Write-Step 'verify CDN version.json'
    try {
        $r = Invoke-RestMethod -Uri "$cdnBase/client/version.json" -Method Get -TimeoutSec 30
        if ($r.app -eq $ver) { Write-Ok "CDN 200, app=$($r.app)" } else { Write-Warn2 "CDN app mismatch: $($r.app)" }
        if ($r.download_base -notlike '*47.107.43.5*') {
            Write-Warn2 "download_base unexpected: $($r.download_base)"
        }
    }
    catch { Write-Warn2 "CDN URL check failed: $_" }
}
if ($doAndroid) {
    try {
        $ar = Invoke-RestMethod -Uri "$cdnBase/android/version.json" -Method Get -TimeoutSec 30
        Write-Ok "CDN android app=$($ar.app)"
    }
    catch { Write-Warn2 "CDN android check failed: $_" }
}

Write-Host "`n==================== release DONE ====================" -ForegroundColor Green
Write-Host "  CDN client:  $cdnBase/client/"
Write-Host "  CDN server:  $cdnBase/server/"
Write-Host "  CDN android: $cdnBase/android/"
if ($doPc) {
    Write-Host "  PC update:     in-app → v$ver"
}
if ($doAndroid) {
    Write-Host "  Android update: in-app → v$androidVer"
}
if ($PublishGitee) {
    if ($doPc) {
        Write-Host "  Gitee Client:  https://gitee.com/Andyqwe44/mimic/releases/download/v$ver/MimicClient_Setup_v$ver.exe"
    }
    if ($doServer) {
        Write-Host "  Gitee Server:  https://gitee.com/Andyqwe44/mimic/releases/download/v$ver/MimicServer_Setup_v$serverVer.exe"
    }
}
