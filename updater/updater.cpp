/**
 * updater.exe — Standalone update file replacer.
 *
 * Usage: updater.exe <staging_dir> <old_pid>
 *
 * 1. Wait for <old_pid> to exit (max 30s)
 * 2. Read InstallPath from HKLM\SOFTWARE\GameAgentMonitor
 * 3. Copy all files from staging_dir/ to install_path/
 * 4. Update install_path/version.json
 * 5. Launch install_path/bin/monitor_app.exe
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
    return CopyFileA(src, dst, FALSE) != 0;
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

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR lpCmdLine, int) {
    // Full path of this running updater image.
    GetModuleFileNameA(nullptr, g_selfPath, MAX_PATH);

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

    // Parse args
    char stagingDir[MAX_PATH] = {};
    DWORD oldPid = 0;

    char* tok = strtok(lpCmdLine, " ");
    if (tok) { strncpy(stagingDir, tok, MAX_PATH-1); stagingDir[MAX_PATH-1] = '\0'; }
    tok = strtok(nullptr, " ");
    if (tok) oldPid = (DWORD)strtoul(tok, nullptr, 10);

    if (!stagingDir[0] || !oldPid) {
        MessageBoxA(nullptr, "Usage: updater.exe <staging_dir> <old_pid>", "GAM Updater", MB_ICONERROR);
        return 1;
    }

    // 1. Wait for old process to exit
    HANDLE hProc = OpenProcess(SYNCHRONIZE, FALSE, oldPid);
    if (hProc) {
        if (WaitForSingleObject(hProc, 30000) == WAIT_TIMEOUT) {
            TerminateProcess(hProc, 0);
        }
        CloseHandle(hProc);
    }

    // Give the old process a moment to fully release file handles
    Sleep(500);

    // 2. Read install path from registry
    char installDir[MAX_PATH] = {};
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
        "SOFTWARE\\GameAgentMonitor", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD size = sizeof(installDir);
        RegQueryValueExA(hKey, "InstallPath", nullptr, nullptr, (LPBYTE)installDir, &size);
        RegCloseKey(hKey);
    }

    if (!installDir[0]) {
        MessageBoxA(nullptr, "Cannot find install path in registry.", "GAM Updater", MB_ICONERROR);
        return 2;
    }

    // 3. Copy staging files to install dir
    int copied = copy_staging(stagingDir, installDir);

    char msg[256];
    snprintf(msg, sizeof(msg), "Update complete: %d files replaced.\nRestarting...", copied);

    // 4. Launch new EXE
    char exePath[MAX_PATH];
    snprintf(exePath, MAX_PATH, "%s\\bin\\monitor_app.exe", installDir);
    ShellExecuteA(nullptr, "open", exePath, nullptr, installDir, SW_SHOW);

    // 5. Clean up staging
    remove_tree(stagingDir);

    return 0;
}
