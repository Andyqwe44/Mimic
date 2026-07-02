/**
 * SendInput Input Backend (Fallback)
 *
 * Uses Win32 SendInput() API. Simple and works for most single-player games.
 * Detectable by anti-cheat (LLMHF_INJECTED flag).
 */
#include "input.hpp"
#include <windows.h>
#include <cstdio>

class SendInputBackend : public IInputBackend {
public:
    const char* name() const override { return "SendInput (Win32)"; }
    bool init() override { return true; }

    bool send_action(const GameAction& a) override {
        switch (a.type) {
        case GameAction::KeyDown:       return key_event(a.vk_code, 0);
        case GameAction::KeyUp:         return key_event(a.vk_code, KEYEVENTF_KEYUP);
        case GameAction::KeyTap:
            return key_event(a.vk_code, 0) &&
                   wait_ms(a.wait_ms) &&
                   key_event(a.vk_code, KEYEVENTF_KEYUP);
        case GameAction::MouseMove:     return mouse_to(a.x, a.y);
        case GameAction::MouseMoveRelative: return mouse_delta(a.dx, a.dy);
        case GameAction::MouseDown:     return mouse_button(btn_flag(a.btn, true));
        case GameAction::MouseUp:       return mouse_button(btn_flag(a.btn, false));
        case GameAction::MouseClick:
            return mouse_to(a.x, a.y) && wait_ms(10) && mouse_click(a.btn);
        case GameAction::Wait:          return wait_ms(a.wait_ms);
        }
        return false;
    }

    bool move_mouse(int x, int y) override { return mouse_to(x, y); }
    bool click(MouseButton btn) override    { return mouse_click(btn); }
    bool key_press(uint16_t vk) override    { return key_event(vk, 0); }
    bool key_release(uint16_t vk) override  { return key_event(vk, KEYEVENTF_KEYUP); }
    bool key_tap(uint16_t vk, int ms) override {
        return key_event(vk, 0) && wait_ms(ms > 0 ? ms : 50) && key_event(vk, KEYEVENTF_KEYUP);
    }
    void shutdown() override {}

private:
    bool wait_ms(int ms) { if (ms > 0) Sleep((DWORD)ms); return true; }

    bool key_event(uint16_t vk, DWORD flags) {
        INPUT input = {};
        input.type = INPUT_KEYBOARD;
        input.ki.wVk = vk;
        input.ki.dwFlags = flags;
        return SendInput(1, &input, sizeof(INPUT)) > 0;
    }

    bool mouse_to(int x, int y) {
        int sw = GetSystemMetrics(SM_CXSCREEN);
        int sh = GetSystemMetrics(SM_CYSCREEN);
        INPUT input = {};
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
        input.mi.dx = (LONG)((double)x / sw * 65535);
        input.mi.dy = (LONG)((double)y / sh * 65535);
        return SendInput(1, &input, sizeof(INPUT)) > 0;
    }

    bool mouse_delta(int dx, int dy) {
        INPUT input = {};
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = MOUSEEVENTF_MOVE;
        input.mi.dx = dx;
        input.mi.dy = dy;
        return SendInput(1, &input, sizeof(INPUT)) > 0;
    }

    bool mouse_click(MouseButton btn) {
        DWORD d = btn_flag(btn, true);
        DWORD u = btn_flag(btn, false);
        return mouse_button(d) && wait_ms(30) && mouse_button(u) && wait_ms(20);
    }

    bool mouse_button(DWORD flags) {
        INPUT input = {};
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = flags;
        return SendInput(1, &input, sizeof(INPUT)) > 0;
    }

    static DWORD btn_flag(MouseButton btn, bool down) {
        if (btn == MouseButton::Left)
            return down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
        if (btn == MouseButton::Right)
            return down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
        return down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
    }
};

// ==================== Factory ====================

std::unique_ptr<IInputBackend> create_input_backend() {
    // Try Interception first (dynamic load)
    // The InterceptionBackend class is in input_interception.cpp
    // For now, default to SendInput
    auto si = std::make_unique<SendInputBackend>();
    si->init();
    return si;
}

// ==================== VK Name Lookup ====================

const char* vk_name(uint16_t vk) {
    switch (vk) {
    case VK_RETURN:  return "Enter";
    case VK_SPACE:   return "Space";
    case VK_ESCAPE:  return "Escape";
    case VK_TAB:     return "Tab";
    case VK_SHIFT:   return "Shift";
    case VK_CONTROL: return "Ctrl";
    case VK_MENU:    return "Alt";
    case VK_LEFT:    return "Left";
    case VK_RIGHT:   return "Right";
    case VK_UP:      return "Up";
    case VK_DOWN:    return "Down";
    case 'W': case 'w': return "W";
    case 'A': case 'a': return "A";
    case 'S': case 's': return "S";
    case 'D': case 'd': return "D";
    default:         return "?";
    }
}
