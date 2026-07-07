/**
 * logger.cpp — Unified logging engine implementation.
 *
 * Thread-safe. Writes to rotating log files AND in-memory ring buffer.
 * Timestamps use QueryPerformanceCounter for high precision.
 */
#include "logger.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <ctime>
#include <mutex>
#include <vector>
#include <string>
#include <algorithm>
#define NOMINMAX
#include <windows.h>

// ── Ring buffer entry ──────────────────────────────────
struct LogEntry {
    std::string ts;   // "HH:MM:SS.mmm"
    std::string msg;  // full log line
};

// ── Global logger state ─────────────────────────────────
static std::mutex         g_mutex;
static FILE*              g_file = nullptr;
static std::string        g_log_dir;
static std::string        g_app_name;
static int                g_max_files = 5;
static std::vector<LogEntry> g_ring;
static int                g_ring_cap = 5000;
static int                g_ring_idx = 0;  // write position (circular)
static bool               g_ring_full = false;
static bool               g_initialized = false;

// ── High-precision timestamp ────────────────────────────
static std::string _timestamp() {
    LARGE_INTEGER freq, cnt;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&cnt);
    uint64_t us = (uint64_t)(cnt.QuadPart * 1'000'000 / freq.QuadPart);

    time_t sec = (time_t)(us / 1'000'000);
    struct tm tm;
    localtime_s(&tm, &sec);
    uint64_t ms = (us / 1000) % 1000;

    char buf[16];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03lld",
             tm.tm_hour, tm.tm_min, tm.tm_sec, (long long)ms);
    return buf;
}

// ── Internal: write to file (must hold g_mutex) ─────────
static void _write_file(const std::string& ts, const std::string& msg) {
    if (!g_file) return;
    fprintf(g_file, "[%s] %s\n", ts.c_str(), msg.c_str());
}

// ── Internal: write to ring buffer (must hold g_mutex) ──
static void _write_ring(const std::string& ts, const std::string& msg) {
    if (g_ring_cap <= 0) return;
    if ((int)g_ring.size() < g_ring_cap) {
        g_ring.push_back({ts, msg});
    } else {
        g_ring[g_ring_idx] = {ts, msg};
        g_ring_idx = (g_ring_idx + 1) % g_ring_cap;
        g_ring_full = true;
    }
}

// ── Internal: cleanup old log files ─────────────────────
static void _cleanup_old_logs() {
    std::string pattern = g_app_name + "_*.log";
    // Simple approach: list all matching files, sort by time, delete oldest
    WIN32_FIND_DATAW fd;
    std::wstring search_path;
    search_path.assign(g_log_dir.begin(), g_log_dir.end());
    if (!search_path.empty() && search_path.back() != '\\' && search_path.back() != '/')
        search_path += L'\\';
    search_path += L"*.log";

    HANDLE hFind = FindFirstFileW(search_path.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;

    struct FileInfo {
        std::wstring path;
        FILETIME ft;
    };
    std::vector<FileInfo> files;

    do {
        std::wstring fname = fd.cFileName;
        // Match: app_name_YYYYMMDD_HHMMSS.log
        std::string prefix_ansi(g_app_name.begin(), g_app_name.end());
        std::string fname_ansi(fname.begin(), fname.end());
        if (fname_ansi.find(prefix_ansi + "_") == 0 &&
            fname_ansi.find(".log") == fname_ansi.size() - 4) {
            std::wstring full_path;
            full_path.assign(g_log_dir.begin(), g_log_dir.end());
            if (!full_path.empty() && full_path.back() != '\\' && full_path.back() != '/')
                full_path += L'\\';
            full_path += fname;
            files.push_back({full_path, fd.ftLastWriteTime});
        }
    } while (FindNextFileW(hFind, &fd));
    FindClose(hFind);

    if ((int)files.size() <= g_max_files) return;

    // Sort by LastWriteTime, oldest first
    std::sort(files.begin(), files.end(), [](const FileInfo& a, const FileInfo& b) {
        return CompareFileTime(&a.ft, &b.ft) < 0;
    });

    // Delete oldest files
    int to_delete = (int)files.size() - g_max_files;
    for (int i = 0; i < to_delete; i++) {
        DeleteFileW(files[i].path.c_str());
    }
}

// ════════════════════════════════════════════════════════
// Public API
// ════════════════════════════════════════════════════════

void capture_log_init(const char* app_name,
                      const char* app_version,
                      const char* log_dir,
                      int max_files,
                      int ring_size) {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_initialized) return;

    g_app_name = app_name;
    g_log_dir = log_dir;
    g_max_files = max_files > 0 ? max_files : 5;
    g_ring_cap = ring_size > 0 ? ring_size : 5000;

    // Create log directory
    CreateDirectoryA(log_dir, nullptr);

    // Generate log filename with timestamp
    time_t now = time(nullptr);
    struct tm tm;
    localtime_s(&tm, &now);
    char fname[512];
    snprintf(fname, sizeof(fname), "%s/%s_%04d%02d%02d_%02d%02d%02d.log",
             log_dir, app_name,
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);

    g_file = fopen(fname, "a");
    if (g_file) {
        fprintf(g_file, "=== %s v%s ===\n", app_name, app_version);
        char ts_buf[32];
        strftime(ts_buf, sizeof(ts_buf), "%Y%m%d_%H%M%S", &tm);
        fprintf(g_file, "Session: %s | PID: %lu\n", ts_buf, (unsigned long)GetCurrentProcessId());
        fprintf(g_file, "Log: %s\n", fname);
        fflush(g_file);
    }

    // Write to ring buffer
    {
        auto ts = _timestamp();
        char buf[512];
        snprintf(buf, sizeof(buf), "=== %s v%s ===", app_name, app_version);
        _write_ring(ts, buf);
    }

    // Cleanup old log files
    _cleanup_old_logs();

    g_initialized = true;
}

