# Build.ps1 ??compile all native modules in one VS Dev Shell.
#
# Replaces logger/build_logger_lib.cmd, capture/build_capture_lib.cmd,
# input/build_input_lib.cmd, updater/build.cmd, mimic_client/build.cmd +
# build_dev.cmd ??and the `cmd /c build_*.cmd` chain in build_release.cmd.
# One PowerShell session enters the build shell ONCE, then builds every module.
#
#   powershell -File scripts\Build.ps1                # build everything (prod)
#   powershell -File scripts\Build.ps1 -Module logger # build one module
#
# Modules migrated so far: logger. (capture/input/updater/mimic_client to follow.)

param(
    [ValidateSet('all', 'logger', 'capture', 'input', 'updater', 'mimic_client', 'controller_server', 'test_target', 'h264_bench')]
    [string[]]$Module = @('all')
)

. "$PSScriptRoot\lib\Common.ps1"

$Root = Get-RepoRoot
$Ver = Get-AppVersion
# Module version for the native libs/DLLs (logger, capture, input) ??DECOUPLED
# from APP_VERSION so an app bump doesn't change every DLL's VERSIONINFO bytes
# (that churn defeated incremental updates: every release re-downloaded all 12
# DLLs). Bump this ONLY when a lib's source actually changes. Combined with
# /Brepro (deterministic PE), same source ??same bytes ??same sha256 ??not in diff.
$LibVer = '1.0.0'
Enter-BuildShell

