/**
 * Interception Driver Input Backend
 *
 * Kernel-level keyboard/mouse filter driver.
 * Uses dynamic loading (LoadLibrary) to avoid hard link dependency.
 *
 * Download: https://github.com/oblitum/Interception/releases
 * Install as Administrator. Dev mode: bcdedit /set testsigning on
 */
#include "input.hpp"
#include <windows.h>
#include <cstdio>
#include "../../logger/logger.h"

// --- Interception API types ---
typedef int  InterceptionContext;
typedef int  InterceptionDevice;
typedef WORD InterceptionFilter;

#pragma pack(push, 1)
typedef struct {
    unsigned short code;
    unsigned short state;
    unsigned int  information;
} I_KbdStroke;

typedef struct {
    unsigned short state;
    unsigned short flags;
    short rolling;
    int   x;
    int   y;
    unsigned int  information;
} I_MouseStroke;
#pragma pack(pop)

enum {
    I_KEYBOARD = 0, I_MOUSE = 1,
    I_FILTER_KEY_ALL  = 0xFFFF, I_FILTER_MOUSE_ALL = 0xFFFF,
    I_KEY_DOWN = 0x00, I_KEY_UP = 0x01,
    I_MOUSE_MOVE              = 0x000,
    I_MOUSE_MOVE_ABSOLUTE     = 0x001,
    I_MOUSE_LEFT_BUTTON_DOWN  = 0x001,
    I_MOUSE_LEFT_BUTTON_UP    = 0x002,
    I_MOUSE_RIGHT_BUTTON_DOWN = 0x004,
    I_MOUSE_RIGHT_BUTTON_UP   = 0x008,
    I_MOUSE_MIDDLE_BUTTON_DOWN= 0x010,
    I_MOUSE_MIDDLE_BUTTON_UP  = 0x020,
};

typedef InterceptionContext (*PFN_create_context)(void);
typedef void (*PFN_destroy_context)(InterceptionContext);
typedef void (*PFN_set_filter)(InterceptionContext, InterceptionDevice, InterceptionFilter);
typedef int  (*PFN_is_invalid)(InterceptionDevice);
typedef int  (*PFN_send)(InterceptionContext, InterceptionDevice, const void*, unsigned int);

class InterceptionBackend : public IInputBackend {
public:
    const char* name() const override { return "Interception Driver"; }

