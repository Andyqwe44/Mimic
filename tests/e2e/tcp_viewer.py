#!/usr/bin/env python3
"""
tcp_viewer.py — Connect to monitor_app TCP :9999, receive wire protocol frames,
render in real-time with OpenCV. Independent of WebView2 GUI.

Usage:
  python tcp_viewer.py                # default: 127.0.0.1:9999
  python tcp_viewer.py --fps          # show FPS counter overlay
  python tcp_viewer.py --save-dir frames/  # save PNGs while viewing

Key bindings:
  q / ESC — quit
  s      — save current frame as PNG
  f      — toggle FPS overlay

Requires: pip install numpy opencv-python
"""
import sys
import time
import argparse
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import numpy as np
import cv2

from tests.e2e.tcp_receiver import TcpFrameReceiver, Frame

# ── Globals ──────────────────────────────────────────────────

class ViewerState:
    def __init__(self):
        self.latest: np.ndarray | None = None  # RGBA uint8 (h,w,4)
        self.fps_history: list[float] = []      # last N frame intervals
        self.show_fps = True
        self.save_dir: Path | None = None
        self.save_count = 0
        self.running = True
        self.frame_count = 0
        self.start_time = time.time()


def on_frame(frame: Frame, state: ViewerState):
    """Called from receiver thread when a frame arrives."""
    rgba = frame.to_rgba()
    # OpenCV expects BGR
    bgr = cv2.cvtColor(rgba, cv2.COLOR_RGBA2BGR)
    state.latest = bgr
    state.frame_count += 1
    state.fps_history.append(time.time())
    # Keep only last 60 samples for FPS calculation
    if len(state.fps_history) > 60:
        state.fps_history = state.fps_history[-60:]


def render_loop(state: ViewerState):
    """Main thread: display latest frame with OpenCV."""
    cv2.namedWindow("GAM TCP Viewer", cv2.WINDOW_NORMAL)
    cv2.resizeWindow("GAM TCP Viewer", 960, 540)

    last_print = time.time()

    while state.running:
        frame = state.latest
        if frame is not None:
            display = frame.copy()

            # FPS overlay
            if state.show_fps and len(state.fps_history) >= 2:
                intervals = np.diff(state.fps_history[-60:])
                if len(intervals) > 0:
                    avg_interval = np.mean(intervals)
                    fps = 1.0 / avg_interval if avg_interval > 0 else 0
                    text = f"FPS: {fps:.1f}  |  Frames: {state.frame_count}"
                    cv2.putText(display, text, (10, 30),
                                cv2.FONT_HERSHEY_SIMPLEX, 0.7, (0, 255, 0), 2)
                    cv2.putText(display, f"Size: {frame.shape[1]}x{frame.shape[0]}",
                                (10, 58), cv2.FONT_HERSHEY_SIMPLEX, 0.5, (0, 255, 0), 1)

            cv2.imshow("GAM TCP Viewer", display)

        # Log stats every 2 seconds
        now = time.time()
        if now - last_print >= 2.0:
            elapsed = now - state.start_time
            avg_fps = state.frame_count / elapsed if elapsed > 0 else 0
            print(f"\r[viewer] {state.frame_count} frames | {avg_fps:.1f} avg FPS | "
                  f"{state.save_count} saved  ", end="", flush=True)
            last_print = now

        key = cv2.waitKey(1) & 0xFF
        if key == ord('q') or key == 27:  # q or ESC
            state.running = False
        elif key == ord('s'):
            # Save current frame
            if frame is not None:
                path = Path(f"tcp_frame_{state.save_count:04d}.png")
                cv2.imwrite(str(path), frame)
                state.save_count += 1
                print(f"\n[save] {path}")
        elif key == ord('f'):
            state.show_fps = not state.show_fps


def main():
    parser = argparse.ArgumentParser(description="GAM TCP Frame Viewer")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=9999)
    parser.add_argument("--fps", action="store_true", default=True,
                        help="Show FPS overlay (default: on)")
    parser.add_argument("--no-fps", action="store_false", dest="fps",
                        help="Hide FPS overlay")
    parser.add_argument("--save-dir", type=str, default=None,
                        help="Save received frames as PNG in this directory")
    args = parser.parse_args()

    print(f"GAM TCP Viewer — connecting to {args.host}:{args.port}...")
    print("Press 'q' or ESC to quit, 's' to save frame, 'f' to toggle FPS")

    state = ViewerState()
    state.show_fps = args.fps
    if args.save_dir:
        state.save_dir = Path(args.save_dir)
        state.save_dir.mkdir(parents=True, exist_ok=True)

    receiver = TcpFrameReceiver(args.host, args.port)
    receiver.set_callback(lambda f: on_frame(f, state))

    if not receiver.connect():
        print("ERROR: Cannot connect to TCP server.")
        print("Make sure monitor_app is running and streaming:")
        print("  cd monitor_app && build\\monitor_app.exe --auto-stream")
        sys.exit(1)

    print(f"Connected! Receiving frames... ({receiver.host}:{receiver.port})")

    try:
        render_loop(state)
    except KeyboardInterrupt:
        pass
    finally:
        state.running = False
        receiver.disconnect()
        cv2.destroyAllWindows()
        elapsed = time.time() - state.start_time
        print(f"\nDone. {state.frame_count} frames in {elapsed:.1f}s "
              f"= {state.frame_count/elapsed:.1f} avg FPS")


if __name__ == "__main__":
    main()
