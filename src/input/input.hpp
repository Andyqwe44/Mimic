/**
 * Input Simulation Middleware - Abstract Interface
 *
 * Clean separation between semantic game actions and their
 * low-level implementation (Interception driver, SendInput, etc.)
 *
 * Backends:
 *   - Interception driver (primary, kernel-level, bypasses anti-cheat)
 *   - SendInput (fallback, OS-level, detectable)
 */
#pragma once
#include <cstdint>
#include <vector>
#include <memory>
#include "../common/types.hpp"

// --- Action Types ---

enum class MouseButton { Left = 0, Right = 1, Middle = 2 };

struct GameAction {
    enum Type {
        KeyDown, KeyUp, KeyTap,
        MouseMove, MouseMoveRelative,
        MouseDown, MouseUp, MouseClick,
        Wait
    };

    Type type = Wait;
    uint16_t  vk_code = 0;   // virtual key code (KeyDown/Up/Tap)
    int       x = 0;          // pixel X (MouseMove, MouseClick)
    int       y = 0;          // pixel Y (MouseMove, MouseClick)
    int       dx = 0;         // delta X (MouseMoveRelative)
    int       dy = 0;         // delta Y (MouseMoveRelative)
    int       wait_ms = 0;    // duration (Wait) or key duration (KeyTap)
    MouseButton btn = MouseButton::Left;

    // Factory methods
    static GameAction key_down(uint16_t vk) {
        GameAction a; a.type = KeyDown; a.vk_code = vk; return a;
    }
    static GameAction key_up(uint16_t vk) {
        GameAction a; a.type = KeyUp; a.vk_code = vk; return a;
    }
    static GameAction key_tap(uint16_t vk, int dur_ms = 50) {
        GameAction a; a.type = KeyTap; a.vk_code = vk; a.wait_ms = dur_ms; return a;
    }
    static GameAction move_to(int x_, int y_) {
        GameAction a; a.type = MouseMove; a.x = x_; a.y = y_; return a;
    }
    static GameAction move_rel(int dx_, int dy_) {
        GameAction a; a.type = MouseMoveRelative; a.dx = dx_; a.dy = dy_; return a;
    }
    static GameAction click_at(int x_, int y_, MouseButton btn_ = MouseButton::Left) {
        GameAction a; a.type = MouseClick; a.x = x_; a.y = y_; a.btn = btn_; return a;
    }
    static GameAction btn_down(MouseButton btn_) {
        GameAction a; a.type = MouseDown; a.btn = btn_; return a;
    }
    static GameAction btn_up(MouseButton btn_) {
        GameAction a; a.type = MouseUp; a.btn = btn_; return a;
    }
    static GameAction wait_for(int ms) {
        GameAction a; a.type = Wait; a.wait_ms = ms; return a;
    }
};

// --- Abstract Backend ---

class IInputBackend {
public:
    virtual ~IInputBackend() = default;

    virtual bool init() = 0;
    virtual bool send_action(const GameAction& a) = 0;

    virtual bool send_actions(const std::vector<GameAction>& actions) {
        for (auto& a : actions)
            if (!send_action(a)) return false;
        return true;
    }

    virtual bool move_mouse(int x, int y) = 0;
    virtual bool click(MouseButton btn = MouseButton::Left) = 0;
    virtual bool key_press(uint16_t vk) = 0;
    virtual bool key_release(uint16_t vk) = 0;
    virtual bool key_tap(uint16_t vk, int dur_ms = 50) = 0;

    virtual const char* name() const = 0;
    virtual void shutdown() = 0;
};

// --- Factory ---

std::unique_ptr<IInputBackend> create_input_backend();

const char* vk_name(uint16_t vk);
