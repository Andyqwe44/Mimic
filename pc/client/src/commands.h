/**
 * commands.h — Backend command dispatch (replaces Tauri invoke).
 *
 * Each command receives parsed JSON args and returns a JSON result string.
 * Thread-safe for stream commands (start/stop from UI thread).
 */
#pragma once
#include <string>
#include <windows.h>  // UINT, WM_USER
#include <objbase.h>  // IStream

// Fwd declarations (avoid pulling in full WebView2.h in header)
struct ICoreWebView2Environment12;
struct ICoreWebView2_17;
struct IGlobalInterfaceTable;

/// Dispatch a WebMessage JSON command. Returns JSON response (or empty if fire-and-forget).
std::string dispatch_command(const std::string& json);

/// Push a BGRA frame to the WebView2 frontend via SharedBuffer (zero-copy).
/// Called from stream thread and single-frame capture.
/// Optional env12/wv17 for cross-thread use (stream thread provides its own).
void shared_buffer_push_frame(const uint8_t* bgra, int w, int h,
    ICoreWebView2Environment12* env12 = nullptr,
    ICoreWebView2_17* wv17 = nullptr);

/// Get GIT cookies for cross-thread WebView2 interface access.
/// Stream thread retrieves interfaces via GIT (IGlobalInterfaceTable).
void shared_buffer_marshal_for_stream(DWORD* out_env_cookie, DWORD* out_wv_cookie);
IGlobalInterfaceTable* shared_buffer_get_git();

/// Stream thread → main thread frame bridge.
/// Copies frame data, posts WM_STREAM_FRAME to main window.
/// Main WndProc calls shared_buffer_push_frame on STA thread.
void stream_bridge_push_frame(const uint8_t* bgra, int w, int h);

/// Accessor for main window HWND (used by get_self_rect command).
void* get_main_hwnd();

/// Raw settings JSON object for boot injection (never empty — "{}" on miss/corrupt).
/// Used by main.cpp AddScriptToExecuteOnDocumentCreated before first paint.
std::string settings_get_boot_json();

/// Initialize backend subsystems (logger, COM, WGC apartment).
void backend_init();

/// Shutdown backend (stop streams, flush logger).
void backend_shutdown();

// ── Auto-update download progress bridge ──────────────────────
// The background download thread updates a shared struct and posts
// WM_UPDATE_PROGRESS to the main window; WndProc reads it on the STA thread and
// pushes a {"type":"update_progress",...} message to JS. (Mirrors WM_STREAM_FRAME.)
static constexpr UINT WM_UPDATE_PROGRESS = WM_USER + 101;

// Posted when the frontend sends 'show_window' (its first frame is painted).
// WndProc reveals the main window, which was kept hidden through the WebView2
// startup gap to avoid a white flash. See app_post_show_window (main.cpp).
static constexpr UINT WM_APP_SHOW_WINDOW = WM_USER + 102;

// Self-test TCP reader thread → main STA thread → PostJsonToWebView.
// Avoids calling WebView2 PostWebMessageAsJson off the UI thread (hello was
// silently dropped, causing "no geometry" on self-test start).
static constexpr UINT WM_SELFTEST_EVENT = WM_USER + 103;

/// Drain queued test_target JSON lines to the WebView (main thread only).
void selftest_drain_to_webview();

struct UpdateProgress {
    bool active = false;       // download thread running
    bool succeeded = false;    // all files done OK
    bool failed = false;       // a file failed (download or sha256 mismatch)
    int current_file = 0;      // 1-based index currently downloading
    int total_files = 0;
    int skipped_files = 0;     // files already in staging (resume)
    std::string file_path;     // current file path
    unsigned long long done_bytes = 0;
    unsigned long long total_bytes = 0;
    unsigned long long skipped_bytes = 0;  // bytes already staged (resume)
    std::string error_file;    // first failed file
    std::string staging_dir;   // where files were written (for updater launch)
};

/// Thread-safe snapshot of current download progress (called by WndProc, STA).
UpdateProgress update_get_progress();

/// Launch updater.exe for the just-finished (succeeded) download. Main thread only.
/// Returns true if launched (caller should Sleep briefly then PostQuitMessage).
bool update_launch_updater();
