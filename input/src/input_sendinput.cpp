/**
 * input_sendinput.cpp — SendInput method (application-level synthesized input).
 */
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include "input_common.h"
#include "input_methods.h"
#include "../../logger/logger.h"
#include <windows.h>
#include <vector>

std::string input_sendinput(HWND hWnd, const InputArgs& a) {
    DWORD absX = 0, absY = 0;
    int clientX = 0, clientY = 0;
    bool coordsOk = true;
    if (!norm_to_screen(hWnd, a.x_norm, a.y_norm, absX, absY)) coordsOk = false;
    if (!norm_to_client(hWnd, a.x_norm, a.y_norm, clientX, clientY)) coordsOk = false;
    bool isMouse = (a.type == "click" || a.type == "dblclick" || a.type == "move" || a.type == "drag" || a.type == "wheel");
    if (!coordsOk && isMouse)
        return "{\"ok\":false,\"error\":\"failed to get target window client rect\"}";

    if (a.type == "click" || a.type == "dblclick") {
        DWORD downFlag = (a.button == "right") ? MOUSEEVENTF_RIGHTDOWN :
                        (a.button == "middle") ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_LEFTDOWN;
        DWORD upFlag   = (a.button == "right") ? MOUSEEVENTF_RIGHTUP :
                        (a.button == "middle") ? MOUSEEVENTF_MIDDLEUP : MOUSEEVENTF_LEFTUP;
        auto doClick = [&]() -> bool {
            INPUT inputs[2] = {};
            inputs[0].type = INPUT_MOUSE;
            inputs[0].mi.dx = absX; inputs[0].mi.dy = absY;
            inputs[0].mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_MOVE | downFlag;
            inputs[1].type = INPUT_MOUSE;
            inputs[1].mi.dx = absX; inputs[1].mi.dy = absY;
            inputs[1].mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_MOVE | upFlag;
            UINT sent = SendInput(2, inputs, sizeof(INPUT));
            if (sent != 2) { LOG("input","SendInput click sent=%u",sent); return false; }
            return true;
        };
        if (!doClick()) return "{\"ok\":false,\"error\":\"SendInput failed (UIPI may block)\"}";
        if (a.type == "dblclick") { Sleep(GetDoubleClickTime()/2); if (!doClick()) return "{\"ok\":false,\"error\":\"dblclick second click failed\"}"; }
        LOG("input","sendinput: %s at (%.3f,%.3f)",a.type.c_str(),a.x_norm,a.y_norm);
    }
    else if (a.type == "move") {
        INPUT i = {}; i.type = INPUT_MOUSE;
        i.mi.dx = absX; i.mi.dy = absY;
        i.mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_MOVE;
        SendInput(1, &i, sizeof(INPUT));
    }
    else if (a.type == "drag") {
        if (a.dragPath.empty()) return "{\"ok\":false,\"error\":\"drag requires at least one point\"}";
        DWORD dF = (a.button=="right")?MOUSEEVENTF_RIGHTDOWN:(a.button=="middle")?MOUSEEVENTF_MIDDLEDOWN:MOUSEEVENTF_LEFTDOWN;
        DWORD uF = (a.button=="right")?MOUSEEVENTF_RIGHTUP:(a.button=="middle")?MOUSEEVENTF_MIDDLEUP:MOUSEEVENTF_LEFTUP;
        { DWORD ax,ay; norm_to_screen(hWnd,a.dragPath[0].first,a.dragPath[0].second,ax,ay);
          INPUT ins[2]={}; ins[0].type=INPUT_MOUSE; ins[0].mi.dx=ax; ins[0].mi.dy=ay;
          ins[0].mi.dwFlags=MOUSEEVENTF_ABSOLUTE|MOUSEEVENTF_MOVE;
          ins[1].type=INPUT_MOUSE; ins[1].mi.dx=ax; ins[1].mi.dy=ay;
          ins[1].mi.dwFlags=MOUSEEVENTF_ABSOLUTE|MOUSEEVENTF_MOVE|dF; SendInput(2,ins,sizeof(INPUT)); }
        for (size_t i = 1; i < a.dragPath.size(); i++) {
            DWORD ax,ay; norm_to_screen(hWnd,a.dragPath[i].first,a.dragPath[i].second,ax,ay);
            INPUT in={}; in.type=INPUT_MOUSE; in.mi.dx=ax; in.mi.dy=ay;
            in.mi.dwFlags=MOUSEEVENTF_ABSOLUTE|MOUSEEVENTF_MOVE; SendInput(1,&in,sizeof(INPUT)); Sleep(5); }
        { DWORD ax,ay; norm_to_screen(hWnd,a.dragPath.back().first,a.dragPath.back().second,ax,ay);
          INPUT in={}; in.type=INPUT_MOUSE; in.mi.dx=ax; in.mi.dy=ay;
          in.mi.dwFlags=MOUSEEVENTF_ABSOLUTE|MOUSEEVENTF_MOVE|uF; SendInput(1,&in,sizeof(INPUT)); }
        LOG("input","sendinput: drag %zu pts",a.dragPath.size());
    }
    else if (a.type == "wheel") {
        int delta = -a.delta;
        INPUT i={}; i.type=INPUT_MOUSE; i.mi.dx=absX; i.mi.dy=absY;
        i.mi.dwFlags=MOUSEEVENTF_ABSOLUTE|MOUSEEVENTF_MOVE|MOUSEEVENTF_WHEEL;
        i.mi.mouseData=(DWORD)delta; SendInput(1,&i,sizeof(INPUT));
        LOG("input","sendinput: wheel delta=%d",delta);
    }
    else if (a.type == "keydown" || a.type == "keyup" || a.type == "keypress") {
        if (a.vk == 0) return "{\"ok\":false,\"error\":\"key requires valid vk\"}";
        WORD s = scan_from_vk((WORD)a.vk);
        DWORD x = is_extended_key((WORD)a.vk) ? KEYEVENTF_EXTENDEDKEY : 0;
        if (a.type == "keypress") {
            INPUT ins[2] = {};
            ins[0].type = INPUT_KEYBOARD; ins[0].ki.wVk = (WORD)a.vk;
            ins[0].ki.wScan = s; ins[0].ki.dwFlags = x;
            ins[1].type = INPUT_KEYBOARD; ins[1].ki.wVk = (WORD)a.vk;
            ins[1].ki.wScan = s; ins[1].ki.dwFlags = KEYEVENTF_KEYUP | x;
            SendInput(2, ins, sizeof(INPUT)); Sleep(5);
        } else {
            INPUT in = {};
            in.type = INPUT_KEYBOARD; in.ki.wVk = (WORD)a.vk;
            in.ki.wScan = s;
            in.ki.dwFlags = (a.type == "keyup") ? (KEYEVENTF_KEYUP | x) : x;
            SendInput(1, &in, sizeof(INPUT));
        }
        LOG("input","sendinput: %s vk=%d",a.type.c_str(),a.vk);
    }
    else if (a.type == "combo") {
        if (a.vk == 0) return "{\"ok\":false,\"error\":\"combo requires valid vk\"}";
        struct { bool ac; WORD vk; } m[] = {
            {a.ctrlKey, VK_CONTROL}, {a.shiftKey, VK_SHIFT},
            {a.altKey, VK_MENU}, {a.metaKey, VK_LWIN}
        };
        std::vector<INPUT> b;
        for (auto& md : m) {
            if (!md.ac) continue;
            INPUT in = {}; in.type = INPUT_KEYBOARD;
            in.ki.wVk = md.vk; in.ki.wScan = scan_from_vk(md.vk);
            in.ki.dwFlags = is_extended_key(md.vk) ? KEYEVENTF_EXTENDEDKEY : 0;
            b.push_back(in);
        }
        DWORD mx = is_extended_key((WORD)a.vk) ? KEYEVENTF_EXTENDEDKEY : 0;
        INPUT kd = {}; kd.type = INPUT_KEYBOARD;
        kd.ki.wVk = (WORD)a.vk; kd.ki.wScan = scan_from_vk((WORD)a.vk);
        kd.ki.dwFlags = mx; b.push_back(kd);
        { INPUT ku = {}; ku.type = INPUT_KEYBOARD;
          ku.ki.wVk = (WORD)a.vk; ku.ki.wScan = scan_from_vk((WORD)a.vk);
          ku.ki.dwFlags = KEYEVENTF_KEYUP | mx; b.push_back(ku); }
        for (int i = 3; i >= 0; i--) {
            if (!m[i].ac) continue;
            INPUT in = {}; in.type = INPUT_KEYBOARD;
            in.ki.wVk = m[i].vk; in.ki.wScan = scan_from_vk(m[i].vk);
            in.ki.dwFlags = KEYEVENTF_KEYUP | (is_extended_key(m[i].vk) ? KEYEVENTF_EXTENDEDKEY : 0);
            b.push_back(in);
        }
        SendInput((UINT)b.size(), b.data(), sizeof(INPUT)); Sleep(5);
        LOG("input","sendinput: combo vk=%d",a.vk);
    }
    else if (a.type == "text") {
        if (a.text.empty()) return "{\"ok\":false,\"error\":\"text requires 'text' field\"}";
        int wl = MultiByteToWideChar(CP_UTF8, 0, a.text.c_str(), (int)a.text.size(), nullptr, 0);
        if (wl <= 0) return "{\"ok\":false,\"error\":\"UTF8->UTF16 failed\"}";
        std::vector<wchar_t> wb(wl + 1);
        MultiByteToWideChar(CP_UTF8, 0, a.text.c_str(), (int)a.text.size(), wb.data(), wl);
        for (int i = 0; i < wl; i++) {
            INPUT is[2] = {};
            is[0].type = INPUT_KEYBOARD; is[0].ki.wScan = wb[i];
            is[0].ki.dwFlags = KEYEVENTF_UNICODE;
            is[1].type = INPUT_KEYBOARD; is[1].ki.wScan = wb[i];
            is[1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
            SendInput(2, is, sizeof(INPUT)); Sleep(5);
        }
        LOG("input","sendinput: text %d chars",wl);
    }
    else { return "{\"ok\":false,\"error\":\"unknown type: " + a.type + "\"}"; }
    return "{\"ok\":true}";
}
