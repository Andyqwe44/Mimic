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
#include <io.h>
#define NOMINMAX
#include <windows.h>

// ── Ring buffer entry ──────────────────────────────────
struct LogEntry {
    std::string ts;       // last occurrence timestamp (HH:MM:SS.mmm)
    std::string msg;      // full log line: "[tag] body"
    int count = 0;        // 0 = single entry; >0 = collapsed consecutive duplicates
    std::string firstTs;  // first occurrence timestamp (valid when count > 0)
};

// ── Global logger state ─────────────────────────────────
static std::mutex         g_mutex;
static FILE*              g_file = nullptr;
static std::string        g_log_dir;
static std::string        g_app_name;
static std::string        g_current_file;  // current session filename, excluded from history
static int                g_max_files = 5;
static std::vector<LogEntry> g_ring;
static int                g_ring_cap = 5000;
static int                g_ring_idx = 0;  // write position (circular)
static bool               g_ring_full = false;
static bool               g_initialized = false;
static int                g_last_ring_pos = -1;  // index of last ring entry (for in-place update)

// ── Consecutive-duplicate collapse tracking ──────────────
// Ring: check-then-update (no duplicate added, efficient)
// File:  write-then-collapse (write raw first for crash safety,
//        then seek back + overwrite with ×N + truncate)
static std::string        g_last_tag;
static std::string        g_last_msg_body;  // raw msg body (without [tag], for comparison)
static std::string        g_last_first_ts;  // timestamp of first occurrence in current run
static int                g_last_count = 0; // consecutive count (0 = no active run)
static long               g_last_file_pos = 0;  // file position where current run starts

// ── Local-time timestamp with milliseconds ──────────────
static std::string _timestamp() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char buf[16];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03d",
             st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return buf;
}

// ── Internal: write an entry to file (must hold g_mutex) ──
// Does NOT update g_last_file_pos — caller must set it before calling
// (for new runs: ftell before write; for collapse: unchanged from run start).
static void _file_write_entry(const char* ts, const char* tag, const char* msg) {
    if (!g_file) return;
    fprintf(g_file, "[%s] [%s] %s\n", ts, tag, msg);
    fflush(g_file);
}

// ── Internal: collapse file — overwrite current run with ×N ──
// Uses low-level I/O (not stdio) to avoid buffering/seek issues with fseek+fprintf.
// 1. fflush pending stdio data  2. _lseek to run start  3. _write collapsed line
// 4. _chsize_s truncate  5. fseek to resync stdio position.
// Must hold g_mutex.
static void _file_collapse(const char* firstTs, const char* lastTs,
                            const char* tag, const char* msg, int count) {
    if (!g_file || count <= 1) return;
    fflush(g_file);  // flush any pending stdio writes
    int fd = _fileno(g_file);
    char buf[4096];
    int len = snprintf(buf, sizeof(buf), "[%s → %s] [%s] %s ×%d\n",
                       firstTs, lastTs, tag, msg, count);
    if (len <= 0) return;
    _lseek(fd, g_last_file_pos, SEEK_SET);
    _write(fd, buf, len);
    _chsize_s(fd, (__int64)(g_last_file_pos + len));
    fseek(g_file, 0, SEEK_END);  // resync stdio
}

// ── Internal: write to ring buffer (must hold g_mutex) ──
// Returns the index where the entry was written (for later in-place collapse).
static int _write_ring(const std::string& ts, const std::string& msg) {
    if (g_ring_cap <= 0) return -1;
    int pos;
    if ((int)g_ring.size() < g_ring_cap) {
        g_ring.push_back({ts, msg, 0, ""});
        pos = (int)g_ring.size() - 1;
    } else {
        g_ring[g_ring_idx] = {ts, msg, 0, ""};
        pos = g_ring_idx;
        g_ring_idx = (g_ring_idx + 1) % g_ring_cap;
        g_ring_full = true;
    }
    g_last_ring_pos = pos;
    return pos;
}

