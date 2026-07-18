# Release.ps1 — build → CDN shelf → thin Setups (local) → verify → git.
# Gitee Setup upload is OFF by default (old Setup + in-app update is enough).
# Opt in with -PublishGitee when a new thin installer must be attached on Gitee.
#
#   powershell -File scripts\Release.ps1
#   powershell -File scripts\Release.ps1 -DryRun
#   powershell -File scripts\Release.ps1 -ClientOnly
#   powershell -File scripts\Release.ps1 -ServerOnly
#   powershell -File scripts\Release.ps1 -PublishGitee
#
# Binaries live on http://47.107.43.5/mimic/ — NOT in git.

[CmdletBinding()]
param(
    [switch]$DryRun,
    [switch]$Interactive,
    [switch]$SkipVerify,
    [switch]$ClientOnly,
    [switch]$ServerOnly,
    [switch]$PublishGitee
)

if ($ClientOnly -and $ServerOnly) { throw 'Use only one of -ClientOnly / -ServerOnly (or neither for both)' }

. "$PSScriptRoot\lib\Common.ps1"

$root = Get-RepoRoot
$ver = Get-AppVersion
$serverVer = Get-ServerVersion
$cdnBase = 'http://47.107.43.5/mimic'
$doClient = -not $ServerOnly
$doServer = -not $ClientOnly
Write-Step "Release client=$doClient(v$ver) server=$doServer(v$serverVer)$(if ($DryRun) { ' (dry-run)' })"

$clientSetup = Join-Path $root "release\MimicClient_Setup_v$ver.exe"
$serverSetup = Join-Path $root "release\MimicServer_Setup_v$serverVer.exe"

if ($doClient) {
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

if ($doServer) {
    Write-Step 'pack MimicServer'
    & "$PSScriptRoot\Pack-MimicServer.ps1" -Version $serverVer
}

$androidVer = '0.1.0'
$ajPath = Join-Path $root 'android\version.json'
if (Test-Path $ajPath) {
    $androidVer = [string](Get-Content -Raw $ajPath | ConvertFrom-Json).app
}
if ($doClient) {
    Write-Step 'build MimicAndroid APKs'
    & "$PSScriptRoot\Build-Android.ps1"
    $androidVer = [string](Get-Content -Raw $ajPath | ConvertFrom-Json).app
    Write-Step 'pack MimicAndroid'
    & "$PSScriptRoot\Pack-MimicAndroid.ps1" -Version $androidVer
}

Write-Step 'publish CDN'
if ($doClient -and $doServer) {
    & "$PSScriptRoot\Publish-Cdn.ps1" -Version $ver
} elseif ($ClientOnly) {
    & "$PSScriptRoot\Publish-Cdn.ps1" -Version $ver -ClientOnly
} else {
    & "$PSScriptRoot\Publish-Cdn.ps1" -Version $serverVer -ServerOnly -SkipAndroid
}
Write-Ok 'CDN live'

Write-Step 'thin installers (ISCC)'
$iscc = 'C:\Program Files\Inno Setup 6\ISCC.exe'
if (-not (Test-Path $iscc)) { throw 'Inno Setup 6 not found' }
if ($doClient) {
    & $iscc "/DMyAppVersion=$ver" (Join-Path $root 'installer\setup.iss') | Out-Null
    if ($LASTEXITCODE -or -not (Test-Path $clientSetup)) { throw 'MimicClient_Setup ISCC failed' }
    Write-Ok (Split-Path $clientSetup -Leaf)
}
if ($doServer) {
    & $iscc "/DMyAppVersion=$serverVer" (Join-Path $root 'installer\setup_server.iss') | Out-Null
    if ($LASTEXITCODE -or -not (Test-Path $serverSetup)) { throw 'MimicServer_Setup ISCC failed' }
    Write-Ok (Split-Path $serverSetup -Leaf)
}

if ($doClient -and -not $SkipVerify) {
    Write-Step 'isolated verify (gate)'
    $vArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "$PSScriptRoot\Verify.ps1", '-Version', $ver)
    if ($Interactive) { $vArgs += '-Interactive' }
    & powershell.exe @vArgs
    if ($LASTEXITCODE -ne 0) { throw 'isolated verification failed - nothing pushed, nothing published' }
} elseif (-not $doClient) {
    Write-Warn2 'ServerOnly ??isolated Verify.ps1 skipped'
} else {
    Write-Warn2 'SkipVerify ??isolated Verify.ps1 skipped'
}

