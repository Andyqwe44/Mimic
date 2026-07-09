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

// ── Key mapping ──
WORD vk_from_name(const std::string& name);
WORD scan_from_vk(WORD vk);
bool is_extended_key(WORD vk);

// ── Coordinate conversion ──
// Returns false if GetClientRect fails (window destroyed)
bool norm_to_screen(HWND hWnd, double nx, double ny, DWORD& absX, DWORD& absY);
bool norm_to_client(HWND hWnd, double nx, double ny, int& cx, int& cy);

// Parse drag path from raw JSON args string: "path":[{"x":0.5,"y":0.5},...]
std::vector<std::pair<double, double>> parse_drag_path(const std::string& json);