    bool init() override {
        dll_ = LoadLibraryA("interception.dll");
        if (!dll_) {
            LOG("input", "Interception: interception.dll not found");
            return false;
        }
        #define LOAD(fn) pfn_##fn = (PFN_##fn)GetProcAddress(dll_, #fn); \
            if (!pfn_##fn) { LOG("input", "Interception: missing %s", #fn); return false; }
        LOAD(interception_create_context);
        LOAD(interception_destroy_context);
        LOAD(interception_set_filter);
        LOAD(interception_is_invalid);
        LOAD(interception_send);
        #undef LOAD

        ctx_ = pfn_interception_create_context();
        if (!ctx_) { LOG("input", "Interception: create_context failed. Run as Admin?"); return false; }

        pfn_interception_set_filter(ctx_, I_KEYBOARD, I_FILTER_KEY_ALL);
        pfn_interception_set_filter(ctx_, I_MOUSE, I_FILTER_MOUSE_ALL);
        kbd_dev_ = 1; mouse_dev_ = 2;
        return true;
    }

    bool send_action(const GameAction& a) override {
        switch (a.type) {
        case GameAction::KeyDown:  return kbd(a.vk_code, I_KEY_DOWN);
        case GameAction::KeyUp:    return kbd(a.vk_code, I_KEY_UP);
        case GameAction::KeyTap:
            return kbd(a.vk_code, I_KEY_DOWN) && wait_ms(a.wait_ms ? a.wait_ms : 50) && kbd(a.vk_code, I_KEY_UP);
        case GameAction::MouseMove: return mouse_abs(a.x, a.y);
        case GameAction::MouseMoveRelative: return mouse_rel(a.dx, a.dy);
        case GameAction::MouseDown: return mbtn(a.btn, true);
        case GameAction::MouseUp:   return mbtn(a.btn, false);
        case GameAction::MouseClick:
            return mouse_abs(a.x, a.y) && wait_ms(10) && mclick(a.btn);
        case GameAction::Wait: return wait_ms(a.wait_ms);
        }
        return false;
    }

    bool move_mouse(int x, int y) override  { return mouse_abs(x, y); }
    bool click(MouseButton btn) override     { return mclick(btn); }
    bool key_press(uint16_t vk) override     { return kbd(vk, I_KEY_DOWN); }
    bool key_release(uint16_t vk) override   { return kbd(vk, I_KEY_UP); }
    bool key_tap(uint16_t vk, int ms) override {
        return kbd(vk, I_KEY_DOWN) && wait_ms(ms > 0 ? ms : 50) && kbd(vk, I_KEY_UP);
    }

    void shutdown() override {
        if (ctx_) pfn_interception_destroy_context(ctx_);
        ctx_ = 0;
        if (dll_) { FreeLibrary(dll_); dll_ = nullptr; }
    }

private:
    bool wait_ms(int ms) { if (ms > 0) Sleep((DWORD)ms); return true; }

    bool kbd(uint16_t code, uint16_t state) {
        I_KbdStroke s = { code, state, 0 };
        return pfn_interception_send(ctx_, kbd_dev_, &s, 1) > 0;
    }
    bool mouse_abs(int x, int y) {
        I_MouseStroke s = {};
        s.state = I_MOUSE_MOVE;
        s.flags = I_MOUSE_MOVE_ABSOLUTE;
        s.x = x; s.y = y;
        return pfn_interception_send(ctx_, mouse_dev_, &s, 1) > 0;
    }
    bool mouse_rel(int dx, int dy) {
        I_MouseStroke s = {};
        s.state = I_MOUSE_MOVE;
        s.flags = 0;
        s.x = dx; s.y = dy;
        return pfn_interception_send(ctx_, mouse_dev_, &s, 1) > 0;
    }
    bool mbtn(MouseButton btn, bool down) {
        I_MouseStroke s = {};
        s.flags = I_MOUSE_MOVE_ABSOLUTE;
        if (btn == MouseButton::Left)
            s.state = down ? I_MOUSE_LEFT_BUTTON_DOWN : I_MOUSE_LEFT_BUTTON_UP;
        else if (btn == MouseButton::Right)
            s.state = down ? I_MOUSE_RIGHT_BUTTON_DOWN : I_MOUSE_RIGHT_BUTTON_UP;
        else
            s.state = down ? I_MOUSE_MIDDLE_BUTTON_DOWN : I_MOUSE_MIDDLE_BUTTON_UP;
        return pfn_interception_send(ctx_, mouse_dev_, &s, 1) > 0;
    }
    bool mclick(MouseButton btn) {
        return mbtn(btn, true) && wait_ms(30) && mbtn(btn, false) && wait_ms(20);
    }

    HMODULE dll_ = nullptr;
    InterceptionContext ctx_ = 0;
    InterceptionDevice kbd_dev_ = 0, mouse_dev_ = 0;
    PFN_create_context  pfn_interception_create_context  = nullptr;
    PFN_destroy_context pfn_interception_destroy_context = nullptr;
    PFN_set_filter      pfn_interception_set_filter      = nullptr;
    PFN_is_invalid      pfn_interception_is_invalid      = nullptr;
    PFN_send            pfn_interception_send            = nullptr;
};

// Re-export factory from this file when interception is compiled in
// (The weak factory in input_sendinput.cpp will be overridden at link time)
extern std::unique_ptr<IInputBackend> create_input_backend_interception() {
    auto be = std::make_unique<InterceptionBackend>();
    if (be->init()) return be;
    return nullptr;
}
