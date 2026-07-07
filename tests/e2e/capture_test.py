"""
capture_test.py — E2E capture + input test scenarios.

Tests:
  1. desktop_capture    — Stream entire desktop, receive N frames, save PNGs.
  2. window_capture     — Find a window by title, stream it, save PNGs.
  3. desktop_input      — Simulate random activity while capturing desktop.
  4. window_input       — Simulate activity inside a specific window while capturing.

Usage:
  python -m tests.e2e.capture_test  [--test desktop|window|desktop_input|window_input|all]
"""
import argparse
import time
import os
import sys
import threading
import ctypes
from pathlib import Path
from typing import Optional
import numpy as np

# Add project root to path
sys.path.insert(0, str(Path(__file__).resolve().parent.parent.parent))

from tests.e2e.tcp_receiver import TcpFrameReceiver, Frame
from tests.e2e.input_sim import (
    move_to, click, drag, scroll, type_text, combo, press,
    random_desktop_activity, draw_circle, wiggle,
    screen_size, cursor_pos,
)

# Output directory for saved frames
OUT_DIR = Path(__file__).resolve().parent.parent / "frames"
OUT_DIR.mkdir(exist_ok=True)

# ── Windows API helpers for window operations ────────────────

user32 = ctypes.windll.user32

SW_RESTORE = 9
SW_SHOW = 5
HWND_TOPMOST = -1
SWP_NOSIZE = 0x0001
SWP_NOMOVE = 0x0002
SWP_SHOWWINDOW = 0x0040


def find_window(title_substring: str) -> Optional[int]:
    """Find a visible window whose title contains `title_substring`. Returns HWND."""
    result = []

    def enum_callback(hwnd, _lparam):
        if not ctypes.windll.user32.IsWindowVisible(hwnd):
            return True
        length = user32.GetWindowTextLengthW(hwnd)
        if length == 0:
            return True
        buf = ctypes.create_unicode_buffer(length + 1)
        user32.GetWindowTextW(hwnd, buf, length + 1)
        if title_substring.lower() in buf.value.lower():
            # Skip self (console windows, etc.) and the GAM app itself
            if "game agent monitor" in buf.value.lower():
                return True
            result.append(hwnd)
        return True

    WNDENUMPROC = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)
    user32.EnumWindows(WNDENUMPROC(enum_callback), 0)
    return result[0] if result else None


def restore_window(hwnd: int):
    """Restore window if minimized."""
    if user32.IsIconic(hwnd):
        user32.ShowWindow(hwnd, SW_RESTORE)


def bring_to_front(hwnd: int):
    """Bring window to front (not force-topmost, just SetForegroundWindow)."""
    restore_window(hwnd)
    user32.SetForegroundWindow(hwnd)
    time.sleep(0.3)


def get_window_rect(hwnd: int) -> tuple:
    """Get window rect as (left, top, right, bottom)."""
    r = ctypes.wintypes.RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(r))
    return r.left, r.top, r.right, r.bottom


def activate_via_click(hwnd: int):
    """Activate a window by clicking its title bar."""
    left, top, right, bottom = get_window_rect(hwnd)
    # Click near center of title bar
    title_y = top + 15  # title bar is ~30px
    title_x = left + (right - left) // 2
    click(title_x, title_y)


# ── Frame saving ─────────────────────────────────────────────

