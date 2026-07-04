/**
 * Window enumeration tool — called by Tauri as a subprocess
 * Outputs JSON array of visible taskbar windows, one per line
 *
 * Build: same as capture_test.exe (MSVC build.cmd)
 * Usage: window_list.exe → prints JSON to stdout
 */

#ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dwmapi.h>
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "user32.lib")

struct WinInfo {
    std::string title;
    std::string category; // "desktop" | "window"
};

static std::vector<WinInfo> g_windows;

// Check if a window should appear in the taskbar / Alt+Tab
static bool is_taskbar_window(HWND hwnd) {
    if (!IsWindowVisible(hwnd)) return false;
    LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);

    // Must have a title bar
    bool has_title = (style & WS_CAPTION) != 0;
    // Not a tool window
    bool not_tool = (exStyle & WS_EX_TOOLWINDOW) == 0;
    // Not cloaked (minimized to tray, etc.)
    BOOL cloaked = FALSE;
    DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));

    // Must have non-zero size
    RECT r;
    GetWindowRect(hwnd, &r);
    bool has_size = (r.right - r.left) > 0 && (r.bottom - r.top) > 0;

    // Owner window = probably a dialog, skip
    HWND owner = GetWindow(hwnd, GW_OWNER);

    return has_title && not_tool && !cloaked && has_size && !owner;
}

static BOOL CALLBACK enum_proc(HWND hwnd, LPARAM) {
    wchar_t buf[256];
    if (GetWindowTextW(hwnd, buf, 256) == 0) return TRUE;
    std::wstring ws(buf);
    int len = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string title(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, &title[0], len, nullptr, nullptr);
    while (!title.empty() && title.back() == '\0') title.pop_back();
    if (title.empty() || title == "Program Manager") return TRUE;

    g_windows.push_back({title, is_taskbar_window(hwnd) ? "window" : "process"});
    return TRUE;
}

int main() {
    // Add desktop as first entry
    printf("{\"category\":\"desktop\",\"title\":\" Entire Desktop\"}\n");

    EnumWindows(enum_proc, 0);

    // Print as JSON lines
    for (auto& w : g_windows) {
        // Escape quotes in title
        for (size_t i = 0; i < w.title.size(); i++) {
            if (w.title[i] == '"' || w.title[i] == '\\') {
                w.title.insert(i, "\\");
                i++;
            }
        }
        printf("{\"category\":\"%s\",\"title\":\"%s\"}\n",
               w.category.c_str(), w.title.c_str());
    }
    return 0;
}
