/**
 * paths.h — Resolve application paths (install dir, appdata, frontend).
 *
 * Layout:
 *   Install:  C:\Program Files\MimicClient\
 *     bin\mimic_client.exe
 *     frontend\index.html
 *   AppData: %LOCALAPPDATA%\MimicClient\
 *     config\settings.json · log\ · staging\ · WebView2\
 */
#pragma once
#include <string>

/// EXE directory (e.g. C:\...\bin). Always reliable — derived from GetModuleFileName.
std::string paths_get_exe_dir();

/// Install root = parent of exe_dir (e.g. C:\Program Files\MimicClient).
std::string paths_get_install_dir();

/// User data directory under %LOCALAPPDATA%\MimicClient.
std::string paths_get_appdata_dir();

/// Frontend URL (file:///…/frontend/index.html). Prod navigation uses gam.local mapping.
std::string paths_get_frontend_url();

/// Initialize — create appdata directories, copy default config if needed.
void paths_init();
