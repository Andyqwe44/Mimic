/**
 * logger.h — Unified logging engine (C API).
 *
 * ONE write function: capture_log_write_msg(tag, msg).
 * C++ LOG() macro  → snprintf  → capture_log_write_msg  → file + ring buffer
 * Rust dlog!()     → format!() → capture_log_write_msg  → file + ring buffer
 *
 * Thread-safe. Auto-timestamp. Used by C++ + Rust (FFI) + standalone tools.
 *
 * Usage:
 *   capture_log_init("agent", APP_VERSION, "log/", 5, 5000);  // from monitor_app/src/version.h
 *   LOG("wgc", "init OK: %dx%d", w, h);           // C++
 *   dlog!("stream started for hwnd={}", hwnd);     // Rust → same pipe
 *   char* mem = capture_log_read_memory();
 *   capture_log_free(mem);
 *   capture_log_shutdown();
 */
#pragma once
#include <cstdio>

#ifdef __cplusplus
extern "C" {
#endif

/// Initialize logger: create log file, ring buffer, clean old files.
void capture_log_init(const char* app_name, const char* app_version,
                      const char* log_dir, int max_files, int ring_size);

/// Shutdown: flush and close log file, free ring buffer.
void capture_log_shutdown(void);

/// ═══ THE ONE ═══
/// Write a pre-formatted message. Timestamp auto-added: [HH:MM:SS.mmm]
/// Thread-safe. ALL log paths converge here — C++ LOG() macro, Rust dlog!() macro.
void capture_log_write_msg(const char* tag, const char* msg);

/// Read in-memory ring buffer as newline-separated lines.
/// Returns malloc'd string; caller must free with capture_log_free().
char* capture_log_read_memory(void);

/// List log files (newest first) as JSON array.
/// [{"name":"agent_20260707_133408.log","size":1234}]
char* capture_log_list_files(int max_files);

/// Read a historical log file by name (relative to log_dir).
/// Returns malloc'd string with file contents (lines separated by \n).
/// Caller must free with capture_log_free().
char* capture_log_read_file(const char* filename);

/// Free a string returned by the logger.
void capture_log_free(char* s);

/// Flush the log file to disk.
void capture_log_flush(void);

/// ── Notify callback (push C++ LOG entries to TS) ──
/// Called every time capture_log_write_msg() writes an entry.
/// ts=timestamp, tag=log tag, msg=formatted message body (without [tag] prefix).
/// NOT called for capture_log_write_ui() — TS already knows about its own entries.
typedef void (*capture_log_notify_cb)(const char* ts, const char* tag, const char* msg);

/// Register a callback for real-time log push (C++ → TS).
void capture_log_set_notify(capture_log_notify_cb cb);

/// Write a UI-side log entry with [ui] tag.
/// Same as LOG("ui", msg) but does NOT trigger notify callback
/// (avoids pushing back to TS what TS just sent).
void capture_log_write_ui(const char* msg);

#ifdef __cplusplus
}
#endif

// ── C/C++ convenience macro ──────────────────────────────
// Formats with snprintf → calls the ONE write function.
#ifndef LOG
  #define LOG(tag, ...) do { \
      char _lbuf[2048]; \
      snprintf(_lbuf, sizeof(_lbuf), __VA_ARGS__); \
      capture_log_write_msg(tag, _lbuf); \
  } while(0)
#endif
