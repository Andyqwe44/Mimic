"""
payload/bgra.py — BGRA pixel frame payload (PayloadType.BGRA_FRAME).

Depends on: protocol.protocol (for PayloadType).
Does NOT depend on transport.

Payload body: [w:4 LE][h:4 LE][ch:4 LE][reserved:4][pixels...]
"""
import struct
from dataclasses import dataclass

HEADER_SIZE: int = 16
HEADER_STRUCT = struct.Struct("<IIII")  # w, h, ch, reserved


@dataclass
class BgraFrame:
    width: int
    height: int
    channels: int
    pixels: bytes

    def to_numpy(self):
        import numpy as np
        return np.frombuffer(self.pixels, dtype=np.uint8).reshape(
            (self.height, self.width, self.channels))


def pack(w: int, h: int, ch: int, pixels: bytes) -> bytes:
    """Pack BGRA pixels → payload bytes."""
    return HEADER_STRUCT.pack(w, h, ch, 0) + pixels


MAX_DIM: int = 16384      # sanity cap per dimension
MAX_CHANNELS: int = 16
MAX_PIXELS: int = 1024 * 1024 * 1024  # 1 GiB sanity cap

def unpack(payload: bytes) -> BgraFrame | None:
    """Unpack payload bytes → BgraFrame. Returns None on invalid input."""
    if len(payload) < HEADER_SIZE:
        return None
    w, h, ch, _ = HEADER_STRUCT.unpack(payload[:HEADER_SIZE])
    # Validate: non-zero, reasonable dimensions
    if w == 0 or h == 0 or ch == 0:
        return None
    if w > MAX_DIM or h > MAX_DIM or ch > MAX_CHANNELS:
        return None
    expected = w * h * ch
    if expected > MAX_PIXELS:
        return None
    pixels = payload[HEADER_SIZE:HEADER_SIZE + expected]
    if len(pixels) != expected:
        return None
    return BgraFrame(width=w, height=h, channels=ch, pixels=pixels)