// ── Internal: update last ring entry in-place (collapse) ──
// Must hold g_mutex. Only called when count >= 2 and g_last_ring_pos is valid.
static void _update_last_ring(const std::string& ts, int count, const std::string& firstTs) {
    if (g_last_ring_pos < 0) return;
    if (g_ring_full) {
        // Circular buffer: entry at g_last_ring_pos may have been overwritten
        // by a newer non-collapsed entry. Guard: only update if it's still the same run.
        auto& e = g_ring[g_last_ring_pos];
        if (e.count == count - 1 || e.count == 0) {
            e.ts = ts;
            e.count = count;
            if (count == 2) e.firstTs = firstTs;
        }
    } else {
        if (g_last_ring_pos < (int)g_ring.size()) {
            auto& e = g_ring[g_last_ring_pos];
            e.ts = ts;
            e.count = count;
            if (count == 2) e.firstTs = firstTs;
        }
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

    // Clear ring buffer — fresh start for new session
    g_ring.clear();
    g_ring_idx = 0;
    g_ring_full = false;

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

    // Store current session filename (just basename) so listing excludes it
    {
        const char* base = strrchr(fname, '/');
        if (!base) base = strrchr(fname, '\\');
        g_current_file = base ? (base + 1) : fname;
    }

    // Open in binary read+write mode (not append).
    // Binary mode → ftell/fseek return accurate byte offsets (needed for collapse).
    // Read+write → we can seek back + overwrite + truncate for ×N collapse.
    g_file = fopen(fname, "w+b");
    if (g_file) {
        fprintf(g_file, "=== %s v%s ===\n", app_name, app_version);
        char ts_buf[32];
        strftime(ts_buf, sizeof(ts_buf), "%Y%m%d_%H%M%S", &tm);
        fprintf(g_file, "Session: %s | PID: %lu\n", ts_buf, (unsigned long)GetCurrentProcessId());
        fprintf(g_file, "Log: %s\n", fname);
        fflush(g_file);
    }

    // Write session header to ring buffer (matches file content)
    {
        auto ts = _timestamp();
        _write_ring(ts, "=== agent v0.3.0 ===");
        char info[256];
        snprintf(info, sizeof(info), "Session: %04d%02d%02d_%02d%02d%02d | PID: %lu",
                 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                 tm.tm_hour, tm.tm_min, tm.tm_sec,
                 (unsigned long)GetCurrentProcessId());
        _write_ring(ts, info);
        _write_ring(ts, fname);
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
        fflush(g_file);
        fclose(g_file);
        g_file = nullptr;
    }
    g_initialized = false;
    g_last_count = 0;
}

// ── Notify callback (C++ → TS push) ──────────────────────
static capture_log_notify_json_cb g_notify_cb = nullptr;

// ── Minimal JSON string escape (escapes " and \) ──────────
static std::string _json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else out += c;
    }
    return out;
}

// ── Build notify JSON message (must NOT hold g_mutex) ─────
static std::string _build_notify_json(const char* ts, const char* tag, const char* msg,
                                       int count, const char* firstTs) {
    std::string json;
    json.reserve(512);
    json = "{\"type\":\"log\",\"ts\":\"" + _json_escape(ts)
         + "\",\"tag\":\"" + _json_escape(tag)
         + "\",\"msg\":\"" + _json_escape(msg) + "\"";
    if (count > 1) {
        json += ",\"count\":" + std::to_string(count)
             +  ",\"firstTs\":\"" + _json_escape(firstTs) + "\"";
    }
    json += "}";
    return json;
}

// ── THE ONE write function ───────────────────────────────
// Write-then-collapse strategy for crash safety:
// 1. Always append the raw entry to file FIRST (durable on disk).
// 2. If same as previous: seek back + overwrite with [first→last] msg ×N + truncate.
// 3. If crash before step 2: file has individual entries — redundant but truthful.
//    If crash after step 2: file has clean collapsed entry — optimal.
// Ring buffer uses check-then-update (no temporary duplicate needed — it's in-memory).
void capture_log_write_msg(const char* tag, const char* msg) {
    auto ts = _timestamp();

    capture_log_notify_json_cb cb_copy = nullptr;
    std::string notify_json;
    {
        std::lock_guard<std::mutex> lk(g_mutex);

        bool same = (g_last_count > 0 && g_last_tag == tag && g_last_msg_body == msg);

        // Record file position BEFORE writing only when starting a new run.
        // For collapse: g_last_file_pos stays at run's first entry position.
        // Guard: g_file may be nullptr before capture_log_init (early LOG calls).
        if (!same && g_file) {
            g_last_file_pos = ftell(g_file);
        }
        _file_write_entry(ts.c_str(), tag, msg);

        if (same) {
            // ── Step 2: collapse — overwrite from run start ──
            g_last_count++;
            _file_collapse(g_last_first_ts.c_str(), ts.c_str(),
                           tag, msg, g_last_count);
            // Ring: update last entry in-place (no duplicate added)
            _update_last_ring(ts, g_last_count, g_last_first_ts);
        } else {
            // ── Different message: start new run ──
            g_last_tag = tag;
            g_last_msg_body = msg;
            g_last_first_ts = ts;
            g_last_count = 1;
            // Ring: push new entry
            char formatted[4096];
            snprintf(formatted, sizeof(formatted), "[%s] %s", tag, msg);
            _write_ring(ts, formatted);
        }

        // Build notify JSON inside lock (uses g_last_count, g_last_first_ts)
        notify_json = _build_notify_json(ts.c_str(), tag, msg,
                                          g_last_count,
                                          g_last_count > 1 ? g_last_first_ts.c_str() : "");
        cb_copy = g_notify_cb;
    }
    // Notify TS outside lock to avoid re-entrancy deadlock.
    if (cb_copy) {
        cb_copy(notify_json.c_str());
    }
}

