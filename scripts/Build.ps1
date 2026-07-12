# Build.ps1 — compile all native modules in one VS Dev Shell.
#
# Replaces logger/build_logger_lib.cmd, capture/build_capture_lib.cmd,
# input/build_input_lib.cmd, updater/build.cmd, monitor_app/build.cmd +
# build_dev.cmd — and the `cmd /c build_*.cmd` chain in build_release.cmd.
# One PowerShell session enters the build shell ONCE, then builds every module.
#
#   powershell -File scripts\Build.ps1                # build everything (prod)
#   powershell -File scripts\Build.ps1 -Module logger # build one module
#
# Modules migrated so far: logger. (capture/input/updater/monitor_app to follow.)

param(
    [ValidateSet('all', 'logger', 'capture', 'input', 'updater', 'monitor_app')]
    [string[]]$Module = @('all'),
    [switch]$Dev
)

. "$PSScriptRoot\lib\Common.ps1"

$Root = Get-RepoRoot
$Ver = Get-AppVersion
Enter-BuildShell

function Build-Logger {
    Write-Step "logger.dll (v$Ver)"
    $dir = Join-Path $Root 'logger'
    $bld = Join-Path $dir 'build'
    New-Item -ItemType Directory -Force -Path $bld | Out-Null
    New-VerModuleHeader -OutPath (Join-Path $bld '_ver_module.h') `
        -Version $Ver -ModuleDesc 'Unified Logging Engine' -FileType VFT_DLL
    Push-Location $dir
    try {
        Invoke-Native { cl.exe /nologo /EHsc /std:c++17 /I "$Root\common\include" /I build `
                /DGAM_BUILD_DLL /MT /c /Fo"build\logger.obj" logger.cpp } 'logger cl'
        Invoke-Native { rc.exe /nologo /I build /fo build\logger.res "$Root\common\version.rc" } 'logger rc'
        Invoke-Native { link.exe /nologo /DLL /NXCOMPAT /DYNAMICBASE /OUT:build\logger.dll `
                build\logger.obj build\logger.res /IMPLIB:build\logger.lib } 'logger link'
    }
    finally { Pop-Location }
    Write-Ok 'logger.dll + logger.lib'
}

function Build-Capture {
    Write-Step "capture DLLs (v$Ver)"
    $dir = Join-Path $Root 'capture'
    $bld = Join-Path $dir 'build'
    New-Item -ItemType Directory -Force -Path $bld | Out-Null

    $cflags = @('/nologo', '/EHsc', '/std:c++17', '/I', 'include', '/I', "$Root\common\include",
        '/DGAM_BUILD_DLL', '/c', '/MT')
    $syslibs = @('user32.lib', 'gdi32.lib', 'dwmapi.lib', "$Root\logger\build\logger.lib")
    $commonLib = "$Root\capture\build\capture_common.lib"

    # Data-driven module table (replaces the 6 copy-pasted cmd blocks). common
    # first — the others link against capture_common.lib.
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
            New-VerModuleHeader -OutPath 'build\_ver_module.h' -Version $Ver -ModuleDesc $mod.desc -FileType VFT_DLL
            $objs = @()
            foreach ($s in $mod.srcs) {
                cl.exe @cflags /Fo"build\$s.obj" "src\$s.cpp"
                if ($LASTEXITCODE) { throw "capture: cl $s failed" }
                $objs += "build\$s.obj"
            }
            rc.exe /nologo /I build /fo "build\$($mod.name).res" "$Root\common\version.rc"
            if ($LASTEXITCODE) { throw "capture: rc $($mod.name) failed" }
            $lnk = @('/nologo', '/DLL', '/NXCOMPAT', '/DYNAMICBASE', "/OUT:build\$($mod.name).dll") +
                $objs + @("build\$($mod.name).res") + $mod.libs + $syslibs + @("/IMPLIB:build\$($mod.name).lib")
            link.exe @lnk
            if ($LASTEXITCODE) { throw "capture: link $($mod.name) failed" }
        }
    }
    finally { Pop-Location }
    Write-Ok '6 capture DLLs'
}

function Build-Input {
    Write-Step "input DLLs (v$Ver)"
    $dir = Join-Path $Root 'input'
    New-Item -ItemType Directory -Force -Path (Join-Path $dir 'build') | Out-Null
    $cflags = @('/nologo', '/EHsc', '/std:c++17', '/I', 'include', '/I', "$Root\common\include",
        '/I', "$Root\monitor_app\src", '/I', "$Root\capture\include", '/DGAM_BUILD_DLL', '/MT', '/c')
    $syslibs = @('user32.lib', "$Root\logger\build\logger.lib")
    $commonLib = "$Root\input\build\input_common.lib"
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
            New-VerModuleHeader -OutPath 'build\_ver_module.h' -Version $Ver -ModuleDesc $mod.desc -FileType VFT_DLL
            $objs = @()
            foreach ($s in $mod.srcs) {
                cl.exe @cflags /Fo"build\$s.obj" "src\$s.cpp"
                if ($LASTEXITCODE) { throw "input: cl $s failed" }
                $objs += "build\$s.obj"
            }
            rc.exe /nologo /I build /fo "build\$($mod.name).res" "$Root\common\version.rc"
            if ($LASTEXITCODE) { throw "input: rc $($mod.name) failed" }
            $lnk = @('/nologo', '/DLL', '/NXCOMPAT', '/DYNAMICBASE', "/OUT:build\$($mod.name).dll") +
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
    Push-Location (Join-Path $Root 'updater')
    try {
        New-Item -ItemType Directory -Force -Path 'build' | Out-Null
        # --% (stop-parsing) passes the rest verbatim to cl — avoids PowerShell
        # mangling the /MANIFESTUAC quotes. The whole line is literal (no PS vars).
        cl.exe --% /nologo /EHsc /std:c++17 /DNDEBUG /O2 /GS- /Gy /Gw /MT /Fobuild\updater.obj /Fe:build\updater.exe updater.cpp advapi32.lib shell32.lib user32.lib kernel32.lib /link /OPT:REF /OPT:ICF /SUBSYSTEM:WINDOWS /MANIFEST:EMBED /MANIFESTUAC:"level='requireAdministrator' uiAccess='false'"
        if ($LASTEXITCODE) { throw 'updater: build failed' }
    }
    finally { Pop-Location }
    Write-Ok 'updater.exe'
}

function Build-MonitorApp {
    $cfg = if ($Dev) { 'DEV' } else { 'PROD' }
    Write-Step "monitor_app.exe ($cfg, v$Ver)"
    $dir = Join-Path $Root 'monitor_app'
    $out = if ($Dev) { 'build_dev' } else { 'build' }
    Push-Location $dir
    try {
        # Fresh package layout. Prod mirrors the release dir: build\{bin,frontend,config}.
        # Dev only needs build_dev\bin (Vite serves the frontend in dev).
        if (Test-Path $out) { Remove-Item -Recurse -Force $out }
        New-Item -ItemType Directory -Force -Path "$out\bin" | Out-Null
        if (-not $Dev) { New-Item -ItemType Directory -Force -Path "$out\frontend", "$out\config" | Out-Null }

        $inc = @('/I', 'src', '/I', 'dep', '/I', "$Root\capture\include", '/I', "$Root\common\include")
        $cflags = if ($Dev) {
            @('/nologo', '/EHsc', '/std:c++17') + $inc + @('/DDEV_MODE', '/Od', '/Zi', '/MT')
        }
        else {
            @('/nologo', '/EHsc', '/std:c++17') + $inc + @('/DNDEBUG', '/O2', '/GS-', '/Gy', '/Gw', '/MT')
        }
        $lflags = @('d3d11.lib', 'dxgi.lib', 'windowsapp.lib', 'user32.lib', 'gdi32.lib', 'ole32.lib',
            'oleaut32.lib', 'ws2_32.lib', 'windowscodecs.lib', 'dwmapi.lib', 'shell32.lib', 'shlwapi.lib',
            'winhttp.lib', 'bcrypt.lib')
        $linkflags = if ($Dev) { @('/DEBUG:FULL') } else { @('/OPT:REF', '/OPT:ICF') }
        $srcs = @('src\main.cpp', 'src\commands.cpp', 'src\virtual_desktop.cpp', 'src\paths.cpp', 'src\sha256_util.cpp')
        $libs = @('dep\WebView2LoaderStatic.lib', "$Root\logger\build\logger.lib") +
        (@('common', 'wgc', 'gdi', 'pw', 'screen', 'desktop') | ForEach-Object { "$Root\capture\build\capture_$_.lib" }) +
        (@('common', 'sendinput', 'winapi', 'postmessage', 'driver') | ForEach-Object { "$Root\input\build\input_$_.lib" })

        rc.exe /nologo /fo "$out\app.res" app.rc
        if ($LASTEXITCODE) { throw 'monitor_app: rc failed' }

        $clArgs = $cflags + @("/Fo$out\", "/Fe:$out\bin\monitor_app.exe") + $srcs + @("$out\app.res") +
        $libs + $lflags + @('/link') + $linkflags
        cl.exe @clArgs
        if ($LASTEXITCODE) { throw 'monitor_app: cl failed' }

        # Copy the 12 sibling DLLs next to the exe (needed at runtime, dev + prod).
        $dlls = @("$Root\logger\build\logger.dll") +
        (@('common', 'wgc', 'gdi', 'pw', 'screen', 'desktop') | ForEach-Object { "$Root\capture\build\capture_$_.dll" }) +
        (@('common', 'sendinput', 'winapi', 'postmessage', 'driver') | ForEach-Object { "$Root\input\build\input_$_.dll" })
        foreach ($d in $dlls) { Copy-Item $d "$out\bin\" -Force }

        if (-not $Dev) {
            # Prod package: updater(+.new) + frontend(dist) + config into the layout.
            $upd = Join-Path $Root 'updater\build\updater.exe'
            if (Test-Path $upd) {
                Copy-Item $upd "$out\bin\" -Force
                Copy-Item "$out\bin\updater.exe" "$out\bin\updater.new" -Force
            }
            $dist = Join-Path $Root 'monitor_web\dist'
            if (Test-Path $dist) { Copy-Item "$dist\*" "$out\frontend\" -Recurse -Force }
            $settings = Join-Path $Root 'config\settings.default.json'
            if (Test-Path $settings) { Copy-Item $settings "$out\config\" -Force }
        }
    }
    finally { Pop-Location }
    Write-Ok "monitor_app.exe ($cfg)"
}

# Dispatch — flat conditionals in dependency order (capture/input/monitor_app link
# against logger.lib etc). Explicit ifs, not foreach+switch.
$all = $Module -contains 'all'
if ($all -or $Module -contains 'logger')      { Build-Logger }
if ($all -or $Module -contains 'capture')     { Build-Capture }
if ($all -or $Module -contains 'input')       { Build-Input }
if ($all -or $Module -contains 'updater')     { Build-Updater }
if ($all -or $Module -contains 'monitor_app') { Build-MonitorApp }
