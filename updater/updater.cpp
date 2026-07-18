/**
 * updater.exe — Standalone update file replacer.
 *
 * Usage: updater.exe <staging_dir> <old_pid>
 *
 * 1. Wait for <old_pid> to exit (max 30s)
 * 2. Read InstallPath from HKLM\SOFTWARE\GameAgentMonitor
 * 3. Copy all files from staging_dir/ to install_path/
 * 4. Update install_path/version.json
 * 5. Launch install_path/bin/mimic_client.exe
 * 6. Clean up staging_dir
 *
 * Minimal CRT: /MT /GS- /O2 /NODEFAULTLIB with kernel32 + shell32 + advapi32 only.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>

// Minimal string helpers (no STL to keep binary small)
static void str_path_join(char* dst, size_t dstSize, const char* a, const char* b) {
    int n = snprintf(dst, dstSize, "%s\\%s", a, b);
    if (n < 0) dst[0] = '\0';
}

static void str_dirname(char* path) {
    char* slash = strrchr(path, '\\');
    if (slash) *slash = '\0';
}

// Recursively create directories for a full file path
static void ensure_parent_dir(const char* filePath) {
    char tmp[MAX_PATH];
    strncpy(tmp, filePath, MAX_PATH);
    tmp[MAX_PATH-1] = '\0';
    for (char* p = tmp; *p; p++) {
        if (*p == '\\' || *p == '/') {
            char saved = *p;
            *p = '\0';
            CreateDirectoryA(tmp, nullptr);
            *p = saved;
        }
    }
    // CreateDirectoryA for the full path minus filename
    char* last = strrchr(tmp, '\\');
    if (last) {
        *last = '\0';
        CreateDirectoryA(tmp, nullptr);
    }
}

// Full path of THIS running updater image; set at the top of WinMain.
static char g_selfPath[MAX_PATH] = {};
// updater.log path (<install>\bin\updater.log); set at the top of WinMain so
// every step — including failures — is recorded for the update-test panel.
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

// Copy file: src → dst (create parent dirs as needed).
// Self-replace guard: Windows won't let CopyFileA overwrite the running .exe.
// If dst is our own image, rename ourselves aside first (renaming a running exe
// IS allowed), so the copy then lands cleanly. The stale .old is deleted on the
// next run. This lets updater.exe update itself (0.3.5+ chain).
static bool copy_file(const char* src, const char* dst) {
    ensure_parent_dir(dst);
    if (g_selfPath[0] && _stricmp(dst, g_selfPath) == 0) {
        char oldPath[MAX_PATH];
        snprintf(oldPath, MAX_PATH, "%s.old", dst);
        DeleteFileA(oldPath);
        MoveFileExA(dst, oldPath, MOVEFILE_REPLACE_EXISTING);
    }
    BOOL ok = CopyFileA(src, dst, FALSE);
    if (ok) ulog("  copied: %s", dst);
    else    ulog("  COPY FAIL: %s -> %s (err=%lu)", src, dst, (unsigned long)GetLastError());
    return ok != 0;
}

// Walk staging_dir recursively; copy every file to install_dir, preserving relative paths.
// Returns number of files copied.
static int copy_staging(const char* stagingDir, const char* installDir) {
    char searchPath[MAX_PATH];
    snprintf(searchPath, MAX_PATH, "%s\\*", stagingDir);

    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(searchPath, &fd);
    if (hFind == INVALID_HANDLE_VALUE) return 0;

    int count = 0;
    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;

        char srcFull[MAX_PATH], dstFull[MAX_PATH];
        snprintf(srcFull, MAX_PATH, "%s\\%s", stagingDir, fd.cFileName);
        snprintf(dstFull, MAX_PATH, "%s\\%s", installDir, fd.cFileName);

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            count += copy_staging(srcFull, dstFull);
        } else {
            if (copy_file(srcFull, dstFull)) count++;
        }
    } while (FindNextFileA(hFind, &fd));
    FindClose(hFind);
    return count;
}

// Delete a directory tree recursively
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
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            remove_tree(full);
        } else {
            DeleteFileA(full);
        }
    } while (FindNextFileA(hFind, &fd));
    FindClose(hFind);
    RemoveDirectoryA(dir);
}

// ── Desired-state deletion sync (P1b) ───────────────────────────────────────
// After copy_staging (which also lands the fresh version.json), delete install
// files no longer present in the manifest — so removed/renamed files (e.g. the
// front-end's hashed index-XXXX.js) don't accumulate forever. Strictly bounded:
// whitelisted dirs only, protected files never touched, no-op on an empty manifest.

static int  g_expectedCount = 0;
static char g_expected[256][MAX_PATH];   // manifest file paths (forward slashes)

// Extract each file path (the keys of the "files" object) from a version.json.
// Depth-1 quoted keys are paths; value objects sit at depth 2 (auto-skipped).
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
            p = keyEnd;   // value object's braces handled by depth counting
        }
    }
    return count;
}

static bool path_in_expected(const char* rel) {   // rel: forward slashes
    for (int i = 0; i < g_expectedCount; i++)
        if (_stricmp(g_expected[i], rel) == 0) return true;
    return false;
}

// Never delete the updater's own files, the main exe, or any *.old sidecar.
static bool is_protected_rel(const char* rel) {
    size_t n = strlen(rel);
    if (n >= 4 && _stricmp(rel + n - 4, ".old") == 0) return true;
    static const char* prot[] = {
        "bin/updater.exe", "bin/updater.new", "bin/updater.log", "bin/mimic_client.exe"
    };
    for (int i = 0; i < 4; i++) if (_stricmp(rel, prot[i]) == 0) return true;
    return false;
}

// Recursively scan installDir\<subdir>; delete files whose relative path
// (forward-slash, from installDir) is neither expected nor protected.
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
        snprintf(rel, MAX_PATH, "%s/%s", subdir, fd.cFileName);   // forward slash
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            removed += sync_delete_dir(installDir, rel);
        } else if (!path_in_expected(rel) && !is_protected_rel(rel)) {
            char full[MAX_PATH];
            snprintf(full, MAX_PATH, "%s\\%s", installDir, rel);
            if (DeleteFileA(full)) { ulog("  deleted stale: %s", rel); removed++; }
            else ulog("  DELETE FAIL: %s (err=%lu)", rel, (unsigned long)GetLastError());
        }
    } while (FindNextFileA(hFind, &fd));
    FindClose(hFind);
    return removed;
}

// Load {install}\version.json (just copied from staging) and prune stale files.
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
    // NEVER treat an empty/unparsable manifest as "delete everything".
    if (g_expectedCount <= 0) { ulog("sync_deletions: 0 expected paths -> skip (never delete-all)"); return; }
    ulog("sync_deletions: %d expected files; scanning bin, frontend", g_expectedCount);
    int removed = sync_delete_dir(installDir, "bin") + sync_delete_dir(installDir, "frontend");
    ulog("sync_deletions: removed %d stale files", removed);
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR lpCmdLine, int) {
    // Full path of this running updater image.
    GetModuleFileNameA(nullptr, g_selfPath, MAX_PATH);
    // updater.log next to this exe (<install>\bin\updater.log) — written from the
    // very start so any failure (denied copy, occupied exe, missing install) is
    // captured for the update-test panel.
    strncpy(g_logPath, g_selfPath, MAX_PATH-1); g_logPath[MAX_PATH-1] = '\0';
    str_dirname(g_logPath);
    strncat(g_logPath, "\\updater.log", MAX_PATH - strlen(g_logPath) - 1);
    ulog("=== updater start === self=%s", g_selfPath);
    ulog("cmdline: [%s]", lpCmdLine ? lpCmdLine : "(null)");

    // Best-effort cleanup of a leftover .old from a previous self-replace.
    {
        char oldSelf[MAX_PATH];
        snprintf(oldSelf, MAX_PATH, "%s.old", g_selfPath);
        DeleteFileA(oldSelf);
    }

    // --self-install: we are bin\updater.new, launched by monitor_app to replace a
    // stale bin\updater.exe (breaks the pre-0.3.5 self-replace deadlock: an old
    // updater could never overwrite its own running image). updater.exe is NOT
    // running now, so copy ourselves straight over it, then exit. No pid wait,
    // no staging copy, no relaunch (monitor_app is already running).
    if (strstr(lpCmdLine, "--self-install")) {
        char dir[MAX_PATH];
        strncpy(dir, g_selfPath, MAX_PATH); dir[MAX_PATH-1] = '\0';
        str_dirname(dir);                                    // → install\bin
        char target[MAX_PATH];
        snprintf(target, MAX_PATH, "%s\\updater.exe", dir);
        CopyFileA(g_selfPath, target, FALSE);                // overwrite stale updater.exe
        MoveFileExA(g_selfPath, nullptr, MOVEFILE_DELAY_UNTIL_REBOOT); // drop updater.new on reboot
        return 0;
    }

    // Strip surrounding double quotes from a token (defensive — the caller
    // must not embed quotes, but we guard anyway, 铁律 9b).
    auto unquote = [](char* s) {
        size_t len = strlen(s);
        if (len >= 2 && s[0] == '"' && s[len-1] == '"') {
            memmove(s, s + 1, len - 2);
            s[len - 2] = '\0';
        }
    };

    // Parse args
    char stagingDir[MAX_PATH] = {};
    DWORD oldPid = 0;

    char* tok = strtok(lpCmdLine, " ");
    if (tok) { unquote(tok); strncpy(stagingDir, tok, MAX_PATH-1); stagingDir[MAX_PATH-1] = '\0'; }
    tok = strtok(nullptr, " ");
    if (tok) { unquote(tok); oldPid = (DWORD)strtoul(tok, nullptr, 10); }

    if (!stagingDir[0] || !oldPid) {
        ulog("ERROR: bad args (staging=%s pid=%lu)", stagingDir, (unsigned long)oldPid);
        MessageBoxA(nullptr, "Usage: updater.exe <staging_dir> <old_pid>", "GAM Updater", MB_ICONERROR);
        return 1;
    }
    ulog("staging=%s  oldPid=%lu", stagingDir, (unsigned long)oldPid);

    // 1. Wait for old process to exit
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

    // Give the old process a moment to fully release file handles
    Sleep(500);

    // 2. Resolve install dir — prefer exe-relative (this updater lives in
    //    <install>\bin\updater.exe, so install = parent of bin), matching
    //    monitor_app's paths_get_install_dir. Robust to a missing/stale registry
    //    entry; fall back to the registry InstallPath only if exe-relative looks wrong.
    char installDir[MAX_PATH] = {};
    strncpy(installDir, g_selfPath, MAX_PATH-1); installDir[MAX_PATH-1] = '\0';
    str_dirname(installDir);  // <install>\bin
    str_dirname(installDir);  // <install>
    {
        char probe[MAX_PATH];
        snprintf(probe, MAX_PATH, "%s\\bin\\mimic_client.exe", installDir);
        if (GetFileAttributesA(probe) == INVALID_FILE_ATTRIBUTES) {
            // Pre-rename installs still ship monitor_app.exe
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
        MessageBoxA(nullptr, "Cannot resolve install path.", "GAM Updater", MB_ICONERROR);
        return 2;
    }
    ulog("install dir = %s", installDir);

    // 3. Copy staging files to install dir
    ulog("copying staging -> install ...");
    int copied = copy_staging(stagingDir, installDir);
    ulog("copied %d files total", copied);

    // Desired-state sync: prune install files no longer in the (freshly-copied)
    // manifest. Whitelisted dirs only, protected files kept, no-op on empty manifest.
    sync_deletions(installDir);

    char msg[256];
    snprintf(msg, sizeof(msg), "Update complete: %d files replaced.\nRestarting...", copied);

    // 4. Launch new EXE
    char exePath[MAX_PATH];
    snprintf(exePath, MAX_PATH, "%s\\bin\\mimic_client.exe", installDir);
    ulog("launching %s", exePath);
    HINSTANCE h = ShellExecuteA(nullptr, "open", exePath, nullptr, installDir, SW_SHOW);
    ulog("launch result = %llu (>32 = OK)", (unsigned long long)(ULONG_PTR)h);

    // 5. Clean up staging
    remove_tree(stagingDir);
    ulog("=== updater done (copied %d files) ===", copied);

    return 0;
}