void capture_log_set_notify(capture_log_notify_json_cb cb) {
    g_notify_cb = cb;
}

// ── UI-side log (TS → C++, no echo back) ─────────────────
// Same write-then-collapse strategy as capture_log_write_msg.
// No notify — TS already knows about its own log entries.
void capture_log_write_ui(const char* msg) {
    auto ts = _timestamp();
    {
        std::lock_guard<std::mutex> lk(g_mutex);

        bool same = (g_last_count > 0 && g_last_tag == "ui" && g_last_msg_body == msg);

        // Step 1: write raw entry to file (crash-safe)
        // Record file position BEFORE writing only for new run.
        if (!same && g_file) {
            g_last_file_pos = ftell(g_file);
        }
        _file_write_entry(ts.c_str(), "ui", msg);

        if (same) {
            // Step 2: collapse — overwrite from run start
            g_last_count++;
            _file_collapse(g_last_first_ts.c_str(), ts.c_str(),
                           "ui", msg, g_last_count);
            _update_last_ring(ts, g_last_count, g_last_first_ts);
        } else {
            g_last_tag = "ui";
            g_last_msg_body = msg;
            g_last_first_ts = ts;
            g_last_count = 1;
            char formatted[4096];
            snprintf(formatted, sizeof(formatted), "[ui] %s", msg);
            _write_ring(ts, formatted);
        }
    }
    // No notify — TS already knows about its own log entries
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
        if (e.count > 0) {
            // Collapsed entry: [firstTs → lastTs] msg ×N
            out += "[";
            out += e.firstTs;
            out += " → ";
            out += e.ts;
            out += "] ";
            out += e.msg;
            out += " ×";
            out += std::to_string(e.count);
            out += "\n";
        } else {
            // Normal entry: [ts] msg
            out += "[";
            out += e.ts;
            out += "] ";
            out += e.msg;
            out += "\n";
        }
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

    // Build JSON — include current session file (matches what Explorer shows)
    std::string json = "[";
    int taken = 0;
    int limit = max_files > 0 ? max_files : 5;
    for (size_t i = 0; i < files.size() && taken < limit; i++) {
        if (taken > 0) json += ",";
        char buf[512];
        snprintf(buf, sizeof(buf), R"({"name":"%s","size":%zu})",
                 files[i].name.c_str(), files[i].size);
        json += buf;
        taken++;
    }
    json += "]";

    char* result = (char*)malloc(json.size() + 1);
    if (result) memcpy(result, json.c_str(), json.size() + 1);
    return result;
}

char* capture_log_read_file(const char* filename) {
    std::lock_guard<std::mutex> lk(g_mutex);
    std::string path = g_log_dir;
    if (!path.empty() && path.back() != '\\' && path.back() != '/')
        path += '\\';
    path += filename;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        char* empty = (char*)malloc(1);
        if (empty) empty[0] = 0;
        return empty;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* buf = (char*)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return nullptr; }
    if (sz > 0) fread(buf, 1, (size_t)sz, f);
    buf[sz] = 0;
    fclose(f);
    return buf;
}

void capture_log_free(char* s) {
    free(s);
}

void capture_log_flush(void) {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_file) fflush(g_file);
}

const char* capture_log_get_dir(void) {
    return g_log_dir.c_str();
}
