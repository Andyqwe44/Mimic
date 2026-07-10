/**
 * paths.cpp — Application path resolution.
 */
#include "paths.h"
#include "../../logger/logger.h"
#include <windows.h>
#include <shlobj.h>
#include <cstdio>
#include <cstdlib>
#include <direct.h>

static std::string g_exe_dir;
static std::string g_install_dir;
static std::string g_appdata_dir;

static std::string narrow(const wchar_t* ws) {
    int len = WideCharToMultiByte(CP_UTF8, 0, ws, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return "";
    std::string s(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws, -1, &s[0], len, nullptr, nullptr);
    return s;
}

static void ensure_dir(const std::string& path) {
    // Recursively create directories
    for (size_t i = 0; i < path.size(); i++) {
        if (path[i] == '\\' || path[i] == '/') {
            std::string part = path.substr(0, i);
            if (!part.empty()) CreateDirectoryA(part.c_str(), nullptr);
        }
    }
    CreateDirectoryA(path.c_str(), nullptr);
}

static void copy_file_if_missing(const std::string& src, const std::string& dst) {
    if (GetFileAttributesA(dst.c_str()) != INVALID_FILE_ATTRIBUTES) return; // exists
    if (!CopyFileA(src.c_str(), dst.c_str(), TRUE)) {
        // If source doesn't exist either, create empty JSON
        FILE* f = fopen(dst.c_str(), "wb");
        if (f) { fprintf(f, "{}\n"); fclose(f); }
    }
}

std::string paths_get_exe_dir() {
    if (!g_exe_dir.empty()) return g_exe_dir;

    char buf[MAX_PATH];
    GetModuleFileNameA(nullptr, buf, MAX_PATH);
    char* slash = strrchr(buf, '\\');
    if (slash) *slash = '\0';
    g_exe_dir = buf;
    return g_exe_dir;
}

std::string paths_get_install_dir() {
    if (!g_install_dir.empty()) return g_install_dir;

    // Try registry first (set by installer)
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
        "SOFTWARE\\GameAgentMonitor", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        char val[512];
        DWORD size = sizeof(val);
        if (RegQueryValueExA(hKey, "InstallPath", nullptr, nullptr,
            (LPBYTE)val, &size) == ERROR_SUCCESS) {
            val[size] = '\0';
            g_install_dir = val;
            RegCloseKey(hKey);
            return g_install_dir;
        }
        RegCloseKey(hKey);
    }

    // Fallback: parent of exe directory (development / portable mode)
    std::string exeDir = paths_get_exe_dir();
    char* slash = strrchr(&exeDir[0], '\\');
    if (slash) *slash = '\0';
    g_install_dir = exeDir;
    return g_install_dir;
}

std::string paths_get_appdata_dir() {
    if (!g_appdata_dir.empty()) return g_appdata_dir;

    wchar_t localAppData[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, localAppData))) {
        g_appdata_dir = narrow(localAppData) + "\\GameAgentMonitor";
    } else {
        // Last resort
        g_appdata_dir = paths_get_install_dir() + "\\data";
    }

    // Ensure directory tree exists
    ensure_dir(g_appdata_dir);
    ensure_dir(g_appdata_dir + "\\config");
    ensure_dir(g_appdata_dir + "\\log");
    ensure_dir(g_appdata_dir + "\\staging");

    return g_appdata_dir;
}

std::string paths_get_frontend_url() {
#ifdef DEV_MODE
    return "http://localhost:1420";
#else
    std::string html = paths_get_install_dir() + "\\frontend\\index.html";
    // Convert backslashes to forward slashes, prepend file:///
    std::string url = "file:///";
    for (char c : html) {
        if (c == '\\') url += '/';
        else url += c;
    }
    return url;
#endif
}

void paths_init() {
    // Ensure appdata directories exist
    paths_get_appdata_dir();

    // Copy default config if user config doesn't exist
    std::string defaultConfig = paths_get_install_dir() + "\\config\\settings.default.json";
    std::string userConfig = paths_get_appdata_dir() + "\\config\\settings.json";
    copy_file_if_missing(defaultConfig, userConfig);

    // Merge: add any new keys from default that are missing in user config
    // This is a simple key-by-key merge for now
    FILE* fd = fopen(defaultConfig.c_str(), "rb");
    FILE* fu = fopen(userConfig.c_str(), "rb+");
    if (fd && fu) {
        fseek(fd, 0, SEEK_END);
        long dsz = ftell(fd);
        fseek(fd, 0, SEEK_SET);
        std::string djson(dsz, '\0');
        fread(&djson[0], 1, dsz, fd);

        fseek(fu, 0, SEEK_END);
        long usz = ftell(fu);
        fseek(fu, 0, SEEK_SET);
        std::string ujson(usz, '\0');
        fread(&ujson[0], 1, usz, fu);

        // Simple merge: for each key in default not in user, append to user
        // This is best-effort; full JSON merge would need a parser
        // We just check if user config is basically empty or broken
        bool user_empty = ujson.size() < 5; // "{}" or less

        if (user_empty && dsz > 0) {
            // User config is empty — just copy default
            fclose(fu); fu = nullptr;
            FILE* fw = fopen(userConfig.c_str(), "wb");
            if (fw) { fwrite(djson.data(), 1, dsz, fw); fclose(fw); }
        } else if (!user_empty && !djson.empty()) {
            // User config has content — append missing keys from default
            // For now, write both side by side (JSON is forgiving of extra keys)
            // A proper merge would parse both objects
            LOG("main", "paths_init: user config exists (%ld bytes)", usz);
        }
    }
    if (fd) fclose(fd);
    if (fu) fclose(fu);

    LOG("main", "paths_init: install=%s appdata=%s",
        paths_get_install_dir().c_str(), paths_get_appdata_dir().c_str());
}
