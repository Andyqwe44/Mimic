/**
 * input_common.cpp — Shared input helpers + JSON args parser.
 */
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include "input_common.h"
#include "input_methods.h"
#include "../../monitor_app/src/json_helper.h"
#include "../../logger/logger.h"
#include <cstdlib>
#include <cstring>

// ═══ Key mapping ════════════════════════════════════════════

WORD vk_from_name(const std::string& name) {
    if (name.empty()) return 0;
    if (name.length() == 1) {
        char c = (char)toupper((unsigned char)name[0]);
        if (c >= 'A' && c <= 'Z') return (WORD)c;
        if (c >= '0' && c <= '9') return (WORD)c;
    }
    if (name == "Enter" || name == "Return") return VK_RETURN;
    if (name == "Tab") return VK_TAB;
    if (name == "Escape" || name == "Esc") return VK_ESCAPE;
    if (name == "Backspace" || name == "Back") return VK_BACK;
    if (name == "Delete" || name == "Del") return VK_DELETE;
    if (name == "Insert" || name == "Ins") return VK_INSERT;
    if (name == "Home") return VK_HOME;
    if (name == "End") return VK_END;
    if (name == "PageUp") return VK_PRIOR;
    if (name == "PageDown") return VK_NEXT;
    if (name == "Up" || name == "ArrowUp") return VK_UP;
    if (name == "Down" || name == "ArrowDown") return VK_DOWN;
    if (name == "Left" || name == "ArrowLeft") return VK_LEFT;
    if (name == "Right" || name == "ArrowRight") return VK_RIGHT;
    if (name == "Space" || name == " ") return VK_SPACE;
    if (name == "Ctrl" || name == "Control") return VK_CONTROL;
    if (name == "Shift") return VK_SHIFT;
    if (name == "Alt" || name == "Menu") return VK_MENU;
    if (name == "Win" || name == "Meta" || name == "LWin") return VK_LWIN;
    if (name == "RWin") return VK_RWIN;
    if (name == "F1") return VK_F1;   if (name == "F2") return VK_F2;
    if (name == "F3") return VK_F3;   if (name == "F4") return VK_F4;
    if (name == "F5") return VK_F5;   if (name == "F6") return VK_F6;
    if (name == "F7") return VK_F7;   if (name == "F8") return VK_F8;
    if (name == "F9") return VK_F9;   if (name == "F10") return VK_F10;
    if (name == "F11") return VK_F11; if (name == "F12") return VK_F12;
    if (name == "CapsLock") return VK_CAPITAL;
    if (name == "NumLock") return VK_NUMLOCK;
    if (name == "PrintScreen" || name == "PrtSc") return VK_SNAPSHOT;
    if (name == "ScrollLock") return VK_SCROLL;
    if (name == "Pause" || name == "Break") return VK_PAUSE;
    return 0;
}

WORD scan_from_vk(WORD vk) {
    return (WORD)MapVirtualKeyA(vk, MAPVK_VK_TO_VSC);
}

bool is_extended_key(WORD vk) {
    WORD scan = scan_from_vk(vk);
    return (scan & 0xE000) != 0; // E0/E1 scan prefix = extended keyboard key
}

// ═══ Coordinate conversion ══════════════════════════════════

bool norm_to_screen(HWND hWnd, double nx, double ny, DWORD& absX, DWORD& absY) {
    RECT cr;
    if (!GetClientRect(hWnd, &cr)) {
        LOG("input", "norm_to_screen: GetClientRect FAILED for hwnd=0x%llx", (unsigned long long)(uintptr_t)hWnd);
        absX = 0; absY = 0;
        return false;
    }
    POINT pt = { cr.left, cr.top }; ClientToScreen(hWnd, &pt);
    int sx = pt.x + (int)(nx * (cr.right - cr.left));
    int sy = pt.y + (int)(ny * (cr.bottom - cr.top));
    int vsX = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vsY = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vsW = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vsH = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    absX = (DWORD)(((double)(sx - vsX) / (double)vsW) * 65535.0);
    absY = (DWORD)(((double)(sy - vsY) / (double)vsH) * 65535.0);
    return true;
}

bool norm_to_client(HWND hWnd, double nx, double ny, int& cx, int& cy) {
    RECT cr;
    if (!GetClientRect(hWnd, &cr)) {
        LOG("input", "norm_to_client: GetClientRect FAILED for hwnd=0x%llx", (unsigned long long)(uintptr_t)hWnd);
        cx = 0; cy = 0;
        return false;
    }
    cx = (int)(nx * (cr.right - cr.left));
    cy = (int)(ny * (cr.bottom - cr.top));
    return true;
}

// ═══ Drag path JSON parser ══════════════════════════════════

std::vector<std::pair<double, double>> parse_drag_path(const std::string& json) {
    std::vector<std::pair<double, double>> pts;
    std::string s = "\"path\":[";
    size_t p = json.find(s);
    if (p == std::string::npos) return pts;
    p += s.length();
    while (p < json.size() && json[p] != ']') {
        size_t obj = json.find('{', p);
        if (obj == std::string::npos) break;
        size_t end = json.find('}', obj);
        if (end == std::string::npos) break;
        size_t close = json.find(']', p);
        if (end > close && close != std::string::npos) break;
        double x = 0, y = 0;
        size_t xp = json.find("\"x\":", obj);
        if (xp != std::string::npos && xp < end) x = strtod(json.c_str() + xp + 4, nullptr);
        size_t yp = json.find("\"y\":", obj);
        if (yp != std::string::npos && yp < end) y = strtod(json.c_str() + yp + 4, nullptr);
        pts.push_back({ x, y });
        p = end + 1;
        while (p < json.size() && (json[p] == ' ' || json[p] == ',')) p++;
    }
    return pts;
}

// ═══ InputArgs parser ═══════════════════════════════════════

InputArgs parse_input_args(const std::string& argsJson) {
    InputArgs a;
    a.hwnd   = json_get_uint64(argsJson, "hwnd");
    a.type   = json_get_str(argsJson, "type");
    a.method = json_get_str(argsJson, "method");
    a.button = json_get_str(argsJson, "button");
    a.x_norm = json_get_double(argsJson, "x_norm");
    a.y_norm = json_get_double(argsJson, "y_norm");
    a.vk     = json_get_int(argsJson, "vk");
    a.keyName = json_get_str(argsJson, "key");
    a.code   = json_get_str(argsJson, "code");
    a.delta  = (int)json_get_double(argsJson, "delta");
    a.ctrlKey  = json_get_bool(argsJson, "ctrlKey");
    a.shiftKey = json_get_bool(argsJson, "shiftKey");
    a.altKey   = json_get_bool(argsJson, "altKey");
    a.metaKey  = json_get_bool(argsJson, "metaKey");
    a.text   = json_get_str(argsJson, "text");
    a.dragPath = parse_drag_path(argsJson);

    // VK fallback: if vk not provided, derive from key name
    if (a.vk == 0 && !a.keyName.empty()) {
        a.vk = vk_from_name(a.keyName);
    }

    // Wheel delta fallback
    if (a.delta == 0 && a.type == "wheel") {
        size_t dp = argsJson.find("\"delta\":");
        if (dp != std::string::npos) {
            a.delta = (int)strtol(argsJson.c_str() + dp + 8, nullptr, 10);
        }
    }

    return a;
}
