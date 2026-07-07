"""
auto_stream_test.py — Automate GUI interaction + measure streaming FPS.

Uses Win32 SendInput to click the Start button in the GAM GUI, then
connects to TCP :9999 to measure real FPS of the streaming pipeline.

Requires: monitor_app.exe running (GUI visible on screen).
"""
import sys
import time
import threading
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent.parent.parent))

from tests.e2e.tcp_receiver import TcpFrameReceiver
from tests.e2e.input_sim import (
    move_to, click, screen_size, cursor_pos,
)
from tests.e2e.capture_test import find_window as _find_window

user32 = __import__('ctypes').windll.user32


def find_gam_window():
    """Find the Game Agent Monitor window HWND."""
    result = []
    def enum_cb(hwnd, _):
        if not user32.IsWindowVisible(hwnd):
            return True
        length = user32.GetWindowTextLengthW(hwnd)
        if length == 0:
            return True
        buf = __import__('ctypes').create_unicode_buffer(length + 1)
        user32.GetWindowTextW(hwnd, buf, length + 1)
        if 'game agent monitor' in buf.value.lower():
            result.append(hwnd)
            return False  # stop enum
        return True
    WNDENUMPROC = __import__('ctypes').WINFUNCTYPE(
        __import__('ctypes').c_bool, __import__('ctypes').c_void_p,
        __import__('ctypes').c_void_p)
    user32.EnumWindows(WNDENUMPROC(enum_cb), 0)
    return result[0] if result else None


def get_window_rect(hwnd):
    r = __import__('ctypes').wintypes.RECT()
    user32.GetWindowRect(hwnd, __import__('ctypes').byref(r))
    return r.left, r.top, r.right, r.bottom


def click_screenshot_play():
    """
    Find GAM window, expand Screenshot panel, then click its Play button.

    Layout (1024px wide window):
      - Right sidebar: ~700-1075 (width ~324)
      - Panel headers are ~44px tall with 12px gap
      - Connection panel: y=top+8
      - Screenshot panel: y=top+8+44+12 = top+64 (if connection collapsed)
      - Play button: right side of Screenshot header, ~x=right-70
    """
    hwnd = find_gam_window()
    if not hwnd:
        print("ERROR: Game Agent Monitor window not found!")
        return False

    left, top, right, bottom = get_window_rect(hwnd)
    w = right - left
    print(f"GAM window: ({left},{top})-({right},{bottom}) {w}x{bottom-top}")

    # Right sidebar starts at about left + (window_width - right_panel_width)
    # Right panel width: default 324, possible range 324-400
    rp_width = 324
    sidebar_x = right - rp_width

    # Step 1: Expand Screenshot panel if collapsed
    # Click Screenshot header (middle of header row)
    # If Connection is collapsed (44px), Screenshot header starts at top + 44 + 12(gap)
    ss_header_y = top + 64  # Approximate (connection collapsed + gap)
    ss_header_x = sidebar_x + rp_width // 2  # Middle of right sidebar
    print(f"Expanding Screenshot panel: click ({ss_header_x}, {ss_header_y})")
    click(ss_header_x, ss_header_y)
    time.sleep(0.5)

    # Step 2: Click Play button in Screenshot header
    # Play button is on the right side of the header, ~70px from right edge
    play_x = right - 70
    play_y = ss_header_y
    print(f"Clicking Play: ({play_x}, {play_y})")
    click(play_x, play_y)
    time.sleep(1.0)
    return True


def test_fps(duration: float = 10.0):
    """Connect to TCP :9999, receive frames for `duration` seconds, report FPS."""
    print(f"\nConnecting to TCP :9999, receiving for {duration}s...")
    receiver = TcpFrameReceiver()
    if not receiver.connect():
        print("FAIL: TCP connection failed. Is monitor_app streaming?")
        return None

    time.sleep(duration)
    receiver.disconnect()

    count = receiver.frame_count
    mb = receiver.byte_count / (1024 * 1024)
    errors = receiver.error_count

    print(f"\n--- Results ---")
    print(f"Frames: {count}")
    print(f"Data: {mb:.1f} MB")
    print(f"Errors: {errors}")
    if count > 0:
        fps = count / duration
        print(f"FPS: {fps:.1f}")
        avg_frame_size = receiver.byte_count / count if count > 0 else 0
        print(f"Avg frame size: {avg_frame_size / 1024:.1f} KB")
        return fps
    else:
        print("FAIL: No frames received. Did you click Start?")
        return 0


def main():
    import argparse
    parser = argparse.ArgumentParser(description="Auto stream test")
    parser.add_argument("--duration", type=float, default=10.0,
                        help="Streaming duration in seconds")
    parser.add_argument("--no-click", action="store_true",
                        help="Don't click Start (already streaming)")
    args = parser.parse_args()

    print("=" * 60)
    print("GAM Auto Stream Test")
    print("=" * 60)

    if not args.no_click:
        print("\nClicking Start button in GAM GUI...")
        print("Make sure GAM window is visible on screen!")
        print("Starting in 2 seconds...")
        time.sleep(2)
        if not click_screenshot_play():
            print("Could not find GAM window. Maybe it's minimized?")
            print("Try manually clicking Start, then run with --no-click")
            sys.exit(1)
        time.sleep(1.0)  # Wait for stream to initialize
    else:
        print("\nAssuming stream is already active...")

    fps = test_fps(duration=args.duration)

    if fps is not None and fps > 0:
        print(f"\nStream FPS: {fps:.1f}")
        if fps >= 30:
            print("PASS: 30+ FPS streaming ✓")
        elif fps >= 10:
            print("OK: 10-30 FPS — may improve with dynamic content")
        else:
            print("LOW FPS — static desktop produces few WGC frames")
    else:
        print("\nFAIL: Check that GAM GUI is visible and Start was clicked.")


if __name__ == "__main__":
    main()
