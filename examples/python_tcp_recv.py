"""
示例: Python 连接 TCP :9999 → 接收帧 → 保存为 PNG

运行:
    # 终端1: ./cpp_tcp_send.exe
    # 终端2: python examples/python_tcp_recv.py

依赖: 无 (纯标准库). 可选: numpy + PIL 保存图片.
"""
import sys
import os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "model"))

try:
    from stream_protocol import StreamClient, Frame, FRAME_MAGIC
    USE_MODULE = True
except ImportError:
    USE_MODULE = False
    # 回退: 直接内联协议解析 (不依赖项目)
    import socket, struct
    HEADER = struct.Struct("<IIIII")

try:
    import numpy as np
    HAS_NUMPY = True
except ImportError:
    HAS_NUMPY = False

try:
    from PIL import Image
    HAS_PIL = True
except ImportError:
    HAS_PIL = False


def main():
    host = "127.0.0.1"
    port = 9999

    print(f"Connecting to {host}:{port}...")

    if USE_MODULE:
        # ── 使用封装好的 StreamClient ──
        client = StreamClient(host, port)
        client.connect()

        frame_count = 0
        for frame in client.frames():
            frame_count += 1
            print(f"[python] frame {frame_count}: {frame.width}x{frame.height} "
                  f"ch={frame.channels} {len(frame.pixels)//1024}KB")

            if frame_count % 25 == 0 and HAS_NUMPY and HAS_PIL:
                img = frame.to_numpy()
                # BGRA → RGBA
                rgba = img[:, :, [2, 1, 0, 3]]
                Image.fromarray(rgba).save(f"frame_{frame_count:04d}.png")
                print(f"  -> saved frame_{frame_count:04d}.png")

    else:
        # ── 手写接收 (无依赖) ──
        sock = socket.create_connection((host, port))
        frame_count = 0
        while True:
            try:
                hdr = sock.recv(20)
                if not hdr: break
                magic, w, h, ch, size = HEADER.unpack(hdr)
                if magic != 0x4D415246:
                    print(f"Bad magic: 0x{magic:08X}")
                    break
                pixels = b""
                while len(pixels) < size:
                    chunk = sock.recv(size - len(pixels))
                    if not chunk: break
                    pixels += chunk
                frame_count += 1
                print(f"[python] frame {frame_count}: {w}x{h} ch={ch} {len(pixels)//1024}KB")
            except (ConnectionError, OSError):
                break

    print(f"Done: {frame_count} frames received")


if __name__ == "__main__":
    main()