void capture_log_shutdown(void) {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_file) {
        auto ts = _timestamp();
        fprintf(g_file, "[%s] Application shutdown\n", ts.c_str());
        fclose(g_file);
        g_file = nullptr;
    }
    g_initialized = false;
}

// ── THE ONE write function ───────────────────────────────
void capture_log_write_msg(const char* tag, const char* msg) {
    auto ts = _timestamp();
    char formatted[4096];
    snprintf(formatted, sizeof(formatted), "[%s] %s", tag, msg);
    std::lock_guard<std::mutex> lk(g_mutex);
    _write_file(ts, formatted);
    _write_ring(ts, formatted);
    fflush(g_file);
}

char* capture_log_read_memory(void) {
    std::lock_guard<std::mutex> lk(g_mutex);
    std::string out;

    int start, count;
    if (g_ring_full) {
        start = g_ring_idx;
        count = g_ring_cap;
    } else {
        start = 0;
        count = (int)g_ring.size();
    }

    for (int i = 0; i < count; i++) {
        const auto& e = g_ring[(start + i) % g_ring_cap];
        out += "[";
        out += e.ts;
        out += "] ";
        out += e.msg;
        out += "\n";
    }

    char* result = (char*)malloc(out.size() + 1);
    if (result) {
        memcpy(result, out.c_str(), out.size() + 1);
    }
    return result;
}

char* capture_log_list_files(int max_files) {
    std::lock_guard<std::mutex> lk(g_mutex);

    std::wstring search_path;
    search_path.assign(g_log_dir.begin(), g_log_dir.end());
    if (!search_path.empty() && search_path.back() != '\\' && search_path.back() != '/')
        search_path += L'\\';
    search_path += L"*.log";

    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(search_path.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) {
        char* empty = (char*)malloc(3);
        if (empty) { empty[0] = '['; empty[1] = ']'; empty[2] = 0; }
        return empty;
    }

    struct FileInfo {
        std::string name;
        FILETIME ft;
        size_t size;
    };
    std::vector<FileInfo> files;

    do {
        std::wstring fname = fd.cFileName;
        std::string fname_ansi(fname.begin(), fname.end());
        std::string prefix_ansi(g_app_name.begin(), g_app_name.end());
        if (fname_ansi.find(prefix_ansi + "_") == 0 && fname_ansi.find(".log") != std::string::npos) {
            ULARGE_INTEGER size;
            size.LowPart = fd.nFileSizeLow;
            size.HighPart = fd.nFileSizeHigh;
            files.push_back({fname_ansi, fd.ftLastWriteTime, (size_t)size.QuadPart});
        }
    } while (FindNextFileW(hFind, &fd));
    FindClose(hFind);

    // Sort newest first
    std::sort(files.begin(), files.end(), [](const FileInfo& a, const FileInfo& b) {
        return CompareFileTime(&a.ft, &b.ft) > 0;
    });

    // Build JSON
    std::string json = "[";
    int n = std::min((int)files.size(), max_files > 0 ? max_files : 5);
    for (int i = 0; i < n; i++) {
        if (i > 0) json += ",";
        char buf[512];
        snprintf(buf, sizeof(buf), R"({"name":"%s","size":%zu})",
                 files[i].name.c_str(), files[i].size);
        json += buf;
    }
    json += "]";

    char* result = (char*)malloc(json.size() + 1);
    if (result) memcpy(result, json.c_str(), json.size() + 1);
    return result;
}

void capture_log_free(char* s) {
    free(s);
}

void capture_log_flush(void) {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_file) fflush(g_file);
}
