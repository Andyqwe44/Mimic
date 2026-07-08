/**
 * commands.h — Backend command dispatch (replaces Tauri invoke).
 *
 * Each command receives parsed JSON args and returns a JSON result string.
 * Thread-safe for stream commands (start/stop from UI thread).
 */
#pragma once
#include <string>

/// Dispatch a WebMessage JSON command. Returns JSON response (or empty if fire-and-forget).
std::string dispatch_command(const std::string& json);

/// Push a BGRA frame to the WebView2 frontend via SharedBuffer (zero-copy).
/// Called from stream thread and single-frame capture.
void shared_buffer_push_frame(const uint8_t* bgra, int w, int h);

/// Initialize backend subsystems (logger, COM, WGC apartment).
void backend_init();

/// Shutdown backend (stop streams, flush logger).
void backend_shutdown();
