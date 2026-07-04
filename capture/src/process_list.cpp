/**
 * Process enumeration tool — lists ALL visible windows including background processes.
 * Called on-demand when user clicks "Process" filter in the window picker.
 * No desktop entry — desktop is handled by window_list.exe.
 *
 * Build: MSVC build.cmd
 * Usage: process_list.exe → prints JSON to stdout
 */

#ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>

#pragma comment(lib, "user32.lib")

struct WinInfo {
    std::string title;
    unsigned long long hwnd;
};

static std::vector<WinInfo> g_all;

static BOOL CALLBACK enum_proc(HWND hwnd, LPARAM) {
    if (!IsWindowVisible(hwnd)) return TRUE;

    wchar_t buf[256];
    if (GetWindowTextW(hwnd, buf, 256) == 0) return TRUE;

    std::wstring ws(buf);
    int len = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string title(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, &title[0], len, nullptr, nullptr);
    while (!title.empty() && title.back() == '\0') title.pop_back();
    if (title.empty() || title == "Program Manager") return TRUE;

    g_all.push_back({title, (unsigned long long)(ULONG_PTR)hwnd});
    return TRUE;
}

int main() {
    EnumWindows(enum_proc, 0);

    for (auto& w : g_all) {
        for (size_t i = 0; i < w.title.size(); i++)
            if (w.title[i] == '"' || w.title[i] == '\\') { w.title.insert(i, "\\"); i++; }
        printf("{\"hwnd\":\"%llu\",\"category\":\"process\",\"title\":\"%s\"}\n",
               w.hwnd, w.title.c_str());
    }
    return 0;
}
