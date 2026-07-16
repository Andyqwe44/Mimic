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
#include <vector>

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
    return (WORD)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC_EX);
}

bool is_extended_key(WORD vk) {
    WORD scan = scan_from_vk(vk);
    return (scan & 0xFF00) == 0xE000 || (scan & 0xFF00) == 0xE100;
}

LPARAM key_message_lparam(WORD vk, bool keyUp) {
    WORD scan = scan_from_vk(vk);
    LPARAM lp = 1 | ((LPARAM)(scan & 0x00FF) << 16);
    if (is_extended_key(vk)) lp |= (1 << 24);
    if (keyUp) lp |= (1LL << 31) | (1 << 30);
    return lp;
}

// ═══ Coordinate conversion ══════════════════════════════════

bool norm_to_screen_point(HWND hWnd, double nx, double ny, POINT& screenPoint) {
    int sx, sy;
    if (hWnd == nullptr || hWnd == (HWND)0) {
        // Desktop: map normalized coords directly to virtual screen
        int vsX = GetSystemMetrics(SM_XVIRTUALSCREEN);
        int vsY = GetSystemMetrics(SM_YVIRTUALSCREEN);
        int vsW = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        int vsH = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        sx = vsX + (int)(nx * vsW);
        sy = vsY + (int)(ny * vsH);
    } else {
        // Window capture: WGC captures full window including non-client area
        // (title bar, borders). Use GetWindowRect to match the captured image.
        RECT wr;
        if (!GetWindowRect(hWnd, &wr)) {
            LOG("input", "norm_to_screen_point: GetWindowRect FAILED for hwnd=0x%llx",
                (unsigned long long)(uintptr_t)hWnd);
            screenPoint = { 0, 0 };
            return false;
        }
        sx = wr.left + (int)(nx * (double)(wr.right - wr.left));
        sy = wr.top  + (int)(ny * (double)(wr.bottom - wr.top));
    }
    screenPoint = { sx, sy };
    return true;
}

bool norm_to_screen(HWND hWnd, double nx, double ny, DWORD& absX, DWORD& absY) {
    POINT screenPoint = {};
    if (!norm_to_screen_point(hWnd, nx, ny, screenPoint)) {
        absX = 0;
        absY = 0;
        return false;
    }
    int vsX = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vsY = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vsW = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vsH = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    absX = (DWORD)(((double)(screenPoint.x - vsX) / (double)vsW) * 65535.0);
    absY = (DWORD)(((double)(screenPoint.y - vsY) / (double)vsH) * 65535.0);
    return true;
}

bool norm_to_client(HWND hWnd, double nx, double ny, int& cx, int& cy) {
    if (hWnd == nullptr || hWnd == (HWND)0) {
        // Desktop: use virtual screen as "client"
        cx = (int)(nx * GetSystemMetrics(SM_CXVIRTUALSCREEN));
        cy = (int)(ny * GetSystemMetrics(SM_CYVIRTUALSCREEN));
        return true;
    }
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

bool input_type_uses_norm_coords(const std::string& type) {
    return type == "click" || type == "dblclick" || type == "mousedown" ||
           type == "mouseup" || type == "move" || type == "drag" || type == "wheel";
}

std::string input_validate_norm_bounds(HWND hWnd, double nx, double ny) {
    if (!(nx == nx) || !(ny == ny)) // NaN
        return "invalid coordinates (NaN)";
    if (hWnd == nullptr || hWnd == (HWND)0) {
        // Desktop: normalized virtual-screen coords; still require finite range.
        if (nx < 0.0 || nx > 1.0 || ny < 0.0 || ny > 1.0)
            return "desktop coordinates out of range [0,1]";
        return {};
    }
    // Window: hard isolation — never operate outside the capture window.
    if (nx < 0.0 || nx > 1.0 || ny < 0.0 || ny > 1.0)
        return "coordinates outside target window [0,1]";
    return {};
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
    a.held     = json_get_bool(argsJson, "held");
    a.focus    = json_get_bool(argsJson, "focus");
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

// ═══ Keyboard / committed-text target ═══════════════════════

static HWND g_last_capture_root = nullptr;
static HWND g_last_keyboard_target = nullptr;

void input_remember_keyboard_target(HWND captureHwnd, HWND targetHwnd) {
    g_last_capture_root = captureHwnd;
    g_last_keyboard_target = targetHwnd;
}

HWND input_keyboard_target(HWND captureHwnd) {
    if (g_last_keyboard_target && IsWindow(g_last_keyboard_target)) {
        // Desktop capture passes hwnd=0 — accept any remembered target.
        if (!captureHwnd ||
            (g_last_capture_root == captureHwnd &&
             (g_last_keyboard_target == captureHwnd ||
              IsChild(captureHwnd, g_last_keyboard_target)))) {
            return g_last_keyboard_target;
        }
    }

    if (!captureHwnd || !IsWindow(captureHwnd)) return nullptr;
    DWORD tid = GetWindowThreadProcessId(captureHwnd, nullptr);
    GUITHREADINFO info = {};
    info.cbSize = sizeof(info);
    if (tid && GetGUIThreadInfo(tid, &info) && info.hwndFocus &&
        (info.hwndFocus == captureHwnd || IsChild(captureHwnd, info.hwndFocus))) {
        return info.hwndFocus;
    }
    return captureHwnd;
}

bool input_focus_hwnd(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return false;
    DWORD tidTarget = GetWindowThreadProcessId(hwnd, nullptr);
    DWORD tidCur = GetCurrentThreadId();
    if (tidTarget && tidTarget != tidCur)
        AttachThreadInput(tidCur, tidTarget, TRUE);
    SetFocus(hwnd);
    if (tidTarget && tidTarget != tidCur)
        AttachThreadInput(tidCur, tidTarget, FALSE);
    return true;
}

std::string input_deliver_committed_text(
    HWND keyHwnd, const std::string& utf8, bool allow_focus) {
    if (!keyHwnd || !IsWindow(keyHwnd))
        return "no text target; click a mapped control first";
    if (utf8.empty()) return "text requires 'text' field";

    int wl = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), nullptr, 0);
    if (wl <= 0) return "UTF8->UTF16 failed";
    std::vector<wchar_t> wb(wl + 1);
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), wb.data(), wl);

    // Production default: no SetFocus — must not steal the user's foreground
    // keyboard focus. Test harness may pass focus=true explicitly.
    if (allow_focus) {
        if (!input_focus_hwnd(keyHwnd))
            return "failed to focus text target";
    }

    for (int i = 0; i < wl; i++) {
        // Background Unicode: PostMessage so we never activate/raise the target.
        if (!PostMessageW(keyHwnd, WM_CHAR, (WPARAM)wb[i], MAKELPARAM(1, 1))) {
            DWORD err = GetLastError();
            char detail[96] = {};
            snprintf(detail, sizeof(detail), "PostMessage WM_CHAR failed error=%lu",
                     (unsigned long)err);
            return detail;
        }
        Sleep(5);
    }

    LOG("input", "text: committed %d char(s) via WM_CHAR%s → 0x%llx",
        wl, allow_focus ? "+SetFocus" : " (background)",
        (unsigned long long)(uintptr_t)keyHwnd);
    return {};
}