function Build-Logger {
    Write-Step "logger.dll (lib v$LibVer)"
    $dir = Join-Path $Root 'pc\logger'
    $bld = Join-Path $dir 'build'
    New-Item -ItemType Directory -Force -Path $bld | Out-Null
    New-VerModuleHeader -OutPath (Join-Path $bld '_ver_module.h') `
        -Version $LibVer -ModuleDesc 'Unified Logging Engine' -FileType VFT_DLL
    Push-Location $dir
    try {
        Invoke-Native { cl.exe /nologo /EHsc /std:c++17 /source-charset:utf-8 /I "$Root\pc\common\include" /I build `
                /DGAM_BUILD_DLL /MT /c /Fo"build\logger.obj" logger.cpp } 'logger cl'
        Invoke-Native { rc.exe /nologo /I build /fo build\logger.res "$Root\pc\common\version.rc" } 'logger rc'
        Invoke-Native { link.exe /nologo /DLL /NXCOMPAT /DYNAMICBASE /Brepro /OUT:build\logger.dll `
                build\logger.obj build\logger.res /IMPLIB:build\logger.lib } 'logger link'
    }
    finally { Pop-Location }
    Write-Ok 'logger.dll + logger.lib'
}

function Build-Capture {
    Write-Step "capture DLLs (lib v$LibVer)"
    $dir = Join-Path $Root 'pc\capture'
    $bld = Join-Path $dir 'build'
    New-Item -ItemType Directory -Force -Path $bld | Out-Null

    $cflags = @('/nologo', '/EHsc', '/std:c++17', '/source-charset:utf-8', '/I', 'include', '/I', "$Root\pc\common\include",
        '/DGAM_BUILD_DLL', '/c', '/MT')
    $syslibs = @('user32.lib', 'gdi32.lib', 'dwmapi.lib', "$Root\pc\logger\build\logger.lib")
    $commonLib = "$Root\pc\capture\build\capture_common.lib"

    # Data-driven module table (replaces the 6 copy-pasted cmd blocks). common
    # first ??the others link against capture_common.lib.
    $mods = @(
        @{ name = 'capture_common';  srcs = @('capture_common');                libs = @();                                                     desc = 'Capture Common Utilities' }
        @{ name = 'capture_wgc';     srcs = @('capture_wgc', 'capture_wgc_ffi'); libs = @('d3d11.lib', 'dxgi.lib', 'windowsapp.lib', $commonLib); desc = 'WGC GPU Capture Module' }
        @{ name = 'capture_gdi';     srcs = @('capture_gdi');                    libs = @($commonLib);                                           desc = 'GDI GetWindowDC Capture Module' }
        @{ name = 'capture_pw';      srcs = @('capture_pw');                     libs = @($commonLib);                                           desc = 'PrintWindow Capture Module' }
        @{ name = 'capture_screen';  srcs = @('capture_screen');                 libs = @($commonLib);                                           desc = 'Screen BitBlt Capture Module' }
        @{ name = 'capture_desktop'; srcs = @('capture_desktop');                libs = @($commonLib);                                           desc = 'Desktop BitBlt Capture Module' }
    )

    Push-Location $dir
    try {
        foreach ($mod in $mods) {
            New-VerModuleHeader -OutPath 'build\_ver_module.h' -Version $LibVer -ModuleDesc $mod.desc -FileType VFT_DLL
            $objs = @()
            foreach ($s in $mod.srcs) {
                cl.exe @cflags /Fo"build\$s.obj" "src\$s.cpp"
                if ($LASTEXITCODE) { throw "capture: cl $s failed" }
                $objs += "build\$s.obj"
            }
            rc.exe /nologo /I build /fo "build\$($mod.name).res" "$Root\pc\common\version.rc"
            if ($LASTEXITCODE) { throw "capture: rc $($mod.name) failed" }
            $lnk = @('/nologo', '/DLL', '/NXCOMPAT', '/DYNAMICBASE', '/Brepro', "/OUT:build\$($mod.name).dll") +
                $objs + @("build\$($mod.name).res") + $mod.libs + $syslibs + @("/IMPLIB:build\$($mod.name).lib")
            link.exe @lnk
            if ($LASTEXITCODE) { throw "capture: link $($mod.name) failed" }
        }
    }
    finally { Pop-Location }
    Write-Ok '6 capture DLLs'
}

function Build-Input {
    Write-Step "input DLLs (lib v$LibVer)"
    $dir = Join-Path $Root 'pc\input'
    New-Item -ItemType Directory -Force -Path (Join-Path $dir 'build') | Out-Null
    $cflags = @('/nologo', '/EHsc', '/std:c++17', '/source-charset:utf-8', '/I', 'include', '/I', "$Root\pc\common\include",
        '/I', "$Root\pc\client\src", '/I', "$Root\pc\capture\include", '/DGAM_BUILD_DLL', '/MT', '/c')
    $syslibs = @('user32.lib', "$Root\pc\logger\build\logger.lib")
    $commonLib = "$Root\pc\input\build\input_common.lib"
    $mods = @(
        @{ name = 'input_common';      srcs = @('input_common');      libs = @();           desc = 'Input Common Utilities' }
        @{ name = 'input_sendinput';   srcs = @('input_sendinput');   libs = @($commonLib); desc = 'SendInput Module' }
        @{ name = 'input_winapi';      srcs = @('input_winapi');      libs = @($commonLib); desc = 'WinAPI Input Module' }
        @{ name = 'input_postmessage'; srcs = @('input_postmessage'); libs = @($commonLib); desc = 'PostMessage Input Module' }
        @{ name = 'input_driver';      srcs = @('input_driver');      libs = @($commonLib); desc = 'Driver Input Stub' }
    )
    Push-Location $dir
    try {
        foreach ($mod in $mods) {
            New-VerModuleHeader -OutPath 'build\_ver_module.h' -Version $LibVer -ModuleDesc $mod.desc -FileType VFT_DLL
            $objs = @()
            foreach ($s in $mod.srcs) {
                cl.exe @cflags /Fo"build\$s.obj" "src\$s.cpp"
                if ($LASTEXITCODE) { throw "input: cl $s failed" }
                $objs += "build\$s.obj"
            }
            rc.exe /nologo /I build /fo "build\$($mod.name).res" "$Root\pc\common\version.rc"
            if ($LASTEXITCODE) { throw "input: rc $($mod.name) failed" }
            $lnk = @('/nologo', '/DLL', '/NXCOMPAT', '/DYNAMICBASE', '/Brepro', "/OUT:build\$($mod.name).dll") +
                $objs + @("build\$($mod.name).res") + $mod.libs + $syslibs + @("/IMPLIB:build\$($mod.name).lib")
            link.exe @lnk
            if ($LASTEXITCODE) { throw "input: link $($mod.name) failed" }
        }
    }
    finally { Pop-Location }
    Write-Ok '5 input DLLs'
}

function Build-Updater {
    Write-Step "updater.exe (v$Ver)"
    Push-Location (Join-Path $Root 'pc\updater')
    try {
        New-Item -ItemType Directory -Force -Path 'build' | Out-Null
        # --% (stop-parsing) passes the rest verbatim to cl ??avoids PowerShell
        # mangling the /MANIFESTUAC quotes. The whole line is literal (no PS vars).
        cl.exe --% /nologo /EHsc /std:c++17 /source-charset:utf-8 /DNDEBUG /O2 /GS- /Gy /Gw /MT /Fobuild\updater.obj /Fe:build\updater.exe updater.cpp advapi32.lib shell32.lib user32.lib kernel32.lib /link /OPT:REF /OPT:ICF /Brepro /SUBSYSTEM:WINDOWS /MANIFEST:EMBED /MANIFESTUAC:"level='requireAdministrator' uiAccess='false'"
        if ($LASTEXITCODE) { throw 'updater: build failed' }
    }
    finally { Pop-Location }
    Write-Ok 'updater.exe'
}

function Build-MimicClient {
    Write-Step "mimic_client.exe (PROD, v$Ver)"
    $dir = Join-Path $Root 'pc\client'
    $out = 'build'
    Push-Location $dir
    try {
        if (Test-Path $out) { Remove-Item -Recurse -Force $out }
        New-Item -ItemType Directory -Force -Path "$out\bin", "$out\config", "$out\frontend" | Out-Null

        $inc = @('/I', 'src', '/I', 'dep', '/I', "$Root\pc\capture\include", '/I', "$Root\pc\common\include")
        $cflags = @('/nologo', '/EHsc', '/std:c++17', '/source-charset:utf-8') + $inc + @('/DNDEBUG', '/O2', '/GS-', '/Gy', '/Gw', '/MT')
        $lflags = @('d3d11.lib', 'dxgi.lib', 'windowsapp.lib', 'user32.lib', 'gdi32.lib', 'ole32.lib',
            'oleaut32.lib', 'ws2_32.lib', 'windowscodecs.lib', 'dwmapi.lib', 'shell32.lib', 'shlwapi.lib',
            'winhttp.lib', 'bcrypt.lib', 'advapi32.lib', 'mfplat.lib', 'mf.lib', 'mfuuid.lib', 'wmcodecdspuuid.lib')
        $linkflags = @('/OPT:REF', '/OPT:ICF', '/Brepro')
        $srcs = @('src\main.cpp', 'src\commands.cpp', 'src\h264_encoder.cpp', 'src\ws_client.cpp',
            'src\peer_session.cpp', 'src\peer_udp.cpp',
            'src\virtual_desktop.cpp', 'src\paths.cpp', 'src\sha256_util.cpp', 'src\update_verify.cpp')
        $libs = @('dep\WebView2LoaderStatic.lib', "$Root\pc\logger\build\logger.lib") +
        (@('common', 'wgc', 'gdi', 'pw', 'screen', 'desktop') | ForEach-Object { "$Root\pc\capture\build\capture_$_.lib" }) +
        (@('common', 'sendinput', 'winapi', 'postmessage', 'driver') | ForEach-Object { "$Root\pc\input\build\input_$_.lib" })

        rc.exe /nologo /fo "$out\app.res" app.rc
        if ($LASTEXITCODE) { throw 'mimic_client: rc failed' }

        $clArgs = $cflags + @("/Fo$out\", "/Fe:$out\bin\mimic_client.exe") + $srcs + @("$out\app.res") +
        $libs + $lflags + @('/link') + $linkflags
        cl.exe @clArgs
        if ($LASTEXITCODE) { throw 'mimic_client: cl failed' }

        $dlls = @("$Root\pc\logger\build\logger.dll") +
        (@('common', 'wgc', 'gdi', 'pw', 'screen', 'desktop') | ForEach-Object { "$Root\pc\capture\build\capture_$_.dll" }) +
        (@('common', 'sendinput', 'winapi', 'postmessage', 'driver') | ForEach-Object { "$Root\pc\input\build\input_$_.dll" })
        foreach ($d in $dlls) { Copy-Item $d "$out\bin\" -Force }

        $ttExe = Join-Path $Root 'pc\test_target\build\test_target.exe'
        $ttUi = Join-Path $Root 'pc\test_target\ui'
        if (Test-Path $ttExe) {
            New-Item -ItemType Directory -Force -Path "$out\bin\test_target" | Out-Null
            Copy-Item $ttExe "$out\bin\test_target\" -Force
            if (Test-Path $ttUi) { Copy-Item $ttUi "$out\bin\test_target\" -Recurse -Force }
        }

        $settings = Join-Path $Root 'config\settings.default.json'
        if (Test-Path $settings) { Copy-Item $settings "$out\config\" -Force }

        $upd = Join-Path $Root 'pc\updater\build\updater.exe'
        if (Test-Path $upd) {
            Copy-Item $upd "$out\bin\" -Force
            # No updater.new ??MimicClient installs updater.exe from staging; updater skips self.
        }
        $dist = Join-Path $Root 'shared\web\dist'
        if (Test-Path $dist) { Copy-Item "$dist\*" "$out\frontend\" -Recurse -Force }
    }
    finally { Pop-Location }
    Write-Ok "mimic_client.exe"
}

function Build-ControllerServer {
    Write-Step 'controller_server.exe (HTTP+WS relay)'
    $dir = Join-Path $Root 'pc\legacy\controller_server'
    $bld = Join-Path $dir 'build'
    New-Item -ItemType Directory -Force -Path $bld | Out-Null

    # Build controller_web ??stage as www/
    $ctrlSrc = Join-Path $Root 'pc\legacy\controller_web'
    $ctrlDist = Join-Path $ctrlSrc 'dist'
    if (Test-Path (Join-Path $ctrlSrc 'package.json')) {
        Write-Step 'controller_web (vite build)'
        Push-Location $ctrlSrc
        try {
            if (-not (Test-Path 'node_modules')) { cmd /c "npm install" }
            cmd /c "npm run build"
            if ($LASTEXITCODE) { throw 'controller_web: vite build failed' }
        }
        finally { Pop-Location }
    }

    Push-Location $dir
    try {
        $inc = @('/I', "$Root\pc\logger")
        $cflags = @('/nologo', '/EHsc', '/std:c++17', '/source-charset:utf-8', '/DNOMINMAX', '/DNDEBUG', '/O2', '/MT') + $inc
        $libs = @("$Root\pc\logger\build\logger.lib")
        $lflags = @('ws2_32.lib', 'bcrypt.lib', 'advapi32.lib', 'user32.lib')
        $clArgs = $cflags + @("/Fo$bld\", "/Fe:$bld\controller_server.exe", 'main.cpp') + $libs + $lflags
        cl.exe @clArgs
        if ($LASTEXITCODE) { throw 'controller_server: cl failed' }
        Copy-Item "$Root\pc\logger\build\logger.dll" "$bld\" -Force
        if (Test-Path $ctrlDist) {
            New-Item -ItemType Directory -Force -Path "$bld\www" | Out-Null
            Copy-Item "$ctrlDist\*" "$bld\www\" -Recurse -Force
        }
    }
    finally { Pop-Location }
    Write-Ok 'controller_server.exe'
}

function Build-TestTarget {
    Write-Step 'test_target.exe (WebView2)'
    $dir = Join-Path $Root 'pc\test_target'
    $bld = Join-Path $dir 'build'
    New-Item -ItemType Directory -Force -Path $bld | Out-Null
    Push-Location $dir
    try {
        $wvLib = Join-Path $Root 'pc\client\dep\WebView2LoaderStatic.lib'
        if (-not (Test-Path $wvLib)) {
            throw "WebView2LoaderStatic.lib missing at $wvLib (needed to link test_target)"
        }
        $cflags = @('/nologo', '/EHsc', '/std:c++17', '/source-charset:utf-8',
            '/I', "$Root\pc\client\dep", '/MT', '/O2', '/DNDEBUG')
        $libs = @($wvLib, 'user32.lib', 'gdi32.lib', 'ole32.lib', 'oleaut32.lib',
            'ws2_32.lib', 'shell32.lib', 'shlwapi.lib', 'version.lib', 'advapi32.lib')
        $clArgs = $cflags + @("/Fo$bld\", "/Fe:$bld\test_target.exe", 'test_target.cpp') +
            $libs + @('/link', '/SUBSYSTEM:WINDOWS', '/OPT:REF', '/OPT:ICF')
        cl.exe @clArgs
        if ($LASTEXITCODE) { throw 'test_target: cl failed' }
        # Stage ui/ next to the exe for local launches from test_target\build\
        $uiSrc = Join-Path $dir 'ui'
        if (Test-Path $uiSrc) {
            New-Item -ItemType Directory -Force -Path "$bld\ui" | Out-Null
            Copy-Item "$uiSrc\*" "$bld\ui\" -Recurse -Force
        }
    }
    finally { Pop-Location }
    Write-Ok 'test_target.exe'
}

function Build-H264Bench {
    Write-Step 'h264_hw_bench.exe (WGC?HW H.264?TCP)'
    $dir = Join-Path $Root 'pc\test'
    $bld = Join-Path $dir 'build'
    New-Item -ItemType Directory -Force -Path $bld | Out-Null
    $inc = @('/I', "$Root\pc\client\src", '/I', "$Root\pc\capture\include", '/I', "$Root\pc\common\include", '/I', "$Root\pc\logger")
    $cflags = @('/nologo', '/EHsc', '/std:c++17', '/source-charset:utf-8', '/DNOMINMAX', '/DNDEBUG', '/O2', '/MT') + $inc
    $libs = @(
        "$Root\pc\logger\build\logger.lib",
        "$Root\pc\capture\build\capture_common.lib",
        "$Root\pc\capture\build\capture_wgc.lib"
    )
    $lflags = @('d3d11.lib', 'dxgi.lib', 'windowsapp.lib', 'user32.lib', 'gdi32.lib', 'ole32.lib',
        'oleaut32.lib', 'mfplat.lib', 'mf.lib', 'mfuuid.lib', 'wmcodecdspuuid.lib', 'ws2_32.lib', 'advapi32.lib')
    Push-Location $dir
    try {
        $clArgs = $cflags + @(
            "/Fo$bld\", "/Fe:$bld\h264_hw_bench.exe",
            'h264_hw_bench.cpp',
            "$Root\pc\client\src\h264_encoder.cpp"
        ) + $libs + $lflags
        cl.exe @clArgs
        if ($LASTEXITCODE) { throw 'h264_hw_bench: cl failed' }
        Copy-Item "$Root\pc\logger\build\logger.dll" "$bld\" -Force
        Copy-Item "$Root\pc\capture\build\capture_common.dll" "$bld\" -Force
        Copy-Item "$Root\pc\capture\build\capture_wgc.dll" "$bld\" -Force
    }
    finally { Pop-Location }
    Write-Ok 'h264_hw_bench.exe'
}

# Dispatch ??flat conditionals in dependency order (capture/input/mimic_client link
# against logger.lib etc). Explicit ifs, not foreach+switch.
$all = $Module -contains 'all'
if ($all -or $Module -contains 'logger')      { Build-Logger }
if ($all -or $Module -contains 'capture')     { Build-Capture }
if ($all -or $Module -contains 'input')       { Build-Input }
if ($all -or $Module -contains 'updater')     { Build-Updater }
if ($all -or $Module -contains 'test_target') { Build-TestTarget }
if ($all -or $Module -contains 'h264_bench')  { Build-H264Bench }
if ($all -or $Module -contains 'controller_server') { Build-ControllerServer }
if ($all -or $Module -contains 'mimic_client') { Build-MimicClient }
