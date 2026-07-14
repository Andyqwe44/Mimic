/**
 * paths.h — Resolve application paths (install dir, appdata, frontend).
 *
 * Layout:
 *   Install:  C:\Program Files\GameAgentMonitor\         (get_install_dir)
 *     bin\monitor_app.exe
 *     frontend\index.html
 *   AppData (compile-time /DDEV_MODE split — not a repo-relative path):
 *     Prod: %LOCALAPPDATA%\GameAgentMonitor\
 *     Dev:  %LOCALAPPDATA%\GameAgentMonitor_Dev\
 *       config\settings.json · log\ · staging\ · WebView2\
 */
#pragma once
#include <string>

/// EXE directory (e.g. C:\...\bin). Always reliable — derived from GetModuleFileName.
std::string paths_get_exe_dir();

/// Install root = parent of exe_dir (e.g. C:\Program Files\GameAgentMonitor).
/// Falls back to exe_dir/.. if registry key is missing.
std::string paths_get_install_dir();

/// User data directory under %LOCALAPPDATA% (GameAgentMonitor or GameAgentMonitor_Dev).
/// Creates the directory tree if it doesn't exist.
std::string paths_get_appdata_dir();

/// Frontend URL to navigate to.
/// Dev mode (#ifdef DEV_MODE): http://localhost:1420
/// Prod: file:///<install_dir>/frontend/index.html
std::string paths_get_frontend_url();

/// Initialize — create appdata directories, copy default config if needed.
void paths_init();
