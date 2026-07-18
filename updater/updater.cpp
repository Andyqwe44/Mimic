/**
 * updater.exe — Standalone update file replacer (requireAdministrator).
 *
 * Usage: updater.exe <staging_dir> <old_pid>
 *
 * Ownership split (post-0.3.37):
 *   - MimicClient (main) installs/replaces bin\updater.exe from staging (elevated).
 *   - This process updates everything EXCEPT bin\updater.exe, then relaunches the app.
 *
 * No updater.new / --self-install.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>

static void str_dirname(char* path) {
    char* slash = strrchr(path, '\\');
    if (slash) *slash = '\0';
}

static void ensure_parent_dir(const char* filePath) {
    char tmp[MAX_PATH];
    strncpy(tmp, filePath, MAX_PATH);
    tmp[MAX_PATH - 1] = '\0';
    for (char* p = tmp; *p; p++) {
        if (*p == '\\' || *p == '/') {
            char saved = *p;
            *p = '\0';
            CreateDirectoryA(tmp, nullptr);
            *p = saved;
        }
    }
    char* last = strrchr(tmp, '\\');
    if (last) {
        *last = '\0';
        CreateDirectoryA(tmp, nullptr);
    }
}

static char g_selfPath[MAX_PATH] = {};
static char g_logPath[MAX_PATH] = {};

static void ulog(const char* fmt, ...) {
    if (!g_logPath[0]) return;
    FILE* f = fopen(g_logPath, "a");
    if (!f) return;
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(f, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f);
    fclose(f);
}

static bool copy_file(const char* src, const char* dst) {
    ensure_parent_dir(dst);
    BOOL ok = CopyFileA(src, dst, FALSE);
    if (ok) ulog("  copied: %s", dst);
    else    ulog("  COPY FAIL: %s -> %s (err=%lu)", src, dst, (unsigned long)GetLastError());
    return ok != 0;
}

// Forward-slash relative path from staging root (e.g. "bin/updater.exe").
static bool is_skipped_rel(const char* relFwd) {
    if (_stricmp(relFwd, "bin/updater.exe") == 0) return true;  // main app owns updater
    if (_stricmp(relFwd, "bin/updater.new") == 0) return true;  // retired artifact
    return false;
}

static void to_fwd(char* s) {
    for (; *s; s++) if (*s == '\\') *s = '/';
}

// Walk staging → install. Skips updater.exe (installed by MimicClient before launch).
static int copy_staging(const char* stagingDir, const char* installDir, const char* relPrefix) {
    char searchPath[MAX_PATH];
    snprintf(searchPath, MAX_PATH, "%s\\*", stagingDir);

    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(searchPath, &fd);
    if (hFind == INVALID_HANDLE_VALUE) return 0;

    int count = 0;
    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;

        char srcFull[MAX_PATH], dstFull[MAX_PATH], rel[MAX_PATH];
        snprintf(srcFull, MAX_PATH, "%s\\%s", stagingDir, fd.cFileName);
        snprintf(dstFull, MAX_PATH, "%s\\%s", installDir, fd.cFileName);
        if (relPrefix[0])
            snprintf(rel, MAX_PATH, "%s/%s", relPrefix, fd.cFileName);
        else
            snprintf(rel, MAX_PATH, "%s", fd.cFileName);
        to_fwd(rel);

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            count += copy_staging(srcFull, dstFull, rel);
        } else if (is_skipped_rel(rel)) {
            ulog("  skip (main owns / retired): %s", rel);
        } else {
            if (copy_file(srcFull, dstFull)) count++;
        }
    } while (FindNextFileA(hFind, &fd));
    FindClose(hFind);
    return count;
}

static void remove_tree(const char* dir) {
    char searchPath[MAX_PATH];
    snprintf(searchPath, MAX_PATH, "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(searchPath, &fd);
    if (hFind == INVALID_HANDLE_VALUE) { RemoveDirectoryA(dir); return; }
    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
        char full[MAX_PATH];
        snprintf(full, MAX_PATH, "%s\\%s", dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) remove_tree(full);
        else DeleteFileA(full);
    } while (FindNextFileA(hFind, &fd));
    FindClose(hFind);
    RemoveDirectoryA(dir);
}

static int  g_expectedCount = 0;
static char g_expected[256][MAX_PATH];

static int manifest_paths(const char* json, char paths[][MAX_PATH], int maxPaths) {
    const char* p = strstr(json, "\"files\"");
    if (!p) return 0;
    p = strchr(p, '{');
    if (!p) return 0;
    int depth = 0, count = 0;
    for (; *p; p++) {
        if (*p == '{') depth++;
        else if (*p == '}') { depth--; if (depth == 0) break; }
        else if (depth == 1 && *p == '"') {
            const char* keyEnd = strchr(p + 1, '"');
            if (!keyEnd) break;
            int len = (int)(keyEnd - (p + 1));
            if (len > 0 && len < MAX_PATH && count < maxPaths) {
                memcpy(paths[count], p + 1, len);
                paths[count][len] = '\0';
                count++;
            }
            p = keyEnd;
        }
    }
    return count;
}

static bool path_in_expected(const char* rel) {
    for (int i = 0; i < g_expectedCount; i++)
        if (_stricmp(g_expected[i], rel) == 0) return true;
    return false;
}

static bool is_protected_rel(const char* rel) {
    size_t n = strlen(rel);
    if (n >= 4 && _stricmp(rel + n - 4, ".old") == 0) return true;
    static const char* prot[] = {
        "bin/updater.exe", "bin/updater.log", "bin/mimic_client.exe"
    };
    for (int i = 0; i < 3; i++) if (_stricmp(rel, prot[i]) == 0) return true;
    return false;
}

static int sync_delete_dir(const char* installDir, const char* subdir) {
    char searchPath[MAX_PATH];
    snprintf(searchPath, MAX_PATH, "%s\\%s\\*", installDir, subdir);
    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(searchPath, &fd);
    if (hFind == INVALID_HANDLE_VALUE) return 0;
    int removed = 0;
    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
        char rel[MAX_PATH];
        snprintf(rel, MAX_PATH, "%s/%s", subdir, fd.cFileName);
        to_fwd(rel);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            removed += sync_delete_dir(installDir, rel);
        } else if (!path_in_expected(rel) && !is_protected_rel(rel)) {
            char full[MAX_PATH];
            snprintf(full, MAX_PATH, "%s\\%s", installDir, rel);
            for (char* q = full; *q; q++) if (*q == '/') *q = '\\';
            if (DeleteFileA(full)) { ulog("  deleted stale: %s", rel); removed++; }
            else ulog("  DELETE FAIL: %s (err=%lu)", rel, (unsigned long)GetLastError());
        }
    } while (FindNextFileA(hFind, &fd));
    FindClose(hFind);
    return removed;
}

static void sync_deletions(const char* installDir) {
    char vjPath[MAX_PATH];
    snprintf(vjPath, MAX_PATH, "%s\\version.json", installDir);
    FILE* f = fopen(vjPath, "rb");
    if (!f) { ulog("sync_deletions: no install version.json -> skip"); return; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 1024 * 1024) { fclose(f); ulog("sync_deletions: manifest size bad -> skip"); return; }
    char* buf = (char*)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';
    g_expectedCount = manifest_paths(buf, g_expected, 256);
    free(buf);
    if (g_expectedCount <= 0) { ulog("sync_deletions: 0 expected paths -> skip"); return; }
    ulog("sync_deletions: %d expected files; scanning bin, frontend", g_expectedCount);
    int removed = sync_delete_dir(installDir, "bin") + sync_delete_dir(installDir, "frontend");
    // Retire leftover updater.new from older releases (not protected).
    char retired[MAX_PATH];
    snprintf(retired, MAX_PATH, "%s\\bin\\updater.new", installDir);
    if (DeleteFileA(retired)) {
        ulog("  deleted stale: bin/updater.new");
        removed++;
    }
    ulog("sync_deletions: removed %d stale files", removed);
}

// Parse lpCmdLine into argv-like tokens (handles quoted paths). 铁律 9b.
static int parse_args(char* cmdline, char* out[], int maxOut) {
    int n = 0;
    char* p = cmdline;
    while (*p && n < maxOut) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        if (*p == '"') {
            p++;
            out[n++] = p;
            while (*p && *p != '"') p++;
            if (*p == '"') *p++ = '\0';
        } else {
            out[n++] = p;
            while (*p && *p != ' ' && *p != '\t') p++;
            if (*p) *p++ = '\0';
        }
    }
    return n;
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR lpCmdLine, int) {
    GetModuleFileNameA(nullptr, g_selfPath, MAX_PATH);
    strncpy(g_logPath, g_selfPath, MAX_PATH - 1); g_logPath[MAX_PATH - 1] = '\0';
    str_dirname(g_logPath);
    strncat(g_logPath, "\\updater.log", MAX_PATH - strlen(g_logPath) - 1);
    ulog("=== updater start === self=%s", g_selfPath);
    ulog("cmdline: [%s]", lpCmdLine ? lpCmdLine : "(null)");

    // Drop leftover self-replace sidecar from pre-0.3.37 updaters.
    {
        char oldSelf[MAX_PATH];
        snprintf(oldSelf, MAX_PATH, "%s.old", g_selfPath);
        DeleteFileA(oldSelf);
    }

    char cmdBuf[1024] = {};
    if (lpCmdLine) {
        strncpy(cmdBuf, lpCmdLine, sizeof(cmdBuf) - 1);
    }
    char* argv[8] = {};
    int argc = parse_args(cmdBuf, argv, 8);

    char stagingDir[MAX_PATH] = {};
    DWORD oldPid = 0;
    if (argc >= 1) { strncpy(stagingDir, argv[0], MAX_PATH - 1); stagingDir[MAX_PATH - 1] = '\0'; }
    if (argc >= 2) { oldPid = (DWORD)strtoul(argv[1], nullptr, 10); }

    if (!stagingDir[0] || !oldPid) {
        ulog("ERROR: bad args (staging=%s pid=%lu)", stagingDir, (unsigned long)oldPid);
        MessageBoxA(nullptr, "Usage: updater.exe <staging_dir> <old_pid>", "Mimic Updater", MB_ICONERROR);
        return 1;
    }
    ulog("staging=%s  oldPid=%lu", stagingDir, (unsigned long)oldPid);

    HANDLE hProc = OpenProcess(SYNCHRONIZE, FALSE, oldPid);
    if (hProc) {
        ulog("waiting for pid %lu to exit (<=30s)...", (unsigned long)oldPid);
        if (WaitForSingleObject(hProc, 30000) == WAIT_TIMEOUT) {
            ulog("pid wait TIMEOUT -> terminating");
            TerminateProcess(hProc, 0);
        } else {
            ulog("pid exited");
        }
        CloseHandle(hProc);
    } else {
        ulog("OpenProcess(pid %lu) failed/absent -> proceeding (err=%lu)",
            (unsigned long)oldPid, (unsigned long)GetLastError());
    }
    Sleep(500);

    char installDir[MAX_PATH] = {};
    strncpy(installDir, g_selfPath, MAX_PATH - 1); installDir[MAX_PATH - 1] = '\0';
    str_dirname(installDir);  // <install>\bin
    str_dirname(installDir);  // <install>
    {
        char probe[MAX_PATH];
        snprintf(probe, MAX_PATH, "%s\\bin\\mimic_client.exe", installDir);
        if (GetFileAttributesA(probe) == INVALID_FILE_ATTRIBUTES) {
            snprintf(probe, MAX_PATH, "%s\\bin\\monitor_app.exe", installDir);
        }
        if (GetFileAttributesA(probe) == INVALID_FILE_ATTRIBUTES) {
            installDir[0] = '\0';
            HKEY hKey;
            const char* keys[] = { "SOFTWARE\\MimicClient", "SOFTWARE\\GameAgentMonitor" };
            for (int ki = 0; ki < 2 && !installDir[0]; ki++) {
                if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, keys[ki], 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
                    DWORD size = sizeof(installDir);
                    RegQueryValueExA(hKey, "InstallPath", nullptr, nullptr, (LPBYTE)installDir, &size);
                    RegCloseKey(hKey);
                }
            }
        }
    }

    if (!installDir[0]) {
        ulog("ERROR: cannot resolve install path");
        MessageBoxA(nullptr, "Cannot resolve install path.", "Mimic Updater", MB_ICONERROR);
        return 2;
    }
    ulog("install dir = %s", installDir);

    ulog("copying staging -> install (skip bin/updater.exe) ...");
    int copied = copy_staging(stagingDir, installDir, "");
    ulog("copied %d files total", copied);

    sync_deletions(installDir);

    char exePath[MAX_PATH];
    snprintf(exePath, MAX_PATH, "%s\\bin\\mimic_client.exe", installDir);
    ulog("launching %s", exePath);
    HINSTANCE h = ShellExecuteA(nullptr, "open", exePath, nullptr, installDir, SW_SHOW);
    ulog("launch result = %llu (>32 = OK)", (unsigned long long)(ULONG_PTR)h);

    remove_tree(stagingDir);
    ulog("=== updater done (copied %d files) ===", copied);
    return 0;
}
