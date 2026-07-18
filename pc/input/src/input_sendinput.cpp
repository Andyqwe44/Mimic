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
    bool isMouse = (a.type == "click" || a.type == "dblclick" || a.type == "mousedown" ||
                    a.type == "mouseup" || a.type == "move" || a.type == "drag" || a.type == "wheel");
    if (!coordsOk && isMouse)
        return "{\"ok\":false,\"error\":\"failed to get target window client rect\"}";

    auto down_flag = [&]() -> DWORD {
        return (a.button == "right") ? MOUSEEVENTF_RIGHTDOWN :
               (a.button == "middle") ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_LEFTDOWN;
    };
    auto up_flag = [&]() -> DWORD {
        return (a.button == "right") ? MOUSEEVENTF_RIGHTUP :
               (a.button == "middle") ? MOUSEEVENTF_MIDDLEUP : MOUSEEVENTF_LEFTUP;
    };

    if (a.type == "click" || a.type == "dblclick") {
        DWORD downFlag = down_flag();
        DWORD upFlag   = up_flag();
        auto doClick = [&]() -> bool {
            INPUT inputs[2] = {};
            inputs[0].type = INPUT_MOUSE;
            inputs[0].mi.dx = absX; inputs[0].mi.dy = absY;
            inputs[0].mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK | MOUSEEVENTF_MOVE | downFlag;
            inputs[1].type = INPUT_MOUSE;
            inputs[1].mi.dx = absX; inputs[1].mi.dy = absY;
            inputs[1].mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK | MOUSEEVENTF_MOVE | upFlag;
            UINT sent = SendInput(2, inputs, sizeof(INPUT));
            if (sent != 2) { LOG("input","SendInput click sent=%u",sent); return false; }
            return true;
        };
        if (!doClick()) return "{\"ok\":false,\"error\":\"SendInput failed (UIPI may block)\"}";
        // Best-effort keyboard target: window under the click (desktop uses null root).
        POINT pt = {};
        if (norm_to_screen_point(hWnd, a.x_norm, a.y_norm, pt)) {
            HWND hit = WindowFromPoint(pt);
            if (hit) input_remember_keyboard_target(hWnd, hit);
        }
        LOG("input","sendinput: %s at (%.3f,%.3f)",a.type.c_str(),a.x_norm,a.y_norm);
    }
    else if (a.type == "mousedown" || a.type == "mouseup") {
        DWORD flag = (a.type == "mousedown") ? down_flag() : up_flag();
        INPUT inputs[2] = {};
        inputs[0].type = INPUT_MOUSE;
        inputs[0].mi.dx = absX; inputs[0].mi.dy = absY;
        inputs[0].mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK | MOUSEEVENTF_MOVE;
        inputs[1].type = INPUT_MOUSE;
        inputs[1].mi.dx = absX; inputs[1].mi.dy = absY;
        inputs[1].mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK | MOUSEEVENTF_MOVE | flag;
        if (SendInput(2, inputs, sizeof(INPUT)) != 2)
            return "{\"ok\":false,\"error\":\"SendInput press failed\"}";
        if (a.type == "mousedown") {
            POINT pt = {};
            if (norm_to_screen_point(hWnd, a.x_norm, a.y_norm, pt)) {
                HWND hit = WindowFromPoint(pt);
                if (hit) input_remember_keyboard_target(hWnd, hit);
            }
        }
    }
    else if (a.type == "move") {
        INPUT i = {}; i.type = INPUT_MOUSE;
        i.mi.dx = absX; i.mi.dy = absY;
        // Absolute move while button is physically held by a prior mousedown —
        // SendInput MOVE does not re-assert button state; OS tracks it.
        i.mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK | MOUSEEVENTF_MOVE;
        if (SendInput(1, &i, sizeof(INPUT)) != 1)
            return "{\"ok\":false,\"error\":\"SendInput move failed\"}";
    }
    else if (a.type == "drag") {
        if (a.dragPath.empty()) return "{\"ok\":false,\"error\":\"drag requires at least one point\"}";
        DWORD dF = (a.button=="right")?MOUSEEVENTF_RIGHTDOWN:(a.button=="middle")?MOUSEEVENTF_MIDDLEDOWN:MOUSEEVENTF_LEFTDOWN;
        DWORD uF = (a.button=="right")?MOUSEEVENTF_RIGHTUP:(a.button=="middle")?MOUSEEVENTF_MIDDLEUP:MOUSEEVENTF_LEFTUP;
        DWORD lastAx = absX, lastAy = absY;
        bool buttonDown = false;
        auto emergencyRelease = [&]() {
            if (!buttonDown) return;
            INPUT up = {};
            up.type = INPUT_MOUSE;
            up.mi.dx = lastAx;
            up.mi.dy = lastAy;
            up.mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK |
                            MOUSEEVENTF_MOVE | uF;
            if (SendInput(1, &up, sizeof(INPUT)) != 1)
                LOG_ERROR("input", "SendInput drag emergency release failed");
            buttonDown = false;
        };
        {
            if (!norm_to_screen(hWnd, a.dragPath[0].first, a.dragPath[0].second, lastAx, lastAy))
                return "{\"ok\":false,\"error\":\"failed to map drag start\"}";
            INPUT ins[2] = {};
            ins[0].type = INPUT_MOUSE;
            ins[0].mi.dx = lastAx; ins[0].mi.dy = lastAy;
            ins[0].mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK | MOUSEEVENTF_MOVE;
            ins[1].type = INPUT_MOUSE;
            ins[1].mi.dx = lastAx; ins[1].mi.dy = lastAy;
            ins[1].mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK | MOUSEEVENTF_MOVE | dF;
            if (SendInput(2, ins, sizeof(INPUT)) != 2)
                return "{\"ok\":false,\"error\":\"SendInput drag start failed\"}";
            buttonDown = true;
        }
        for (size_t i = 1; i < a.dragPath.size(); i++) {
            if (!norm_to_screen(hWnd, a.dragPath[i].first, a.dragPath[i].second, lastAx, lastAy)) {
                emergencyRelease();
                return "{\"ok\":false,\"error\":\"failed to map drag point\"}";
            }
            INPUT in = {};
            in.type = INPUT_MOUSE; in.mi.dx = lastAx; in.mi.dy = lastAy;
            in.mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK | MOUSEEVENTF_MOVE;
            if (SendInput(1, &in, sizeof(INPUT)) != 1) {
                emergencyRelease();
                return "{\"ok\":false,\"error\":\"SendInput drag move failed\"}";
            }
            Sleep(5);
        }
        INPUT up = {};
        up.type = INPUT_MOUSE; up.mi.dx = lastAx; up.mi.dy = lastAy;
        up.mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK |
                        MOUSEEVENTF_MOVE | uF;
        if (SendInput(1, &up, sizeof(INPUT)) != 1) {
            emergencyRelease();
            return "{\"ok\":false,\"error\":\"SendInput drag release failed\"}";
        }
        buttonDown = false;
        LOG("input","sendinput: drag %zu pts",a.dragPath.size());
    }
    else if (a.type == "wheel") {
        int delta = -a.delta;
        INPUT i={}; i.type=INPUT_MOUSE; i.mi.dx=absX; i.mi.dy=absY;
        i.mi.dwFlags=MOUSEEVENTF_ABSOLUTE|MOUSEEVENTF_VIRTUALDESK|MOUSEEVENTF_MOVE|MOUSEEVENTF_WHEEL;
        i.mi.mouseData=(DWORD)delta;
        if (SendInput(1,&i,sizeof(INPUT)) != 1)
            return "{\"ok\":false,\"error\":\"SendInput wheel failed\"}";
        LOG("input","sendinput: wheel delta=%d",delta);
    }
    else if (a.type == "keydown" || a.type == "keyup" || a.type == "keypress") {
        if (a.vk == 0) return "{\"ok\":false,\"error\":\"key requires valid vk\"}";
        WORD s = scan_from_vk((WORD)a.vk) & 0x00FF;
        DWORD x = is_extended_key((WORD)a.vk) ? KEYEVENTF_EXTENDEDKEY : 0;
        if (a.type == "keypress") {
            INPUT ins[2] = {};
            ins[0].type = INPUT_KEYBOARD; ins[0].ki.wVk = (WORD)a.vk;
            ins[0].ki.wScan = s; ins[0].ki.dwFlags = x;
            ins[1].type = INPUT_KEYBOARD; ins[1].ki.wVk = (WORD)a.vk;
            ins[1].ki.wScan = s; ins[1].ki.dwFlags = KEYEVENTF_KEYUP | x;
            if (SendInput(2, ins, sizeof(INPUT)) != 2)
                return "{\"ok\":false,\"error\":\"SendInput keypress failed\"}";
            Sleep(5);
        } else {
            INPUT in = {};
            in.type = INPUT_KEYBOARD; in.ki.wVk = (WORD)a.vk;
            in.ki.wScan = s;
            in.ki.dwFlags = (a.type == "keyup") ? (KEYEVENTF_KEYUP | x) : x;
            if (SendInput(1, &in, sizeof(INPUT)) != 1)
                return "{\"ok\":false,\"error\":\"SendInput key event failed\"}";
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
            in.ki.wVk = md.vk; in.ki.wScan = scan_from_vk(md.vk) & 0x00FF;
            in.ki.dwFlags = is_extended_key(md.vk) ? KEYEVENTF_EXTENDEDKEY : 0;
            b.push_back(in);
        }
        DWORD mx = is_extended_key((WORD)a.vk) ? KEYEVENTF_EXTENDEDKEY : 0;
        INPUT kd = {}; kd.type = INPUT_KEYBOARD;
        kd.ki.wVk = (WORD)a.vk; kd.ki.wScan = scan_from_vk((WORD)a.vk) & 0x00FF;
        kd.ki.dwFlags = mx; b.push_back(kd);
        { INPUT ku = {}; ku.type = INPUT_KEYBOARD;
          ku.ki.wVk = (WORD)a.vk; ku.ki.wScan = scan_from_vk((WORD)a.vk) & 0x00FF;
          ku.ki.dwFlags = KEYEVENTF_KEYUP | mx; b.push_back(ku); }
        for (int i = 3; i >= 0; i--) {
            if (!m[i].ac) continue;
            INPUT in = {}; in.type = INPUT_KEYBOARD;
            in.ki.wVk = m[i].vk; in.ki.wScan = scan_from_vk(m[i].vk) & 0x00FF;
            in.ki.dwFlags = KEYEVENTF_KEYUP | (is_extended_key(m[i].vk) ? KEYEVENTF_EXTENDEDKEY : 0);
            b.push_back(in);
        }
        if (SendInput((UINT)b.size(), b.data(), sizeof(INPUT)) != b.size())
            return "{\"ok\":false,\"error\":\"SendInput combo failed\"}";
        Sleep(5);
        LOG("input","sendinput: combo vk=%d",a.vk);
    }
    else if (a.type == "text") {
        HWND keyHwnd = input_keyboard_target(hWnd);
        std::string terr = input_deliver_committed_text(keyHwnd, a.text, a.focus);
        if (!terr.empty()) return "{\"ok\":false,\"error\":\"" + terr + "\"}";
    }
    else { return "{\"ok\":false,\"error\":\"unknown type: " + a.type + "\"}"; }
    return "{\"ok\":true}";
}
