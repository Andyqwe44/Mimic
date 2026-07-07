#!/usr/bin/env python3
"""
run_all.py — E2E test orchestrator for GAM capture + input pipeline.

Runs capture tests + input simulation tests in sequence, saves frame PNGs,
and prints a summary report.

Quick start:
  # Run all tests (needs monitor_app.exe streaming in background)
  python -m tests.e2e.run_all

  # Run specific tests
  python -m tests.e2e.run_all --test desktop_input
  python -m tests.e2e.run_all --test window --window chrome

Requirements:
  pip install numpy pillow
"""
import sys
import subprocess
from pathlib import Path

# Ensure project root is in path
ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT))


def check_deps():
    """Verify Python dependencies are installed."""
    missing = []
    for mod in ["numpy", "PIL"]:
        try:
            __import__(mod)
        except ImportError:
            missing.append(mod)
    if missing:
        print(f"Missing dependencies: {', '.join(missing)}")
        print("Install: pip install numpy pillow")
        sys.exit(1)


def check_monitor_app():
    """Check if monitor_app is running (TCP :9999 listening)."""
    import socket
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(1.0)
    try:
        s.connect(("127.0.0.1", 9999))
        s.close()
        return True
    except (ConnectionRefusedError, socket.timeout, OSError):
        return False


def main():
    import argparse
    parser = argparse.ArgumentParser(
        description="GAM E2E Test Suite",
        epilog="Prerequisite: monitor_app.exe running with stream active (click Start in GUI)."
    )
    parser.add_argument("--test", default="all",
                        choices=["desktop", "window", "desktop_input", "window_input", "all"])
    parser.add_argument("--window", default="notepad",
                        help="Window title substring for window tests")
    parser.add_argument("--duration", type=float, default=0,
                        help="Override test duration (seconds)")
    parser.add_argument("--check-only", action="store_true",
                        help="Only check prerequisites, don't run tests")
    args = parser.parse_args()

    print("=" * 60)
    print("GAM E2E Test Suite")
    print("=" * 60)

    # Check dependencies
    check_deps()
    print("✓ numpy, PIL available")

    # Check monitor_app
    if check_monitor_app():
        print("✓ monitor_app TCP :9999 is listening")
    else:
        print("✗ monitor_app TCP :9999 NOT reachable!")
        print()
        print("  Please start the GAM GUI and click Start to begin streaming:")
        print(f"    cd {ROOT / 'monitor_app'}")
        print(f"    build\\monitor_app.exe --dev")
        print()
        print("  Then select a target (desktop or window) in the Connection panel")
        print("  and click Start in the Screenshot panel.")
        print()
        if not args.check_only:
            print("  Aborting tests.")
            sys.exit(1)

    if args.check_only:
        print("\nAll prerequisites OK. Ready to test.")
        return

    # Run tests
    from tests.e2e.capture_test import (
        test_desktop_capture,
        test_window_capture,
        test_desktop_input,
        test_window_input,
    )

    results = {}
    test_name = args.test
    window_title = args.window

    if test_name in ("desktop", "all"):
        dur = args.duration or 5.0
        results['desktop_capture'] = test_desktop_capture(duration=dur)

    if test_name in ("window", "all"):
        dur = args.duration or 5.0
        results['window_capture'] = test_window_capture(window_title, duration=dur)

    if test_name in ("desktop_input", "all"):
        dur = args.duration or 8.0
        results['desktop_input'] = test_desktop_input(duration=dur)

    if test_name in ("window_input", "all"):
        dur = args.duration or 10.0
        results['window_input'] = test_window_input(window_title, duration=dur)

    # Summary
    print("\n" + "=" * 60)
    print("TEST RESULTS")
    print("=" * 60)
    passed = failed = skipped = 0
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

    if failed:
        print("\nSome tests FAILED. Check that:")
        print("  1. monitor_app.exe is running and streaming")
        print("  2. Target window is visible and not minimized")
        print("  3. Capture method is WGC (for dynamic content)")

    sys.exit(0 if failed == 0 else 1)


if __name__ == "__main__":
    main()
