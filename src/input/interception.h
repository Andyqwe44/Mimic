/**
 * Interception Driver API (minimal subset)
 *
 * Interception is a kernel-level keyboard/mouse filter driver.
 * Events injected via interception_send() are indistinguishable
 * from real hardware input — undetectable by anti-cheat systems.
 *
 * Download: https://github.com/oblitum/Interception/releases
 * Install:  install-interception.exe (admin)
 *            driver goes to C:\Windows\System32\drivers\interception.sys
 *
 * Dev mode: bcdedit /set testsigning on  (driver needs test signing)
 */
#pragma once
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Device types */
enum InterceptionDeviceType {
    INTERCEPTION_KEYBOARD = 0,
    INTERCEPTION_MOUSE    = 1,
};

/* Filter mode for interception_set_filter */
enum InterceptionFilterKeyState {
    INTERCEPTION_FILTER_KEY_NONE             = 0x0000,
    INTERCEPTION_FILTER_KEY_ALL              = 0xFFFF,
    INTERCEPTION_FILTER_KEY_DOWN             = 0x0100,
    INTERCEPTION_FILTER_KEY_UP               = 0x0200,
    INTERCEPTION_FILTER_KEY_E0               = 0x0400,
    INTERCEPTION_FILTER_KEY_E1               = 0x0800,
    INTERCEPTION_FILTER_KEY_TERMSRV_SET_LED  = 0x1000,
    INTERCEPTION_FILTER_KEY_TERMSRV_SHADOW   = 0x2000,
    INTERCEPTION_FILTER_KEY_TERMSRV_VKPACKET = 0x4000,
};

enum InterceptionFilterMouseState {
    INTERCEPTION_FILTER_MOUSE_NONE               = 0x0000,
    INTERCEPTION_FILTER_MOUSE_ALL                = 0xFFFF,
    INTERCEPTION_FILTER_MOUSE_LEFT_BUTTON_DOWN   = 0x0001,
    INTERCEPTION_FILTER_MOUSE_LEFT_BUTTON_UP     = 0x0002,
    INTERCEPTION_FILTER_MOUSE_RIGHT_BUTTON_DOWN  = 0x0004,
    INTERCEPTION_FILTER_MOUSE_RIGHT_BUTTON_UP    = 0x0008,
    INTERCEPTION_FILTER_MOUSE_MIDDLE_BUTTON_DOWN = 0x0010,
    INTERCEPTION_FILTER_MOUSE_MIDDLE_BUTTON_UP   = 0x0020,
    INTERCEPTION_FILTER_MOUSE_BUTTON_1_DOWN      = 0x0040,
    INTERCEPTION_FILTER_MOUSE_BUTTON_1_UP        = 0x0080,
    INTERCEPTION_FILTER_MOUSE_BUTTON_2_DOWN      = 0x0100,
    INTERCEPTION_FILTER_MOUSE_BUTTON_2_UP        = 0x0200,
    INTERCEPTION_FILTER_MOUSE_BUTTON_3_DOWN      = 0x0400,
    INTERCEPTION_FILTER_MOUSE_BUTTON_3_UP        = 0x0800,
    INTERCEPTION_FILTER_MOUSE_MOVE               = 0x1000,
};

enum InterceptionKeyState {
    INTERCEPTION_KEY_DOWN             = 0x00,
    INTERCEPTION_KEY_UP               = 0x01,
    INTERCEPTION_KEY_E0               = 0x02,
    INTERCEPTION_KEY_E1               = 0x04,
    INTERCEPTION_KEY_TERMSRV_SET_LED  = 0x08,
    INTERCEPTION_KEY_TERMSRV_SHADOW   = 0x10,
    INTERCEPTION_KEY_TERMSRV_VKPACKET = 0x20,
};

enum InterceptionMouseState {
    INTERCEPTION_MOUSE_LEFT_BUTTON_DOWN   = 0x001,
    INTERCEPTION_MOUSE_LEFT_BUTTON_UP     = 0x002,
    INTERCEPTION_MOUSE_RIGHT_BUTTON_DOWN  = 0x004,
    INTERCEPTION_MOUSE_RIGHT_BUTTON_UP    = 0x008,
    INTERCEPTION_MOUSE_MIDDLE_BUTTON_DOWN = 0x010,
    INTERCEPTION_MOUSE_MIDDLE_BUTTON_UP   = 0x020,
    INTERCEPTION_MOUSE_BUTTON_1_DOWN      = 0x040,
    INTERCEPTION_MOUSE_BUTTON_1_UP        = 0x080,
    INTERCEPTION_MOUSE_BUTTON_2_DOWN      = 0x100,
    INTERCEPTION_MOUSE_BUTTON_2_UP        = 0x200,
    INTERCEPTION_MOUSE_BUTTON_3_DOWN      = 0x400,
    INTERCEPTION_MOUSE_BUTTON_3_UP        = 0x800,
    INTERCEPTION_MOUSE_MOVE               = 0x000,
};

enum InterceptionMouseFlag {
    INTERCEPTION_MOUSE_MOVE_RELATIVE      = 0x000,
    INTERCEPTION_MOUSE_MOVE_ABSOLUTE      = 0x001,
};

typedef int  InterceptionContext;
typedef int  InterceptionDevice;
typedef int  InterceptionPrecedence;
typedef WORD InterceptionFilter;

typedef struct {
    unsigned short state;
    unsigned short flags;
    short rolling;
    int   x;
    int   y;
    unsigned int  information;
} InterceptionMouseStroke;

typedef struct {
    unsigned short code;
    unsigned short state;
    unsigned int  information;
} InterceptionKeyboardStroke;

/* ---- Public API ---- */
InterceptionContext interception_create_context(void);
void                 interception_destroy_context(InterceptionContext context);
InterceptionPrecedence interception_get_precedence(InterceptionContext context, InterceptionDevice device);
void                    interception_set_precedence(InterceptionContext context, InterceptionDevice device, InterceptionPrecedence precedence);
InterceptionFilter      interception_get_filter(InterceptionContext context, InterceptionDevice device);
void                    interception_set_filter(InterceptionContext context, InterceptionDevice device, InterceptionFilter filter);
InterceptionDevice      interception_wait(InterceptionContext context);
InterceptionDevice      interception_wait_with_timeout(InterceptionContext context, unsigned long milliseconds);
int                     interception_send(InterceptionContext context, InterceptionDevice device,
                                          const void* stroke, unsigned int nstroke);
int                     interception_receive(InterceptionContext context, InterceptionDevice device,
                                             void* stroke, unsigned int nstroke);
unsigned int            interception_get_hardware_id(InterceptionContext context, InterceptionDevice device);
int                     interception_is_keyboard(InterceptionDevice device);
int                     interception_is_mouse(InterceptionDevice device);
int                     interception_is_invalid(InterceptionDevice device);

/* Hardware IDs for specific keyboards/mice (optional filtering) */
#define INTERCEPTION_HARDWARE_ID_ROOT         0
#define INTERCEPTION_DEFAULT_HARDWARE_ID      0

#ifdef __cplusplus
}
#endif
