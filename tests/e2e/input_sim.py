"""
input_sim.py — Human-like mouse and keyboard simulation via Windows SendInput API.

Features:
  - Smooth mouse movement with cubic Bezier curves (no teleport)
  - Variable speed with random micro-adjustments
  - Natural click timing (press→pause→release)
  - Drag operations (window dragging, selection)
  - Typing with realistic inter-key delays
  - Scroll wheel
"""
import ctypes
from ctypes import wintypes
import time
import random
import math
from typing import Tuple, Optional

# ── Windows API types and constants ──────────────────────────

user32 = ctypes.windll.user32
kernel32 = ctypes.windll.kernel32

# Screen dimensions (virtual screen)
SM_CXSCREEN = 0
SM_CYSCREEN = 1
_screen_w = user32.GetSystemMetrics(SM_CXSCREEN)
_screen_h = user32.GetSystemMetrics(SM_CYSCREEN)

# Normalized absolute coordinates: 0..65535
ABS_MAX = 65535

# SendInput constants
INPUT_MOUSE = 0
INPUT_KEYBOARD = 1
MOUSEEVENTF_MOVE = 0x0001
MOUSEEVENTF_LEFTDOWN = 0x0002
MOUSEEVENTF_LEFTUP = 0x0004
MOUSEEVENTF_RIGHTDOWN = 0x0008
MOUSEEVENTF_RIGHTUP = 0x0010
MOUSEEVENTF_ABSOLUTE = 0x8000
MOUSEEVENTF_WHEEL = 0x0800
KEYEVENTF_KEYDOWN = 0x0000  # actually 0
KEYEVENTF_KEYUP = 0x0002
KEYEVENTF_SCANCODE = 0x0008


class MOUSEINPUT(ctypes.Structure):
    _fields_ = [
        ("dx", wintypes.LONG),
        ("dy", wintypes.LONG),
        ("mouseData", wintypes.DWORD),
        ("dwFlags", wintypes.DWORD),
        ("time", wintypes.DWORD),
        ("dwExtraInfo", ctypes.POINTER(ctypes.c_ulong)),
    ]


class KEYBDINPUT(ctypes.Structure):
    _fields_ = [
        ("wVk", wintypes.WORD),
        ("wScan", wintypes.WORD),
        ("dwFlags", wintypes.DWORD),
        ("time", wintypes.DWORD),
        ("dwExtraInfo", ctypes.POINTER(ctypes.c_ulong)),
    ]


class INPUT_UNION(ctypes.Union):
    _fields_ = [("mi", MOUSEINPUT), ("ki", KEYBDINPUT)]


class INPUT(ctypes.Structure):
    _fields_ = [("type", wintypes.DWORD), ("union", INPUT_UNION)]


# ── Virtual key codes (common) ───────────────────────────────

VK_CODE = {
    'backspace': 0x08, 'tab': 0x09, 'enter': 0x0D, 'shift': 0x10,
    'ctrl': 0x11, 'alt': 0x12, 'pause': 0x13, 'caps': 0x14,
    'esc': 0x1B, 'space': 0x20, 'pageup': 0x21, 'pagedown': 0x22,
    'end': 0x23, 'home': 0x24, 'left': 0x25, 'up': 0x26,
    'right': 0x27, 'down': 0x28, 'delete': 0x2E,
    '0': 0x30, '1': 0x31, '2': 0x32, '3': 0x33, '4': 0x34,
    '5': 0x35, '6': 0x36, '7': 0x37, '8': 0x38, '9': 0x39,
    'a': 0x41, 'b': 0x42, 'c': 0x43, 'd': 0x44, 'e': 0x45,
    'f': 0x46, 'g': 0x47, 'h': 0x48, 'i': 0x49, 'j': 0x4A,
    'k': 0x4B, 'l': 0x4C, 'm': 0x4D, 'n': 0x4E, 'o': 0x4F,
    'p': 0x50, 'q': 0x51, 'r': 0x52, 's': 0x53, 't': 0x54,
    'u': 0x55, 'v': 0x56, 'w': 0x57, 'x': 0x58, 'y': 0x59,
    'z': 0x5A,
    'f1': 0x70, 'f2': 0x71, 'f3': 0x72, 'f4': 0x73,
    'f5': 0x74, 'f6': 0x75, 'f7': 0x76, 'f8': 0x77,
    'f9': 0x78, 'f10': 0x79, 'f11': 0x7A, 'f12': 0x7B,
}

