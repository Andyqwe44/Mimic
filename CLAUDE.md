# CLAUDE.md — TicTacToe → General Visual Game AI

## Project Vision

Build a self-organizing hierarchical visual game AI. Model interface: **pixels in, actions out**.
C++ for real-time capture + future agent. Rust for monitor GUI + capture IPC.
Python for AI model training/inference.

## Architecture

```
┌─ monitor_web (Tauri 2) ────────────────────────────────┐
│  React (TypeScript + Tailwind)  ←→  Rust (IPC)        │
│       UI 界面                   │  Win32 API 直调      │
│                                  │  TCP server :9999    │
└──────────────────┬───────────────┴─────────────────────┘
                   │
     ┌─────────────┼──────────────┐
     ▼             ▼              ▼
  Rust            Rust           TCP :9999
  EnumWindows     GDI Capture    (agent.exe / Python)
  (窗口枚举)       (单帧+流)       binary frames
                    │
          ┌────────┼────────┐
          ▼        ▼        ▼
    GetWindowDC  PrintWindow  ScreenBitBlt
    品红哨兵检测  纯色检测   自动回退链
```

## Project Structure

```
tictactoe/
├── protocol/                    # Wire format — shared across C++/Rust/Python
│   ├── protocol.h               C/C++ constants + header helpers
│   ├── protocol.rs              Rust: PayloadType enum, build/parse header
│   └── protocol.py              Python: PayloadType IntEnum, header struct
│
├── common/                      # Shared C++ modules
│   ├── include/                 types.hpp, signals.hpp
│   ├── payload/bgra.hpp         BGRA pixel frame pack/unpack
│   └── transport/
│       ├── pipe.hpp             PipeSender/PipeReceiver (stdout/stdin)
│       └── tcp.hpp              TcpSender broadcast server
│
├── capture/                     # C++ screen capture tools
│   ├── src/                     capture_dxgi.cpp, capture_h264.cpp, mf_encoder.cpp, ...
│   ├── include/                 capture.hpp, mf_encoder.hpp, preprocess.hpp
│   └── build.cmd                MSVC build (all .exe in build/)
│
├── monitor_web/                 # Tauri 2 + React desktop app
│   ├── src/
│   │   ├── App.tsx              Main UI (Tooltip/IconBtn/ActionBtn components)
│   │   ├── index.css            Tailwind + CSS variables + scrollbar-gutter
│   │   └── main.tsx             React entry
│   └── src-tauri/
│       ├── src/
│       │   ├── main.rs          Rust backend (capture, stream, TCP, IPC)
│       │   ├── fmp4.rs          fMP4 ISOBMFF builder (H.264 MSE, future)
│       │   ├── protocol.rs      include!(shared protocol/protocol.rs)
│       │   ├── payload/bgra.rs  BGRA frame pack/unpack
│       │   └── transport/pipe.rs Frame send/recv over pipe
│       ├── Cargo.toml           tauri, windows, serde, chrono, miniz_oxide
│       └── tauri.conf.json      1200x780 window, devUrl:1420
│
├── model/                       # Python
│   ├── payload/bgra.py          BGRA frame pack/unpack + StreamClient
│   └── stream_protocol.py       (legacy, being migrated to protocol/)
│
└── examples/                    # End-to-end protocol examples
    ├── hello_cpp_send.cpp       C++ pipe → Rust "hello world" ✓ tested
    ├── hello_rust_recv.rs
    ├── hello_tcp_send.cpp       C++ TCP → Python
    └── hello_python_recv.py
```

## Wire Protocol (protocol/)

```
Frame: [magic:4 "FRAM"][payload_size:4 LE][type_tag:4 LE][payload_body]

type_tag:
  0 = NONE          (unchanged frame / heartbeat)
  1 = BGRA_FRAME    [w:4][h:4][ch:4][reserved:4][pixels: w*h*ch]
  2 = H264_STREAM   (future)
  3 = CONTROL_MSG   (future JSON/text)

Transport constants: DEFAULT_TCP_PORT=9999, MAGIC=0x4D415246, FRAME_HEADER_SIZE=12
```

## Layered Architecture (解耦原则)

```
应用层 (payload/)       ← 知道 BGRA/H264 格式, pack/unpack
    ↓ 只依赖
协议层 (protocol/)      ← 只有常量: magic, type tags, header 结构
    ↑ 只依赖
传输层 (transport/)     ← 只搬运字节, 不管内容: send(type_tag, bytes) / recv()
```

transport 不 import payload。payload 不 import transport。加新格式只加 type_tag + payload 文件。

## Build Commands

```bash
# C++ modules
cd capture  && build.cmd    # all tools

# Tauri monitor (dev: HMR hot reload on frontend)
cd monitor_web
npm install
npm run tauri dev            # Vite HMR + Rust debug (slow, frontend only)
npm run tauri build          # Release .exe (fast, production)

# Python
pip install torch numpy opencv-python
```

## Capture Methods (Rust-native, no subprocess)

**Stream preview** — multi-method fallback:
1. `GetWindowDC + BitBlt` (~2-5ms) — fast, fails for occluded/minimized
2. `PrintWindow(PW_RENDERFULLCONTENT)` (~15-30ms) — magenta sentinel detection
3. `ScreenBitBlt` at window coords (~2-5ms) — last resort

**Single-frame** — same 3-method chain, returns PNG base64 + window position JSON.

**TCP stream** — raw BGRA frames broadcast on `127.0.0.1:9999` when preview is active.
Protocol: `[magic:4][size:4][type_tag:4][w:4][h:4][ch:4][0:4][pixels]`

## Key Performance Numbers

| Metric | Old (exe spawn) | New (Rust-native) |
|--------|-----------------|-------------------|
| Window list | 5000ms | 0ms |
| Single capture | 5000ms | 8-37ms |
| Stream capture | ~30ms/frame | 2-30ms/frame |
| BGRA pack | N/A | ~12μs |
| BMP+base64 | ~5ms | ~0.5ms |

## GPU H.264 Pipeline (capture_h264.exe)

Status: **encoder init fails on AMD GPU** — MFT returns 0 input types.
Tried: MFT_MESSAGE_SET_D3D_MANAGER, D3D11_AWARE, codec props, multiple input formats.
All attempts return MF_E_INVALIDMEDIATYPE. Fallback: BMP streaming via Rust GDI.

Architecture when working:
```
FramePool/WGC → MF H.264 HW Encoder → [size:4][NAL data] → stdout + TCP :9998
```

## Known Issues

1. **MF H.264 encoder**: AMD CLSID ADC9BC80 rejects all input types — GPU driver issue
2. **DXGI desktop**: returns solid black (virtual display adapter) — GDI fallback used
3. **process_list.exe**: still 5s spawn (low priority, Process tab rarely used)
4. **Preview timing logs**: cap_us/pack_us/bmp_us every 30 frames in agent_*.log
5. **Single-frame timing**: total_ms + encode_ms per capture in agent_*.log

## Stream Debug Log

```
stream timing: cap=3500us pack=12us bmp=450us    ← every 30 frames
capture: total=14ms encode=6ms method=PrintWindow  ← single-frame
stream: detected method=PrintWindow state=normal 1920x1080
capture: GetWindowDC → solid(0,0,0)
capture: PrintWindow → magenta sentinel detected
capture: ALL methods failed for hwnd=xxx state=minimized
```
