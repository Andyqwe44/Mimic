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

// ── Coordinate conversion ──
// Returns false if GetClientRect fails (window destroyed)
GAM_API bool norm_to_screen(HWND hWnd, double nx, double ny, DWORD& absX, DWORD& absY);
GAM_API bool norm_to_client(HWND hWnd, double nx, double ny, int& cx, int& cy);

// Parse drag path from raw JSON args string: "path":[{"x":0.5,"y":0.5},...]
GAM_API std::vector<std::pair<double, double>> parse_drag_path(const std::string& json);