if ($DryRun) {
    Write-Host "`n==================== dry-run OK (CDN published; git/Gitee skipped) ====================" -ForegroundColor Green
    if ($doClient) { Write-Host "  Client setup (local): $clientSetup" }
    if ($doServer) { Write-Host "  Server setup (local): $serverSetup" }
    Write-Host "  CDN:    $cdnBase/"
    return
}

# Git — source only (release/ is gitignored)
Write-Step 'git commit + tag + push (source only)'
$tagName = if ($doClient) { "v$ver" } else { "server-v$serverVer" }
$eap = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
try {
    git add -A
    git add -u
    $msg = if ($doClient -and $doServer) {
        "release: client v$ver + server v$serverVer (CDN)"
    } elseif ($doClient) {
        "release: client v$ver (CDN)"
    } else {
        "release: server v$serverVer (CDN)"
    }
    $commitOut = git commit -m $msg 2>&1 | Out-String
    Write-Host $commitOut
    if ($LASTEXITCODE -ne 0 -and $commitOut -notmatch 'nothing to commit') {
        throw "git commit failed (exit $LASTEXITCODE)"
    }
    if ($doClient) {
        git tag -f "v$ver" 2>&1 | Write-Host
    }
    git push origin main 2>&1 | Write-Host
    if ($LASTEXITCODE) { throw "git push main failed (exit $LASTEXITCODE)" }
    if ($doClient) {
        git push -f origin "refs/tags/v${ver}" 2>&1 | Write-Host
        if ($LASTEXITCODE) { throw "git tag push failed (exit $LASTEXITCODE)" }
    }

    git push github main 2>&1 | Write-Host
    if ($LASTEXITCODE) {
        Write-Warn2 "git push github main failed (exit $LASTEXITCODE)"
    } else { Write-Ok 'pushed github main' }
    if ($doClient) {
        git push -f github "refs/tags/v${ver}" 2>&1 | Write-Host
        if ($LASTEXITCODE) {
            Write-Warn2 "git push github tag failed (exit $LASTEXITCODE)"
        } else { Write-Ok "pushed github v$ver" }
    }
}
finally { $ErrorActionPreference = $eap }
Write-Ok "pushed origin main"

# Gitee thin Setup — opt-in only (old Setup + CDN in-app update is the default path)
if ($PublishGitee) {
    Write-Step 'publish Gitee thin Setups (-PublishGitee)'
    if ($doClient -and $doServer) {
        & "$PSScriptRoot\Publish.ps1" -Version $ver -ServerVersion $serverVer
    } elseif ($doClient) {
        & "$PSScriptRoot\Publish.ps1" -Version $ver -ClientOnly
    } else {
        & "$PSScriptRoot\Publish.ps1" -Version $serverVer -ServerOnly -ServerVersion $serverVer
    }
} else {
    Write-Note 'Gitee Setup skipped (default). Use -PublishGitee to attach thin installers.'
}

if ($doClient) {
    Write-Step 'verify CDN version.json'
    try {
        $r = Invoke-RestMethod -Uri "$cdnBase/client/version.json" -Method Get -TimeoutSec 30
        if ($r.app -eq $ver) { Write-Ok "CDN 200, app=$($r.app)" } else { Write-Warn2 "CDN app mismatch: $($r.app)" }
        if ($r.download_base -notlike '*47.107.43.5*') {
            Write-Warn2 "download_base unexpected: $($r.download_base)"
        }
    }
    catch { Write-Warn2 "CDN URL check failed: $_" }
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
if ($doClient) {
    Write-Host "  Update path: old Setup → install → in-app update to v$ver / android v$androidVer"
}
if ($PublishGitee) {
    if ($doClient) {
        Write-Host "  Gitee Client:  https://gitee.com/Andyqwe44/mimic/releases/download/v$ver/MimicClient_Setup_v$ver.exe"
    }
    if ($doServer) {
        Write-Host "  Gitee Server:  https://gitee.com/Andyqwe44/mimic/releases/download/v$ver/MimicServer_Setup_v$serverVer.exe"
    }
    if ($doClient) {
        Write-Host "  Gitee Android: https://gitee.com/Andyqwe44/mimic/releases/download/v$ver/MimicAndroid_Setup_v$androidVer.apk"
    }
}
