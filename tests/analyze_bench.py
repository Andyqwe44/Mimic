"""Analyze WGC capture benchmark output file.

Usage: python analyze_bench.py <frames.bin>
"""
import sys
import struct
from pathlib import Path


def analyze(path: str) -> None:
    data = Path(path).read_bytes()
    offset = 0

    # Header: [frame_count:4][cap_w:4][cap_h:4]
    frame_count = struct.unpack_from("<I", data, offset)[0]; offset += 4
    cap_w = struct.unpack_from("<I", data, offset)[0]; offset += 4
    cap_h = struct.unpack_from("<I", data, offset)[0]; offset += 4

    print(f"Capture: {cap_w}x{cap_h}")
    print(f"Frames in file: {frame_count if frame_count != 0xFFFFFFFF else 'unknown (live dump)'}")
    print()

    timestamps = []
    sizes = []
    total_bytes = 0
    frame_w = 0
    frame_h = 0

    while offset + 20 <= len(data):
        ts = struct.unpack_from("<Q", data, offset)[0]; offset += 8
        fw = struct.unpack_from("<I", data, offset)[0]; offset += 4
        fh = struct.unpack_from("<I", data, offset)[0]; offset += 4
        size = struct.unpack_from("<I", data, offset)[0]; offset += 4
        offset += size  # skip pixel data

        timestamps.append(ts)
        sizes.append(size)
        total_bytes += size
        frame_w = fw
        frame_h = fh

    total_frames = len(timestamps)
    if total_frames == 0:
        print("ERROR: No frames found")
        return

    print(f"Frame size: {frame_w}x{frame_h} = {sizes[0]/1024:.0f} KB each")
    print(f"Total frames parsed: {total_frames}")
    print(f"Total data: {total_bytes / 1024 / 1024:.1f} MB")
    print()

    # FPS calculation from timestamps
    if len(timestamps) >= 2:
        start_us = timestamps[0]
        end_us = timestamps[-1]
        duration_s = (end_us - start_us) / 1_000_000
        avg_fps = total_frames / duration_s if duration_s > 0 else 0
        print(f"Duration: {duration_s:.2f}s")
        print(f"Average FPS: {avg_fps:.1f}")

        # Per-second FPS breakdown
        print("\nPer-second FPS:")
        sec_start_us = timestamps[0]
        sec_frames = 0
        for ts in timestamps:
            if ts - sec_start_us >= 1_000_000:
                fps = sec_frames / ((ts - sec_start_us) / 1_000_000)
                print(f"  {fps:.1f} FPS ({sec_frames} frames)")
                sec_start_us = ts
                sec_frames = 0
            sec_frames += 1
        if sec_frames > 0:
            remaining = (timestamps[-1] - sec_start_us) / 1_000_000
            if remaining > 0:
                print(f"  {sec_frames/remaining:.1f} FPS ({sec_frames} frames, partial)")

        # Frame interval stats
        intervals = [timestamps[i] - timestamps[i-1] for i in range(1, len(timestamps))]
        intervals_ms = [i / 1000 for i in intervals]
        intervals_ms.sort()
        p50 = intervals_ms[len(intervals_ms)//2]
        p95 = intervals_ms[int(len(intervals_ms)*0.95)]
        p99 = intervals_ms[int(len(intervals_ms)*0.99)]
        print(f"\nFrame interval (ms):")
        print(f"  min={min(intervals_ms):.1f}  p50={p50:.1f}  p95={p95:.1f}  p99={p99:.1f}  max={max(intervals_ms):.1f}")
        print(f"  target 60fps=16.7ms  30fps=33.3ms  15fps=66.7ms")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python analyze_bench.py <frames.bin>")
        sys.exit(1)
    analyze(sys.argv[1])
