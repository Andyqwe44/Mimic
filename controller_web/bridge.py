#!/usr/bin/env python3
"""
ControllerBridge — WebSocket (:9997) ↔ agent TCP FRAM (:9999).

Browsers cannot open raw TCP; this tiny bridge:
  - receives H.264 (type=2) from the agent and forwards binary to Web clients
  - receives JSON actions from Web and wraps them as CONTROL_MSG to the agent

Usage (agent streaming first):
  python controller_web/bridge.py
  # then open controller_web/index.html (or any static server) → Connect ws://HOST:9997
"""
from __future__ import annotations

import argparse
import asyncio
import json
import struct
import sys
from pathlib import Path

MAGIC = 0x4D415246
TYPE_BGRA = 1
TYPE_H264 = 2
TYPE_CONTROL = 3
HEADER = struct.Struct("<III")  # magic, payload_size, type_tag

try:
    import websockets
    from websockets.server import serve
except ImportError:
    print("Install websockets: pip install websockets", file=sys.stderr)
    sys.exit(1)


def build_header(payload_size: int, type_tag: int) -> bytes:
    return HEADER.pack(MAGIC, payload_size, type_tag)


async def read_exact(reader: asyncio.StreamReader, n: int) -> bytes:
    buf = bytearray()
    while len(buf) < n:
        chunk = await reader.read(n - len(buf))
        if not chunk:
            raise ConnectionError("agent TCP closed")
        buf.extend(chunk)
    return bytes(buf)


class Bridge:
    def __init__(self, agent_host: str, agent_port: int):
        self.agent_host = agent_host
        self.agent_port = agent_port
        self.clients: set = set()
        self.writer: asyncio.StreamWriter | None = None
        self.reader: asyncio.StreamReader | None = None
        self._lock = asyncio.Lock()

    async def ensure_agent(self) -> None:
        if self.writer and not self.writer.is_closing():
            return
        self.reader, self.writer = await asyncio.open_connection(self.agent_host, self.agent_port)
        print(f"[bridge] connected to agent {self.agent_host}:{self.agent_port}")
        asyncio.create_task(self._agent_read_loop())

    async def _agent_read_loop(self) -> None:
        assert self.reader is not None
        try:
            while True:
                hdr = await read_exact(self.reader, 12)
                magic, size, tag = HEADER.unpack(hdr)
                if magic != MAGIC:
                    print("[bridge] bad magic from agent")
                    break
                body = await read_exact(self.reader, size) if size else b""
                if tag == TYPE_H264 and len(body) >= 16:
                    # Forward meta[16]+annexb as one binary WS message
                    await self._broadcast_bin(body)
                elif tag == TYPE_BGRA:
                    await self._broadcast_text(json.dumps({
                        "type": "status",
                        "msg": "agent sent raw BGRA (H.264 unavailable) — upgrade agent encoder",
                    }))
        except Exception as e:
            print(f"[bridge] agent read ended: {e}")
        finally:
            if self.writer:
                self.writer.close()
                try:
                    await self.writer.wait_closed()
                except Exception:
                    pass
            self.writer = None
            self.reader = None

    async def _broadcast_bin(self, data: bytes) -> None:
        dead = []
        for ws in list(self.clients):
            try:
                await ws.send(data)
            except Exception:
                dead.append(ws)
        for ws in dead:
            self.clients.discard(ws)

    async def _broadcast_text(self, text: str) -> None:
        dead = []
        for ws in list(self.clients):
            try:
                await ws.send(text)
            except Exception:
                dead.append(ws)
        for ws in dead:
            self.clients.discard(ws)

    async def send_control(self, json_obj: dict) -> None:
        await self.ensure_agent()
        assert self.writer is not None
        payload = json.dumps(json_obj, ensure_ascii=False).encode("utf-8")
        async with self._lock:
            self.writer.write(build_header(len(payload), TYPE_CONTROL))
            self.writer.write(payload)
            await self.writer.drain()

    async def ws_handler(self, ws):
        self.clients.add(ws)
        print(f"[bridge] web client +1 ({len(self.clients)})")
        try:
            await self.ensure_agent()
            await ws.send(json.dumps({"type": "status", "msg": "bridge ready"}))
            async for msg in ws:
                if isinstance(msg, bytes):
                    continue
                try:
                    obj = json.loads(msg)
                except json.JSONDecodeError:
                    continue
                if isinstance(obj, dict):
                    await self.send_control(obj)
        except Exception as e:
            print(f"[bridge] ws handler: {e}")
        finally:
            self.clients.discard(ws)
            print(f"[bridge] web client -1 ({len(self.clients)})")


async def main() -> None:
    ap = argparse.ArgumentParser(description="GAM Web↔TCP controller bridge")
    ap.add_argument("--agent-host", default="127.0.0.1")
    ap.add_argument("--agent-port", type=int, default=9999)
    ap.add_argument("--ws-host", default="0.0.0.0")
    ap.add_argument("--ws-port", type=int, default=9997)
    args = ap.parse_args()

    bridge = Bridge(args.agent_host, args.agent_port)
    static = Path(__file__).resolve().parent
    print(f"[bridge] WebSocket ws://{args.ws_host}:{args.ws_port}")
    print(f"[bridge] Serve UI from: {static} (e.g. python -m http.server 8080)")
    print(f"[bridge] Agent TCP {args.agent_host}:{args.agent_port}")

    async with serve(bridge.ws_handler, args.ws_host, args.ws_port, max_size=16 * 1024 * 1024):
        await asyncio.Future()


if __name__ == "__main__":
    asyncio.run(main())