SCAN_CODE = {
    'a': 0x1E, 'b': 0x30, 'c': 0x2E, 'd': 0x20, 'e': 0x12,
    'f': 0x21, 'g': 0x22, 'h': 0x23, 'i': 0x17, 'j': 0x24,
    'k': 0x25, 'l': 0x26, 'm': 0x32, 'n': 0x31, 'o': 0x18,
    'p': 0x19, 'q': 0x10, 'r': 0x13, 's': 0x1F, 't': 0x14,
    'u': 0x16, 'v': 0x2F, 'w': 0x11, 'x': 0x2D, 'y': 0x15,
    'z': 0x2C,
    '0': 0x0B, '1': 0x02, '2': 0x03, '3': 0x04, '4': 0x05,
    '5': 0x06, '6': 0x07, '7': 0x08, '8': 0x09, '9': 0x0A,
    'space': 0x39, 'enter': 0x1C, 'backspace': 0x0E,
    'tab': 0x0F, 'esc': 0x01,
    'left': 0x4B, 'right': 0x4D, 'up': 0x48, 'down': 0x50,
    'shift': 0x2A, 'ctrl': 0x1D, 'alt': 0x38,
}


def _send_input(*inputs: INPUT):
    """Send one or more INPUT structures via SendInput."""
    arr = (INPUT * len(inputs))(*inputs)
    user32.SendInput(len(inputs), arr, ctypes.sizeof(INPUT))


def _normalize(x: int, y: int) -> Tuple[int, int]:
    """Convert screen pixel coords to normalized absolute (0..65535)."""
    nx = int(x * ABS_MAX / _screen_w) if _screen_w > 0 else 0
    ny = int(y * ABS_MAX / _screen_h) if _screen_h > 0 else 0
    return nx, ny


def _mouse_input(flags: int, dx: int = 0, dy: int = 0, data: int = 0) -> INPUT:
    inp = INPUT()
    inp.type = INPUT_MOUSE
    inp.union.mi.dx = dx
    inp.union.mi.dy = dy
    inp.union.mi.mouseData = data
    inp.union.mi.dwFlags = flags
    inp.union.mi.time = 0
    inp.union.mi.dwExtraInfo = None
    return inp


def _key_input(flags: int, vk: int = 0, scan: int = 0) -> INPUT:
    inp = INPUT()
    inp.type = INPUT_KEYBOARD
    inp.union.ki.wVk = vk
    inp.union.ki.wScan = scan
    inp.union.ki.dwFlags = flags
    inp.union.ki.time = 0
    inp.union.ki.dwExtraInfo = None
    return inp


# ── Screen info ──────────────────────────────────────────────

def screen_size() -> Tuple[int, int]:
    """Return (width, height) of virtual screen."""
    return _screen_w, _screen_h


def cursor_pos() -> Tuple[int, int]:
    """Get current cursor position in screen coordinates."""
    pt = wintypes.POINT()
    user32.GetCursorPos(ctypes.byref(pt))
    return pt.x, pt.y


# ── Bezier curve mouse movement ──────────────────────────────

def _cubic_bezier(t: float, p0: float, p1: float, p2: float, p3: float) -> float:
    """Cubic Bezier: B(t) = (1-t)^3*P0 + 3(1-t)^2*t*P1 + 3(1-t)*t^2*P2 + t^3*P3"""
    u = 1.0 - t
    return u*u*u * p0 + 3*u*u*t * p1 + 3*u*t*t * p2 + t*t*t * p3


def move_to(x: int, y: int, duration: float = 0.3, steps: Optional[int] = None):
    """
    Move mouse cursor to (x, y) with human-like cubic Bezier curve.

    Args:
        x, y: Target screen coordinates (pixels).
        duration: Total movement time in seconds (0.1–2.0 is natural).
        steps: Number of interpolation steps (auto-calculated if None, ~200/s).
    """
    sx, sy = cursor_pos()
    if (sx, sy) == (x, y):
        return

    if steps is None:
        steps = max(20, int(duration * 200))

    # Random control points for natural-looking arc
    dx = x - sx
    dy = y - sy
    dist = math.sqrt(dx*dx + dy*dy)

    # Control points: offset perpendicular to direction for slight curve
    angle = math.atan2(dy, dx)
    curve_mag = dist * random.uniform(0.15, 0.35)
    # Random perpendicular offset (positive or negative)
    perp_angle = angle + math.pi/2 * random.choice([-1, 1])

    cp1_x = sx + dx * 0.25 + math.cos(perp_angle) * curve_mag * random.uniform(0.5, 1.0)
    cp1_y = sy + dy * 0.25 + math.sin(perp_angle) * curve_mag * random.uniform(0.5, 1.0)
    cp2_x = sx + dx * 0.75 + math.cos(perp_angle + random.uniform(-0.5, 0.5)) * curve_mag * 0.3
    cp2_y = sy + dy * 0.75 + math.sin(perp_angle + random.uniform(-0.5, 0.5)) * curve_mag * 0.3

    step_time = duration / steps
    for i in range(1, steps + 1):
        t = i / steps
        px = int(_cubic_bezier(t, sx, cp1_x, cp2_x, x))
        py = int(_cubic_bezier(t, sy, cp1_y, cp2_y, y))
        nx, ny = _normalize(px, py)
        _send_input(_mouse_input(MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE, nx, ny))

        # Micro-adjustments: small random jitter (human hand tremor)
        if random.random() < 0.15:
            jx = nx + random.randint(-50, 50)
            jy = ny + random.randint(-50, 50)
            _send_input(_mouse_input(MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE,
                                      max(0, min(ABS_MAX, jx)),
                                      max(0, min(ABS_MAX, jy))))

        # Variable speed: slow down near end (ease-out)
        delay = step_time
        if t > 0.7:
            delay *= 1.0 + (t - 0.7) * 3.0  # gradually slower
        time.sleep(delay)


