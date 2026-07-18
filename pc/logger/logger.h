/**
 * logger.h — Unified logging engine (C API).
 *
 * FOUR log levels. Thread-safe. Auto-timestamp. Used by C++ + Rust (FFI).
 *
 * Level hierarchy (low→high):
 *   LOG_LEVEL_DEBUG(0)  developer details — frame timing, param dumps
 *   LOG_LEVEL_INFO (1)  normal messages — status changes, user actions
 *   LOG_LEVEL_WARN (2)  recoverable problems — fallback used, retry
 *   LOG_LEVEL_ERROR(3)  hard failures — operation failed, must fix
 *
 * Usage:
 *   capture_log_init("agent", APP_VERSION, "log/", 5, 5000);
 *   capture_log_set_level(LOG_LEVEL_DEBUG);   // dev
 *   capture_log_set_level(LOG_LEVEL_INFO);    // prod (default)
 *   LOG_ERROR("wgc", "init failed: hr=0x%08x", hr);
 *   LOG_WARN("cmd", "fallback to GDI for hwnd=%llu", hwnd);
 *   LOG("wgc", "capture OK: %dx%d", w, h);       // INFO
 *   LOG_DEBUG("wgc", "frame %d: %dx%d @ %dms", n, w, h, ms);
 */
#pragma once
#include <cstdio>

#ifdef GAM_BUILD_DLL
  #ifdef _WIN32
    #define GAM_API __declspec(dllexport)
  #else
    #define GAM_API
  #endif
#else
  #ifdef _WIN32
    #define GAM_API __declspec(dllimport)
  #else
    #define GAM_API
  #endif
#endif

// ── Log levels ────────────────────────────────────────────
#define LOG_LEVEL_DEBUG 0
#define LOG_LEVEL_INFO  1
#define LOG_LEVEL_WARN  2
#define LOG_LEVEL_ERROR 3

#ifdef __cplusplus
extern "C" {
#endif

/// Initialize logger: create log file, ring buffer, clean old files.
GAM_API void capture_log_init(const char* app_name, const char* app_version,
                      const char* log_dir, int max_files, int ring_size);

/// Shutdown: flush and close log file, free ring buffer.
GAM_API void capture_log_shutdown(void);

/// ═══ THE ONE (for explicit level) ═══
/// Write a message at a specific level. Entries below the current
/// threshold (set via capture_log_set_level) are silently dropped.
/// Timestamp auto-added: [HH:MM:SS.mmm]
GAM_API void capture_log_write_level(int level, const char* tag, const char* msg);

/// ═══ THE ONE (backward compat — writes at INFO level) ═══
/// Equivalent to capture_log_write_level(LOG_LEVEL_INFO, tag, msg).
GAM_API void capture_log_write_msg(const char* tag, const char* msg);

/// Set minimum log level. Entries below this are dropped.
/// Default: LOG_LEVEL_INFO (1). Call LOG_LEVEL_DEBUG(0) for dev builds.
GAM_API void capture_log_set_level(int level);

/// Return current log level.
GAM_API int capture_log_get_level(void);

/// Read in-memory ring buffer as newline-separated lines.
/// Returns malloc'd string; caller must free with capture_log_free().
GAM_API char* capture_log_read_memory(void);

/// List log files (newest first) as JSON array.
/// [{"name":"agent_20260707_133408.log","size":1234}]
GAM_API char* capture_log_list_files(int max_files);

/// Read a historical log file by name (relative to log_dir).
/// Returns malloc'd string with file contents (lines separated by \n).
/// Caller must free with capture_log_free().
GAM_API char* capture_log_read_file(const char* filename);

/// Free a string returned by the logger.
GAM_API void capture_log_free(char* s);

/// Flush the log file to disk.
GAM_API void capture_log_flush(void);

/// ── JSON notify callback (push C++ LOG entries to TS) ──
/// Called every time capture_log_write_level() writes an entry.
/// json = {"type":"log","ts":"...","tag":"...","msg":"...","level":"INFO","lvl":1,...}
/// Logger owns the wire format; caller just posts the string to WebView2.
/// NOT called for capture_log_write_ui() — TS already knows about its own entries.
typedef void (*capture_log_notify_json_cb)(const char* json);

/// Register a callback for real-time log push (C++ → TS).
GAM_API void capture_log_set_notify(capture_log_notify_json_cb cb);

/// Write a UI-side log entry with [ui] tag.
/// Same as LOG("ui", msg) but does NOT trigger notify callback
/// (avoids pushing back to TS what TS just sent).
GAM_API void capture_log_write_ui(const char* msg);

/// Return the absolute log directory path (set at init).
/// Returns "" if logger not initialized.
GAM_API const char* capture_log_get_dir(void);

/// @deprecated Use capture_log_set_level(LOG_LEVEL_DEBUG / LOG_LEVEL_INFO).
GAM_API void capture_log_set_debug(int enabled);

/// @deprecated Use LOG_DEBUG() macro — internally routes through levels.
GAM_API void capture_log_write_debug(const char* tag, const char* msg);

#ifdef __cplusplus
}
#endif

// ── C/C++ convenience macros ──────────────────────────────
#ifndef LOG_ERROR
  #define LOG_ERROR(tag, ...) do { \
      char _lbuf[2048]; \
      snprintf(_lbuf, sizeof(_lbuf), __VA_ARGS__); \
      capture_log_write_level(LOG_LEVEL_ERROR, tag, _lbuf); \
  } while(0)
#endif

#ifndef LOG_WARN
  #define LOG_WARN(tag, ...) do { \
      char _lbuf[2048]; \
      snprintf(_lbuf, sizeof(_lbuf), __VA_ARGS__); \
      capture_log_write_level(LOG_LEVEL_WARN, tag, _lbuf); \
  } while(0)
#endif

#ifndef LOG
  #define LOG(tag, ...) do { \
      char _lbuf[2048]; \
      snprintf(_lbuf, sizeof(_lbuf), __VA_ARGS__); \
      capture_log_write_level(LOG_LEVEL_INFO, tag, _lbuf); \
  } while(0)
#endif

// LOG_INFO is alias for LOG, kept for explicitness
#ifndef LOG_INFO
  #define LOG_INFO(tag, ...) LOG(tag, __VA_ARGS__)
#endif

#ifndef LOG_DEBUG
  #define LOG_DEBUG(tag, ...) do { \
      char _lbuf[2048]; \
      snprintf(_lbuf, sizeof(_lbuf), __VA_ARGS__); \
      capture_log_write_level(LOG_LEVEL_DEBUG, tag, _lbuf); \
  } while(0)
#endif
