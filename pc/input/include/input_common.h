/**
 * input_common.h — Shared input helpers.
 *
 * Internal header — included by per-method .cpp files, not by commands.cpp.
 */
#pragma once
#include <string>
#include <vector>
#include <utility>
#include <cstdint>
#include <windows.h>
#include "../../common/include/export.h"

// ── Key mapping ──
GAM_API WORD vk_from_name(const std::string& name);
GAM_API WORD scan_from_vk(WORD vk);
GAM_API bool is_extended_key(WORD vk);
GAM_API LPARAM key_message_lparam(WORD vk, bool keyUp);

// ── Coordinate conversion ──
// Returns false if GetClientRect fails (window destroyed)
GAM_API bool norm_to_screen_point(HWND hWnd, double nx, double ny, POINT& screenPoint);
GAM_API bool norm_to_screen(HWND hWnd, double nx, double ny, DWORD& absX, DWORD& absY);
GAM_API bool norm_to_client(HWND hWnd, double nx, double ny, int& cx, int& cy);

// True if this action type carries x_norm/y_norm (or a drag path).
GAM_API bool input_type_uses_norm_coords(const std::string& type);

// Window targets: reject coords outside [0,1] (security isolation).
// Desktop (hwnd null/0): only reject NaN / non-finite.
// Returns empty on OK, otherwise an error message.
GAM_API std::string input_validate_norm_bounds(HWND hWnd, double nx, double ny);

// Parse drag path from raw JSON args string: "path":[{"x":0.5,"y":0.5},...]
GAM_API std::vector<std::pair<double, double>> parse_drag_path(const std::string& json);

// ── Last keyboard / text target (set by mapped clicks) ──
GAM_API void input_remember_keyboard_target(HWND captureHwnd, HWND targetHwnd);
GAM_API HWND input_keyboard_target(HWND captureHwnd);

// Optional test-only focus (AttachThreadInput + SetFocus). Production agent
// paths must not call this — it can steal the user's keyboard focus.
GAM_API bool input_focus_hwnd(HWND hwnd);

// Deliver committed Unicode via WM_CHAR to keyHwnd (background). Never uses
// SetForegroundWindow / BringWindowToTop / WM_MOUSEACTIVATE.
// allow_focus=true → SetFocus first (test harness only; default false).
// Returns empty string on success, or an error message.
GAM_API std::string input_deliver_committed_text(
    HWND keyHwnd, const std::string& utf8, bool allow_focus);