def move_relative(dx: int, dy: int, duration: float = 0.15):
    """Move mouse relative to current position."""
    cx, cy = cursor_pos()
    move_to(cx + dx, cy + dy, duration)


# ── Clicks ───────────────────────────────────────────────────

def click(x: Optional[int] = None, y: Optional[int] = None,
          button: str = 'left', duration: float = 0.2):
    """
    Click at position with natural timing. Moves to (x,y) first if provided.

    Args:
        x, y: Target position (None = current position).
        button: 'left' or 'right'.
        duration: Time between press and release.
    """
    if x is not None and y is not None:
        move_to(x, y)

    down_flag = MOUSEEVENTF_LEFTDOWN if button == 'left' else MOUSEEVENTF_RIGHTDOWN
    up_flag = MOUSEEVENTF_LEFTUP if button == 'left' else MOUSEEVENTF_RIGHTUP

    _send_input(_mouse_input(down_flag))
    press_time = duration * random.uniform(0.3, 0.5)
    time.sleep(press_time)
    _send_input(_mouse_input(up_flag))
    time.sleep(duration * 0.5)


def double_click(x: Optional[int] = None, y: Optional[int] = None):
    """Double-click at position."""
    if x is not None and y is not None:
        move_to(x, y)
    click()
    time.sleep(random.uniform(0.05, 0.1))
    click()


def right_click(x: Optional[int] = None, y: Optional[int] = None):
    """Right-click at position."""
    click(x, y, button='right')


# ── Drag ─────────────────────────────────────────────────────

def drag(start_x: int, start_y: int, end_x: int, end_y: int,
         duration: float = 0.6):
    """
    Drag from (start_x, start_y) to (end_x, end_y).
    Useful for window dragging, text selection, etc.
    """
    move_to(start_x, start_y, duration=0.15)
    time.sleep(random.uniform(0.05, 0.1))
    _send_input(_mouse_input(MOUSEEVENTF_LEFTDOWN))
    time.sleep(random.uniform(0.03, 0.07))
    move_to(end_x, end_y, duration=duration)
    time.sleep(random.uniform(0.05, 0.1))
    _send_input(_mouse_input(MOUSEEVENTF_LEFTUP))


def drag_window(hwnd_title_bar_x: int, hwnd_title_bar_y: int,
                dst_x: int, dst_y: int, duration: float = 0.5):
    """Drag a window by its title bar from one position to another."""
    drag(hwnd_title_bar_x, hwnd_title_bar_y, dst_x, dst_y, duration)


# ── Scroll ───────────────────────────────────────────────────

def scroll(clicks: int, x: Optional[int] = None, y: Optional[int] = None):
    """
    Scroll mouse wheel. Positive = scroll up, negative = scroll down.
    One click = one notch.
    """
    if x is not None and y is not None:
        move_to(x, y)
    WHEEL_DELTA = 120
    _send_input(_mouse_input(MOUSEEVENTF_WHEEL, data=clicks * WHEEL_DELTA))


# ── Keyboard ─────────────────────────────────────────────────

def key_down(key: str):
    """Press and hold a key."""
    key = key.lower()
    vk = VK_CODE.get(key, 0)
    scan = SCAN_CODE.get(key, 0)
    if scan:
        _send_input(_key_input(KEYEVENTF_SCANCODE, vk=vk, scan=scan))
    elif vk:
        _send_input(_key_input(0, vk=vk))


def key_up(key: str):
    """Release a key."""
    key = key.lower()
    vk = VK_CODE.get(key, 0)
    scan = SCAN_CODE.get(key, 0)
    if scan:
        _send_input(_key_input(KEYEVENTF_KEYUP | KEYEVENTF_SCANCODE, vk=vk, scan=scan))
    elif vk:
        _send_input(_key_input(KEYEVENTF_KEYUP, vk=vk))


