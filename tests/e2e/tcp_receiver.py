"""
tcp_receiver.py — Connect to monitor_app TCP :9999, receive wire protocol frames.

Wire protocol (from protocol/protocol.h):
  Frame: [magic:4 "FRAM"][body_size:4 LE][type_tag:4 LE][body: body_size bytes]
  type_tag 1 (BGRA): [w:4][h:4][ch:4][reserved:4][pixels: w*h*ch]
"""
import socket
import struct
import threading
import time
import queue
from dataclasses import dataclass
from typing import Optional, Callable
import numpy as np

MAGIC = 0x4D415246  # "FRAM"
FRAME_HEADER_SIZE = 12
DEFAULT_PORT = 9999
DEFAULT_HOST = "127.0.0.1"


@dataclass
class Frame:
    """Decoded wire protocol frame."""
    type_tag: int
    width: int
    height: int
    channels: int
    pixels: np.ndarray  # shape (h, w, ch), BGRA
    timestamp: float    # time.time() when received

    def to_rgba(self) -> np.ndarray:
        """Convert BGRA → RGBA."""
        if self.channels == 4:
            rgba = self.pixels.copy()
            rgba[:, :, 0] = self.pixels[:, :, 2]  # B→R
            rgba[:, :, 2] = self.pixels[:, :, 0]  # R→B
            return rgba
        return self.pixels


class TcpFrameReceiver:
    """Connect to monitor_app TCP server, receive and parse wire protocol frames.

    Runs a background thread that reads frames into a thread-safe queue.
    """

    def __init__(self, host: str = DEFAULT_HOST, port: int = DEFAULT_PORT,
                 max_queue: int = 300):
        self.host = host
        self.port = port
        self._sock: Optional[socket.socket] = None
        self._thread: Optional[threading.Thread] = None
        self._running = False
        self._frame_queue: queue.Queue = queue.Queue(maxsize=max_queue)
        self._frame_count = 0
        self._byte_count = 0
        self._error_count = 0
        self._lock = threading.Lock()
        self._on_frame: Optional[Callable[[Frame], None]] = None

    # ── Public API ──────────────────────────────────────────

    @property
    def frame_count(self) -> int:
        with self._lock:
            return self._frame_count

    @property
    def byte_count(self) -> int:
        with self._lock:
            return self._byte_count

    @property
    def error_count(self) -> int:
        with self._lock:
            return self._error_count

    def set_callback(self, cb: Callable[[Frame], None]):
        """Set a callback invoked on every received frame (in receiver thread)."""
        self._on_frame = cb

    def connect(self) -> bool:
        """Connect to the TCP server. Returns True on success."""
        try:
            self._sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self._sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            self._sock.settimeout(5.0)
            self._sock.connect((self.host, self.port))
            self._sock.settimeout(1.0)  # read timeout for graceful stop
            self._running = True
            self._thread = threading.Thread(target=self._recv_loop, daemon=True)
            self._thread.start()
            return True
        except (ConnectionRefusedError, socket.timeout, OSError) as e:
            print(f"[TCP] connect failed: {e}")
            return False

    def disconnect(self):
        """Stop receiving and close connection."""
        self._running = False
        if self._thread and self._thread.is_alive():
            self._thread.join(timeout=3.0)
        if self._sock:
            try:
                self._sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            self._sock.close()
            self._sock = None

    def get_frame(self, timeout: float = 5.0) -> Optional[Frame]:
        """Get next frame from queue (blocking with timeout)."""
        try:
            return self._frame_queue.get(timeout=timeout)
        except queue.Empty:
            return None

    def get_frames(self, count: int, timeout: float = 10.0) -> list[Frame]:
        """Collect `count` frames. Returns whatever was received within timeout."""
        frames = []
        deadline = time.time() + timeout
        while len(frames) < count and time.time() < deadline:
            f = self.get_frame(timeout=min(1.0, deadline - time.time()))
            if f is not None:
                frames.append(f)
        return frames

    def drain(self):
        """Empty the frame queue."""
        while not self._frame_queue.empty():
            try:
                self._frame_queue.get_nowait()
            except queue.Empty:
                break

    # ── Internal ────────────────────────────────────────────

    def _recv_loop(self):
        """Background thread: read frames from TCP socket."""
        buf = bytearray()
        while self._running:
            try:
                data = self._sock.recv(65536)
                if not data:
                    print("[TCP] server closed connection")
                    break
                buf.extend(data)
                buf = self._parse_frames(buf)
            except socket.timeout:
                continue
            except OSError as e:
                if self._running:
                    print(f"[TCP] recv error: {e}")
                break

    def _parse_frames(self, buf: bytearray) -> bytearray:
        """Parse complete frames from buffer. Returns remaining bytes."""
        while len(buf) >= FRAME_HEADER_SIZE:
            magic = struct.unpack_from("<I", buf, 0)[0]
            if magic != MAGIC:
                # Skip byte until we find magic
                idx = buf.find(b"FRAM", 0, min(len(buf), 64))
                if idx >= 0:
                    del buf[:idx]
                    continue
                else:
                    del buf[:max(1, len(buf) - 3)]
                    break

            body_size = struct.unpack_from("<I", buf, 4)[0]
            type_tag = struct.unpack_from("<I", buf, 8)[0]
            total = FRAME_HEADER_SIZE + body_size

            if len(buf) < total:
                break  # incomplete frame, wait for more data

            body = buf[FRAME_HEADER_SIZE:total]
            del buf[:total]

            try:
                frame = self._decode_body(type_tag, body)
                if frame:
                    with self._lock:
                        self._frame_count += 1
                        self._byte_count += total
                    try:
                        self._frame_queue.put_nowait(frame)
                    except queue.Full:
                        # Drop oldest frame if queue full
                        try:
                            self._frame_queue.get_nowait()
                            self._frame_queue.put_nowait(frame)
                        except queue.Empty:
                            pass
                    if self._on_frame:
                        self._on_frame(frame)
            except Exception as e:
                with self._lock:
                    self._error_count += 1
                print(f"[TCP] decode error: {e}")

        return buf

    def _decode_body(self, type_tag: int, body: bytes) -> Optional[Frame]:
        """Decode frame body based on type_tag."""
        if type_tag == 1:  # BGRA
            if len(body) < 16:
                return None
            w, h, ch, _reserved = struct.unpack_from("<IIII", body, 0)
            expected = w * h * ch
            if len(body) - 16 < expected:
                print(f"[TCP] truncated BGRA frame: got {len(body)-16}, expected {expected}")
                return None
            pixels = np.frombuffer(body[16:16 + expected], dtype=np.uint8)
            pixels = pixels.reshape((h, w, ch))
            return Frame(
                type_tag=type_tag,
                width=w, height=h, channels=ch,
                pixels=pixels,
                timestamp=time.time(),
            )
        else:
            # Unknown type tag — skip
            return None