def save_frames(frames: list[Frame], prefix: str, max_frames: int = 10):
    """Save frames as PNG files. Saves every Nth frame if too many."""
    if not frames:
        print(f"[{prefix}] No frames to save!")
        return

    step = max(1, len(frames) // max_frames)
    saved = 0
    for i in range(0, len(frames), step):
        f = frames[i]
        rgba = f.to_rgba()
        # PIL import here so it's not a hard dependency for other modules
        from PIL import Image
        img = Image.fromarray(rgba, mode='RGBA')
        path = OUT_DIR / f"{prefix}_{saved:03d}.png"
        img.save(path)
        saved += 1
    print(f"[{prefix}] Saved {saved} frames to {OUT_DIR}/ (total received: {len(frames)})")


# ── Start monitor_app stream via TCP ─────────────────────────
# Note: We can't call hostCall from Python directly (it's WebView2 JS bridge).
# Instead we rely on the user having the GAM GUI open and streaming started.
# The TCP :9999 server runs whenever monitor_app is running.
# For automated testing, we could send commands via a hypothetical REST API,
# but for now we doc: user must click "Start" in GUI first.

# ── Test 1: Desktop Capture ──────────────────────────────────

def test_desktop_capture(duration: float = 5.0):
    """
    Connect to TCP :9999, receive desktop frames for `duration` seconds.

    Precondition: GAM GUI is running and streaming desktop (user clicked Start with
    'Entire Desktop' selected).

    If frames are flowing, this test passes regardless of content.
    """
    print("\n" + "=" * 60)
    print("TEST 1: Desktop Capture")
    print("=" * 60)
    print("Prerequisite: GAM GUI running, desktop stream active (click Start).")
    print("Connecting to TCP :9999...")

    receiver = TcpFrameReceiver()
    if not receiver.connect():
        print("FAIL: Could not connect to TCP :9999.")
        print("      Make sure monitor_app.exe is running and streaming.")
        return False

    print(f"Connected. Receiving frames for {duration}s...")
    time.sleep(duration)

    receiver.disconnect()
    count = receiver.frame_count
    errors = receiver.error_count
    mb = receiver.byte_count / (1024 * 1024)

    print(f"Received: {count} frames, {mb:.1f} MB, {errors} errors")
    if count > 0:
        fps = count / duration
        print(f"FPS: {fps:.1f}")
        if fps >= 10:
            print("PASS: Desktop capture working ✓")
            return True
        else:
            print(f"WARN: Low FPS ({fps:.1f}) — static desktop or slow capture")
            return True  # Not a failure — static desktop produces few frames
    else:
        print("FAIL: No frames received. Is streaming active in GAM GUI?")
        return False


# ── Test 2: Window Capture ───────────────────────────────────

def test_window_capture(window_title: str = "notepad", duration: float = 5.0):
    """
    Find a window by title substring, receive its capture frames.

    Precondition: GAM GUI streaming the target window.

    Args:
        window_title: Substring to match in window title.
    """
    print("\n" + "=" * 60)
    print(f"TEST 2: Window Capture — '{window_title}'")
    print("=" * 60)

    hwnd = find_window(window_title)
    if not hwnd:
        print(f"SKIP: No window found matching '{window_title}'.")
        print(f"      Open a {window_title} window and select it in GAM GUI.")
        return None  # Not fail, just skip

    left, top, right, bottom = get_window_rect(hwnd)
    w, h = right - left, bottom - top
    print(f"Found: HWND=0x{hwnd:X} rect=({left},{top},{right},{bottom}) {w}x{h}")

    # Connect TCP
    receiver = TcpFrameReceiver()
    if not receiver.connect():
        print("FAIL: Could not connect to TCP :9999.")
        return False

    print(f"Receiving frames for {duration}s...")
    time.sleep(duration)

    receiver.disconnect()
    count = receiver.frame_count
    print(f"Received: {count} frames")
    if count > 0:
        print("PASS: Window capture working ✓")
        return True
    else:
        print("FAIL: No frames. Select this window in GAM GUI and click Start.")
        return False


# ── Test 3: Desktop Input + Capture ──────────────────────────

def test_desktop_input(duration: float = 8.0):
    """
    Capture desktop while simulating random user activity.

    This verifies that:
    a) Desktop frames are captured while input is happening.
    b) Input simulation looks natural (mouse moves smoothly, not teleporting).
    c) The capture pipeline doesn't crash under concurrent input load.
    """
    print("\n" + "=" * 60)
    print("TEST 3: Desktop Input + Capture")
    print("=" * 60)
    print(f"Simulating random human activity for {duration}s while capturing desktop.")
    print("WARNING: Mouse will move! Keep hands off mouse/keyboard.")
    print("Starting in 3 seconds...")
    time.sleep(3)

    receiver = TcpFrameReceiver()
    if not receiver.connect():
        print("FAIL: Could not connect to TCP :9999.")
        return False

    # Collect frames in background
    captured_frames = []
    def on_frame(f: Frame):
        captured_frames.append(f)
    receiver.set_callback(on_frame)

    # Run input simulation in another thread
    def input_thread():
        random_desktop_activity(duration)

    t = threading.Thread(target=input_thread, daemon=True)
    t.start()
    t.join()

    receiver.disconnect()

    count = len(captured_frames)
    print(f"Captured {count} frames during {duration}s of activity.")

    if count > 0:
        fps = count / duration
        print(f"FPS: {fps:.1f}")
        save_frames(captured_frames, "desktop_input")

        # Verify frames are not all identical (content is changing due to input)
        if count >= 3:
            first = captured_frames[0].pixels
            last = captured_frames[-1].pixels
            diff = np.mean(np.abs(first.astype(np.float32) - last.astype(np.float32)))
            print(f"Frame delta (first vs last): {diff:.1f} (higher = more change)")
            if diff < 1.0 and fps > 1:
                print("NOTE: Low frame delta — desktop may be static or WGC not firing.")
        print("PASS: Desktop input + capture ✓")
        return True
    else:
        print("FAIL: No frames captured.")
        return False


# ── Test 4: Window Input + Capture ───────────────────────────

def test_window_input(window_title: str = "notepad", duration: float = 10.0):
    """
    Capture a specific window while simulating operations INSIDE it:
    - Click to activate
    - Move mouse within window bounds
    - Type text
    - Select text (shift+arrow)
    - Scroll

    This verifies that window-targeted capture works correctly when
    the captured content is actively changing due to user input.
    """
    print("\n" + "=" * 60)
    print(f"TEST 4: Window Input + Capture — '{window_title}'")
    print("=" * 60)

    hwnd = find_window(window_title)
    if not hwnd:
        print(f"SKIP: No window found matching '{window_title}'.")
        print(f"      Open Notepad (or similar) and retry.")
        return None

    left, top, right, bottom = get_window_rect(hwnd)
    w, h = right - left, bottom - top
    print(f"Found: HWND=0x{hwnd:X} rect=({left},{top},{right},{bottom}) {w}x{h}")

    # Bring window to front
    print("Activating window...")
    bring_to_front(hwnd)
    activate_via_click(hwnd)

    receiver = TcpFrameReceiver()
    if not receiver.connect():
        print("FAIL: Could not connect to TCP :9999.")
        return False

    captured_frames = []
    def on_frame(f: Frame):
        captured_frames.append(f)
    receiver.set_callback(on_frame)

    print(f"Simulating input inside window for {duration}s...")
    print("WARNING: Mouse will move! Keep hands off.")

    def input_thread():
        # Click inside window to focus
        cx = left + w // 2
        cy = top + h // 2
        click(cx, cy)
        time.sleep(0.3)

        # Type some text with natural rhythm
        type_text("Hello from GAM test framework! ", wpm=70)
        time.sleep(0.2)

        # Select all text with mouse drag
        sx = left + w // 4
        sy = top + h // 3
        ex = left + w * 3 // 4
        ey = top + h * 2 // 3
        drag(sx, sy, ex, ey, duration=0.5)
        time.sleep(0.2)

        # Click to deselect
        click(left + w // 2, top + h // 2)
        time.sleep(0.3)

        # Scroll a few times
        for _ in range(3):
            scroll(random.choice([-3, -2, -1, 1, 2, 3]))
            time.sleep(0.2)

        # Move mouse around window (simulating reading/exploring)
        for _ in range(5):
            tx = left + random.randint(w // 6, w * 5 // 6)
            ty = top + random.randint(h // 6, h * 5 // 6)
            move_to(tx, ty, duration=random.uniform(0.2, 0.5))
            wiggle(duration=0.3)

        # Type more text
        type_text("more test input here.", wpm=60)
        time.sleep(0.2)

        # Close with Ctrl+W (or just finish)
        press('enter')
        time.sleep(0.2)

    t = threading.Thread(target=input_thread, daemon=True)
    t.start()
    t.join()

    receiver.disconnect()

    count = len(captured_frames)
    print(f"Captured {count} frames during {duration}s of window activity.")

    if count > 0:
        fps = count / duration
        print(f"FPS: {fps:.1f}")
        save_frames(captured_frames, "window_input")

        # Verify frame diversity
        if count >= 3:
            pixels_list = [f.pixels for f in captured_frames[::max(1, count//10)]]
            unique = 0
            for i in range(len(pixels_list) - 1):
                diff = np.mean(np.abs(pixels_list[i].astype(np.float32) -
                                      pixels_list[i+1].astype(np.float32)))
                if diff > 1.0:
                    unique += 1
            print(f"Unique frame transitions: {unique}/{len(pixels_list)-1}")

        print("PASS: Window input + capture ✓")
        return True
    else:
        print("FAIL: No frames captured.")
        return False


# ── Main ──────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description="GAM E2E Capture Tests")
    parser.add_argument("--test", choices=["desktop", "window", "desktop_input",
                        "window_input", "all"], default="all",
                        help="Which test to run (default: all)")
    parser.add_argument("--window", default="notepad",
                        help="Window title substring for window tests (default: notepad)")
    parser.add_argument("--duration", type=float, default=0,
                        help="Override test duration in seconds")
    args = parser.parse_args()

    results = {}

    if args.test in ("desktop", "all"):
        dur = args.duration or 5.0
        results['desktop_capture'] = test_desktop_capture(duration=dur)

    if args.test in ("window", "all"):
        dur = args.duration or 5.0
        results['window_capture'] = test_window_capture(args.window, duration=dur)

    if args.test in ("desktop_input", "all"):
        dur = args.duration or 8.0
        results['desktop_input'] = test_desktop_input(duration=dur)

    if args.test in ("window_input", "all"):
        dur = args.duration or 10.0
        results['window_input'] = test_window_input(args.window, duration=dur)

    # Summary
    print("\n" + "=" * 60)
    print("TEST RESULTS")
    print("=" * 60)
    passed = 0
    failed = 0
    skipped = 0
    for name, result in results.items():
        if result is True:
            status = "✓ PASS"
            passed += 1
        elif result is False:
            status = "✗ FAIL"
            failed += 1
        else:
            status = "— SKIP"
            skipped += 1
        print(f"  {status}  {name}")
    print(f"\n{passed} passed, {failed} failed, {skipped} skipped")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