def press(key: str, duration: float = 0.08):
    """Press and release a single key."""
    key_down(key)
    time.sleep(duration * random.uniform(0.8, 1.2))
    key_up(key)


def combo(*keys: str, hold_duration: float = 0.1):
    """Press a key combination (e.g. combo('ctrl', 'c')). All keys held, last pressed first released."""
    for k in keys:
        key_down(k)
        time.sleep(0.02)
    time.sleep(hold_duration)
    for k in reversed(keys):
        key_up(k)
        time.sleep(0.02)


def type_text(text: str, wpm: int = 60):
    """
    Type a string with realistic inter-key delays.

    Args:
        text: The string to type (ASCII only — uses scan codes).
        wpm: Words per minute (affects typing speed). 60 = ~200ms per char.
    """
    # Base delay: 5 chars per word, wpm words per minute
    base_delay = 60.0 / (wpm * 5)  # seconds per character
    for ch in text:
        if ch.isupper() or ch in '~!@#$%^&*()_+{}|:"<>?':
            key_down('shift')
            time.sleep(0.02)
        key = ch.lower()
        vk = VK_CODE.get(key, 0)
        scan = SCAN_CODE.get(key, 0)
        if scan:
            _send_input(_key_input(KEYEVENTF_SCANCODE, vk=vk, scan=scan))
            time.sleep(base_delay * random.uniform(0.3, 0.6))
            _send_input(_key_input(KEYEVENTF_KEYUP | KEYEVENTF_SCANCODE, vk=vk, scan=scan))
        elif vk:
            _send_input(_key_input(0, vk=vk))
            time.sleep(base_delay * random.uniform(0.3, 0.6))
            _send_input(_key_input(KEYEVENTF_KEYUP, vk=vk))
        if ch.isupper() or ch in '~!@#$%^&*()_+{}|:"<>?':
            key_up('shift')
        # Variable inter-key delay
        time.sleep(base_delay * random.uniform(0.5, 1.5))


# ── Higher-level actions ─────────────────────────────────────

def draw_circle(cx: int, cy: int, radius: int, duration: float = 2.0):
    """Draw a circle with the mouse (like a human tracing a circle)."""
    n_points = max(30, int(duration * 50))
    for i in range(1, n_points + 1):
        angle = 2 * math.pi * i / n_points
        # Add some imperfection
        r = radius + random.randint(-3, 3)
        px = int(cx + r * math.cos(angle))
        py = int(cy + r * math.sin(angle) * 0.7)  # slightly elliptical (natural)
        nx, ny = _normalize(px, py)
        _send_input(_mouse_input(MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE, nx, ny))
        time.sleep(duration / n_points * random.uniform(0.8, 1.2))


def wiggle(duration: float = 0.5, amplitude: int = 5):
    """Small random mouse wiggles (like a user thinking/reading)."""
    cx, cy = cursor_pos()
    steps = int(duration * 30)
    for _ in range(steps):
        dx = random.randint(-amplitude, amplitude)
        dy = random.randint(-amplitude, amplitude)
        nx, ny = _normalize(cx + dx, cy + dy)
        _send_input(_mouse_input(MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE, nx, ny))
        time.sleep(duration / steps)


def random_desktop_activity(duration: float = 5.0):
    """
    Simulate random human desktop activity: mouse moves, clicks, scrolls, typing.
    Useful for generating dynamic capture content.
    """
    sw, sh = screen_size()
    start = time.time()
    actions = 0
    while time.time() - start < duration:
        action = random.choice(['move', 'click', 'scroll', 'type', 'drag', 'wiggle'])
        if action == 'move':
            tx = random.randint(sw // 4, sw * 3 // 4)
            ty = random.randint(sh // 4, sh * 3 // 4)
            move_to(tx, ty, duration=random.uniform(0.3, 1.0))
        elif action == 'click':
            tx = random.randint(sw // 4, sw * 3 // 4)
            ty = random.randint(sh // 4, sh * 3 // 4)
            click(tx, ty)
        elif action == 'scroll':
            scroll(random.randint(-3, 3))
        elif action == 'type':
            texts = ["hello world", "test input", "game agent", "hello"]
            type_text(random.choice(texts), wpm=random.randint(40, 80))
        elif action == 'drag':
            sx = random.randint(sw // 4, sw * 3 // 4)
            sy = random.randint(sh // 4, sh * 3 // 4)
            ex = sx + random.randint(-200, 200)
            ey = sy + random.randint(-100, 100)
            drag(sx, sy, ex, ey, duration=random.uniform(0.3, 0.7))
        elif action == 'wiggle':
            wiggle(duration=random.uniform(0.2, 0.8))
        actions += 1
        time.sleep(random.uniform(0.1, 0.5))
    print(f"[input] {actions} actions in {duration:.1f}s")
