/**
 * version.h — Single canonical version for the entire project.
 *
 * C++:  #include "version.h" → use APP_VERSION
 * JS:   hostCall('get_version') → returns APP_VERSION at runtime
 *
 * UPDATE THIS ONE FILE when bumping the version.
 */
#pragma once
#define APP_VERSION "0.3.18"

// Comma form for Win32 VERSIONINFO (app.rc FILEVERSION/PRODUCTVERSION).
// Keep in sync with APP_VERSION above.
#define APP_VERSION_RC 0,3,18,0
