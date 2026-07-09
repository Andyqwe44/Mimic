/**
 * input_winapi.cpp — WinAPI method (OS-level: AttachThreadInput + SendMessage).
 */
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include "input_common.h"
#include "input_methods.h"
#include "../../logger/logger.h"
#include <windows.h>
#include <vector>

std::string input_winapi(HWND hWnd, const InputArgs& a) {
    DWORD absX = 0, absY = 0;
    int clientX = 0, clientY = 0;
    bool coordsOk = true;
    if (!norm_to_screen(hWnd, a.x_norm, a.y_norm, absX, absY)) coordsOk = false;
    if (!norm_to_client(hWnd, a.x_norm, a.y_norm, clientX, clientY)) coordsOk = false;
    bool isMouse = (a.type == "click" || a.type == "dblclick" || a.type == "move" || a.type == "drag" || a.type == "wheel");
    if (!coordsOk && isMouse)
        return "{\"ok\":false,\"error\":\"failed to get target window client rect\"}";

    // Attach thread input for cross-thread input state sync
    DWORD targetTid = GetWindowThreadProcessId(hWnd, nullptr);
    DWORD myTid = GetCurrentThreadId();
    BOOL attached = (targetTid != myTid) ? AttachThreadInput(myTid, targetTid, TRUE) : FALSE;
    if (targetTid != myTid && !attached) {
        LOG("input","winapi: AttachThreadInput FAILED tid=%lu",(unsigned long)targetTid);
    }

    // Bring window to foreground for mouse operations
    if (isMouse) {
        SetForegroundWindow(hWnd);
        SetFocus(hWnd);
        Sleep(30);
    }

    // Helper to detach on early returns
    auto detach = [&]() { if (attached) AttachThreadInput(myTid, targetTid, FALSE); };

    if (a.type == "click" || a.type == "dblclick") {
        UINT downMsg = (a.button == "right") ? WM_RBUTTONDOWN :
                      (a.button == "middle") ? WM_MBUTTONDOWN : WM_LBUTTONDOWN;
        UINT upMsg   = (a.button == "right") ? WM_RBUTTONUP :
                      (a.button == "middle") ? WM_MBUTTONUP : WM_LBUTTONUP;
        WPARAM wDown = (a.button == "right") ? MK_RBUTTON :
                      (a.button == "middle") ? MK_MBUTTON : MK_LBUTTON;
        LPARAM lp = MAKELPARAM(clientX, clientY);
        auto doClick = [&]() {
            SendMessageW(hWnd, downMsg, wDown, lp);
            SendMessageW(hWnd, upMsg, 0, lp);
        };
        doClick();
        if (a.type == "dblclick") { Sleep(GetDoubleClickTime() / 2); doClick(); }
    }
    else if (a.type == "move") {
        SendMessageW(hWnd, WM_MOUSEMOVE, 0, MAKELPARAM(clientX, clientY));
    }
    else if (a.type == "drag") {
        if (a.dragPath.empty()) { detach(); return "{\"ok\":false,\"error\":\"drag requires at least one point\"}"; }
        UINT dM = (a.button=="right")?WM_RBUTTONDOWN:(a.button=="middle")?WM_MBUTTONDOWN:WM_LBUTTONDOWN;
        UINT uM = (a.button=="right")?WM_RBUTTONUP:(a.button=="middle")?WM_MBUTTONUP:WM_LBUTTONUP;
        WPARAM wD = (a.button=="right")?MK_RBUTTON:(a.button=="middle")?MK_MBUTTON:MK_LBUTTON;
        { int cx,cy; norm_to_client(hWnd,a.dragPath[0].first,a.dragPath[0].second,cx,cy);
          SendMessageW(hWnd,WM_MOUSEMOVE,0,MAKELPARAM(cx,cy));
          SendMessageW(hWnd,dM,wD,MAKELPARAM(cx,cy)); }
        for (size_t i=1;i<a.dragPath.size();i++) { int cx,cy; norm_to_client(hWnd,a.dragPath[i].first,a.dragPath[i].second,cx,cy);
          SendMessageW(hWnd,WM_MOUSEMOVE,wD,MAKELPARAM(cx,cy)); Sleep(5); }
        { int cx,cy; norm_to_client(hWnd,a.dragPath.back().first,a.dragPath.back().second,cx,cy);
          SendMessageW(hWnd,uM,0,MAKELPARAM(cx,cy)); }
    }
    else if (a.type == "wheel") {
        int delta = -a.delta;
        SendMessageW(hWnd, WM_MOUSEWHEEL, MAKEWPARAM(0, (short)delta), MAKELPARAM(clientX, clientY));
    }
    else if (a.type == "keydown" || a.type == "keyup" || a.type == "keypress") {
        if (a.vk == 0) { detach(); return "{\"ok\":false,\"error\":\"key requires valid vk\"}"; }
        WORD s = scan_from_vk((WORD)a.vk);
        auto doKey = [&](bool up) {
            UINT msg = up ? WM_KEYUP : WM_KEYDOWN;
            LPARAM lp = MAKELPARAM(1, s);
            if (up) lp |= (1 << 31) | (1 << 30);
            SendMessageW(hWnd, msg, (WPARAM)a.vk, lp);
        };
        if (a.type == "keypress") { doKey(false); Sleep(5); doKey(true); }
        else { doKey(a.type == "keyup"); }
    }
    else if (a.type == "combo") {
        if (a.vk == 0) { detach(); return "{\"ok\":false,\"error\":\"combo requires valid vk\"}"; }
        struct { bool ac; WORD vk; } m[] = {{a.ctrlKey,VK_CONTROL},{a.shiftKey,VK_SHIFT},{a.altKey,VK_MENU},{a.metaKey,VK_LWIN}};
        auto dk = [&](WORD kv, bool up) {
            UINT msg = up ? WM_KEYUP : WM_KEYDOWN;
            LPARAM lp = MAKELPARAM(1, scan_from_vk(kv));
            if (up) lp |= (1 << 31) | (1 << 30);
            SendMessageW(hWnd, msg, (WPARAM)kv, lp);
        };
        for (auto& md : m) { if (md.ac) dk(md.vk, false); }
        dk((WORD)a.vk, false); Sleep(5); dk((WORD)a.vk, true);
        for (int i = 3; i >= 0; i--) { if (m[i].ac) dk(m[i].vk, true); }
    }
    else if (a.type == "text") {
        if (a.text.empty()) { detach(); return "{\"ok\":false,\"error\":\"text requires 'text' field\"}"; }
        int wl = MultiByteToWideChar(CP_UTF8, 0, a.text.c_str(), (int)a.text.size(), nullptr, 0);
        if (wl <= 0) { detach(); return "{\"ok\":false,\"error\":\"UTF8->UTF16 failed\"}"; }
        std::vector<wchar_t> wb(wl + 1);
        MultiByteToWideChar(CP_UTF8, 0, a.text.c_str(), (int)a.text.size(), wb.data(), wl);
        for (int i = 0; i < wl; i++) {
            SendMessageW(hWnd, WM_CHAR, wb[i], MAKELPARAM(1, 1));
            Sleep(5);
        }
    }
    else { detach(); return "{\"ok\":false,\"error\":\"unknown type: " + a.type + "\"}"; }

    detach();
    LOG("input","winapi: %s attached=%d",a.type.c_str(),attached);
    return "{\"ok\":true}";
}
