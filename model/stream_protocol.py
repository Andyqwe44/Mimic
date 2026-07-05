"""
Stream Protocol — shared constants across C++, Rust, Python.

Frame format (binary, little-endian):
  [magic:4 "FRAM"][w:4][h:4][ch:4][size:4][pixels: size bytes]

Usage:
  from model.stream_protocol import StreamClient
  client = StreamClient()
  for w, h, ch, pixels in client.frames():
      img = np.frombuffer(pixels, dtype=np.uint8).reshape((h, w, ch))
      # ... feed to AI model

Keep in sync with:
  common/include/stream_protocol.hpp
  monitor_web/src-tauri/src/stream_protocol.rs
"""
import struct
import socket
from dataclasses import dataclass
from typing import Iterator, Tuple

# ── Network ─────────────────────────────────────────────
DEFAULT_TCP_PORT: int = 9999
DEFAULT_HOST: str = "127.0.0.1"

# ── Pipe ────────────────────────────────────────────────
DEFAULT_PIPE_NAME: str = "tictactoe_stream"  # -> \\.\\pipe\\tictactoe_stream

# ── Frame format ────────────────────────────────────────
FRAME_MAGIC: int = 0x4D415246  # "FRAM" LE
FRAME_HEADER_SIZE: int = 20    # magic(4) + w(4) + h(4) + ch(4) + size(4)
FRAME_CH_BGRA: int = 4
MAX_FRAME_DIM: int = 640

# struct format: < = little-endian, I = uint32
HEADER_STRUCT = struct.Struct("<IIIII")  # magic, w, h, ch, size

# ── Capabilities (bitmask) ──────────────────────────────
CAP_BGRA_RAW: int = 1 << 0
CAP_H264_STREAM: int = 1 << 1
CAP_DESKTOP: int = 1 << 2
CAP_WINDOW: int = 1 << 3


@dataclass
class Frame:
    """One captured frame."""
    width: int
    height: int
    channels: int
    pixels: bytes  # raw BGRA, width * height * channels bytes

    def to_numpy(self):
        """Convert to numpy array (h, w, ch) BGRA."""
        import numpy as np
        return np.frombuffer(self.pixels, dtype=np.uint8).reshape(
            (self.height, self.width, self.channels))


class StreamClient:
    """Connect to capture stream via TCP, iterate frames."""

    def __init__(self, host: str = DEFAULT_HOST, port: int = DEFAULT_TCP_PORT):
        self.host = host
        self.port = port
        self._sock: socket.socket | None = None

    def connect(self) -> None:
        self._sock = socket.create_connection((self.host, self.port), timeout=5.0)

    def close(self) -> None:
        if self._sock:
            self._sock.close()
            self._sock = None

    def __enter__(self):
        self.connect()
        return self

    def __exit__(self, *args):
        self.close()

    def read_frame(self) -> Frame | None:
        """Read one frame. Returns None on EOF / connection closed."""
        if not self._sock:
            raise RuntimeError("Not connected")
        try:
            hdr = self._recv_exact(FRAME_HEADER_SIZE)
            if not hdr:
                return None
            magic, w, h, ch, size = HEADER_STRUCT.unpack(hdr)
            if magic != FRAME_MAGIC:
                raise ValueError(f"Bad magic: 0x{magic:08X}, expected 0x{FRAME_MAGIC:08X}")
            pixels = self._recv_exact(size)
            if not pixels:
                return None
            return Frame(width=w, height=h, channels=ch, pixels=pixels)
        except (ConnectionError, OSError):
            return None

    def frames(self) -> Iterator[Frame]:
        """Iterate frames until connection closes."""
        while True:
            frame = self.read_frame()
            if frame is None:
                break
            yield frame

    def _recv_exact(self, n: int) -> bytes | None:
        buf = b""
        while len(buf) < n:
            try:
                chunk = self._sock.recv(n - len(buf))
                if not chunk:
                    return None
                buf += chunk
            except (ConnectionError, OSError):
                return None
        return buf


# ── Convenience ──────────────────────────────────────────

def connect_and_stream(host: str = DEFAULT_HOST, port: int = DEFAULT_TCP_PORT):
    """Context manager for streaming frames. Use in 'with' statement."""
    return StreamClient(host, port)
