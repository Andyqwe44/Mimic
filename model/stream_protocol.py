"""
Stream Protocol — pure transport layer. Payload-agnostic.

Frame format (binary, little-endian):
  [magic:4 "FRAM"][size:4 LE][payload: size bytes]

Payload is opaque bytes. Application layer defines its own format.

Keep in sync with:
  common/include/stream_protocol.hpp (C++)
  monitor_web/src-tauri/src/main.rs (Rust — uses protocol/ wire format)

For BGRA frame payload, use: model.payload.bgra
"""
import struct
import socket
from typing import Iterator, Optional

# ── Transport constants ──────────────────────────────────
DEFAULT_TCP_PORT: int = 9999
DEFAULT_HOST: str = "127.0.0.1"
DEFAULT_PIPE_NAME: str = "tictactoe_stream"

FRAME_MAGIC: int = 0x4D415246  # "FRAM" LE
FRAME_HEADER_SIZE: int = 8     # magic(4) + size(4)
MAX_PAYLOAD_SIZE: int = 256 * 1024 * 1024  # 256 MiB, reject larger as corrupt

TRANSPORT_HEADER = struct.Struct("<II")  # magic, size


# ═══ Transport layer ═══════════════════════════════════════

def build_frame_header(payload_size: int) -> bytes:
    return TRANSPORT_HEADER.pack(FRAME_MAGIC, payload_size)

def parse_frame_header(data: bytes) -> Optional[int]:
    """Returns payload_size or None if bad magic."""
    magic, size = TRANSPORT_HEADER.unpack(data)
    if magic != FRAME_MAGIC:
        return None
    return size

def send_frame(sock: socket.socket, payload: bytes) -> None:
    sock.sendall(build_frame_header(len(payload)))
    sock.sendall(payload)

def recv_frame(sock: socket.socket) -> Optional[bytes]:
    """Receive one frame. Returns None on EOF or unchanged signal."""
    hdr = _recv_exact(sock, FRAME_HEADER_SIZE)
    if not hdr:
        return None
    size = parse_frame_header(hdr)
    if size is None:
        # All-zero header = unchanged signal from sender
        if hdr == b"\x00" * FRAME_HEADER_SIZE:
            return None
        raise ValueError(f"Bad magic in frame header")
    if size > MAX_PAYLOAD_SIZE:
        raise ValueError(f"Payload size {size} exceeds max {MAX_PAYLOAD_SIZE}")
    return _recv_exact(sock, size)

def _recv_exact(sock: socket.socket, n: int) -> Optional[bytes]:
    buf = b""
    while len(buf) < n:
        try:
            chunk = sock.recv(n - len(buf))
            if not chunk:
                return None
            buf += chunk
        except (ConnectionError, OSError):
            return None
    return buf


# ═══ Convenience client ────────────────────────────────────

class StreamClient:
    """Connect to TCP stream, iterate application-level BgraFrames."""

    def __init__(self, host: str = DEFAULT_HOST, port: int = DEFAULT_TCP_PORT):
        self.host = host; self.port = port; self._sock: socket.socket | None = None

    def connect(self) -> None:
        self._sock = socket.create_connection((self.host, self.port), timeout=5.0)

    def close(self) -> None:
        if self._sock: self._sock.close(); self._sock = None

    def __enter__(self): self.connect(); return self
    def __exit__(self, *args): self.close()

    def read_frame(self):
        """Returns BgraFrame or None on EOF/unchanged signal."""
        from .payload.bgra import unpack as bgra_unpack
        payload = recv_frame(self._sock)
        if payload is None:
            return None
        return bgra_unpack(payload)

    def frames(self):
        """Iterator yielding BgraFrame objects."""
        while True:
            frame = self.read_frame()
            if frame is None: break
            yield frame
