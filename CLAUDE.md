# CLAUDE.md — TicTacToe → General Visual Game AI

## 语言偏好
用中文思考和回答。代码、commit、PR 描述用英文。

## Project Vision

Build self-organizing hierarchical visual game AI. Model interface: **pixels in, actions out**.
C++ for real-time capture + future agent. Rust for monitor GUI + capture IPC.
Python for AI model training/inference.

## Architecture

```
┌─ monitor_web (Tauri 2) ────────────────────────────────────┐
│  React (TypeScript + Tailwind)  ←→  Rust (IPC)            │
│       MXU-style UI               │  Win32 API 直调         │
│       Dashboard/Monitor/Log       │  TCP server :9999       │
└──────────────────┬────────────────┴────────────────────────┘
                   │
     ┌─────────────┼──────────────┐
     ▼             ▼              ▼
  Rust            C++ static lib  TCP :9999
  EnumWindows     GDI+WGC+DXGI    (agent.exe / Python)
  (0ms)           (capture_lib)    binary frames
```

## Project Structure

```
tictactoe/
├── protocol/                    # Wire format — shared across C++/Rust/Python
│   ├── protocol.h / .rs / .py
├── common/                      # Shared C++ modules
│   ├── include/
│   │   ├── types.hpp            Shared types (Rect, sleep_ms)
│   │   └── capture_helpers.hpp  ScaleBgra, IsSolidColor, etc.
│   ├── payload/bgra.hpp         BGRA pixel frame pack/unpack
│   └── transport/               pipe.hpp, tcp.hpp
├── capture/                     # C++ screen capture (static lib + standalone tools)
│   ├── src/
│   │   ├── capture_common.cpp   Content validation + window state (FFI)
│   │   ├── capture_gdi.cpp      GetWindowDC (FFI, DPI-aware)
│   │   ├── capture_pw.cpp       PrintWindow + magenta sentinel (FFI, DPI-aware)
│   │   ├── capture_screen.cpp   ScreenBitBlt (FFI, virtual screen DC)
│   │   ├── capture_desktop.cpp  DesktopBlt (FFI, virtual screen DC)
│   │   ├── capture_auto.cpp     Auto-detect fallback chain (FFI)
│   │   ├── capture_wgc.cpp      WGC GPU FramePool (D3D11+WinRT, OBS patterns)
│   │   ├── capture_wgc_ffi.cpp  WGC stream FFI wrapper
│   │   ├── capture_dxgi.cpp     DXGI Desktop Duplication backend
│   │   ├── capture_single.cpp   Standalone: single-frame screenshot
│   │   ├── capture_stream.cpp   Standalone: stream with frame-differ
│   │   └── capture_wgc_main.cpp Standalone: WGC CLI (single/stream)
│   ├── include/
│   │   ├── capture_methods.h    Public FFI header (all methods)
│   │   ├── capture_wgc_ffi.h    WGC stream FFI header
│   │   ├── capture_internal.h   Shared GDI inline helpers + DpiGuard RAII
│   │   ├── capture_wgc.hpp      WGC C++ class (FrameArrived + condition_variable)
│   │   └── capture.hpp          ICaptureBackend (DXGI + GDI)
│   ├── build.cmd                MSVC build (standalone exes)
│   └── build_capture_lib.cmd    MSVC → capture_lib.lib (8 FFI files)
├── monitor_web/                 # Tauri 2 + React desktop app
│   ├── src/
│   │   └── App.tsx              Main UI (MXU-style, Dashboard/Screenshot/Log/Settings)
│   └── src-tauri/
│       ├── src/
│       │   ├── main.rs          Rust backend (WGC FFI, MJPEG server, H.264 MFT, log)
│       │   ├── mjpeg_server.rs  MJPEG HTTP server (JPEG encode + multipart stream)
│       │   └── h264_encoder.rs  H.264 MFT GPU encoder (mpsc channel → MP4)
│       ├── Cargo.toml           v0.2.0 (single source of version truth)
│       └── tauri.conf.json      Window config + metadata
├── model/                       # Python
│   ├── __init__.py               Re-exports public API
│   ├── action_space.py           Token vocabulary + serialization (LE)
│   ├── generic_agent.py          VisionEncoder + ActionDecoder + GenericAgent
│   ├── hierarchical.py           PerceptionSpecialist + StrategicReasoner
│   └── payload/
│       └── bgra.py               Canonical BGRA pack/unpack for Python
├── test/                        # Test artifacts
│   ├── frames/                  Debug BGRA dumps (gitignored)
│   │   └── .gitkeep
│   └── view_frame.py            Python frame viewer
├── examples/                    # Protocol examples + Benchmark
│   ├── wgc_bench_send.cpp       WGC→TCP benchmark (C++)
│   ├── wgc_bench_recv.rs        TCP→file benchmark (Rust)
│   └── run_bench.bat
└── log/                         # Unified logs (gitignored)
    ├── agent_*.log               Rust (Tauri main process)
    └── wgc_*.log                 C++ (WGC subprocess)
```

## Wire Protocol (protocol/)

```
Frame: [magic:4 "FRAM"][body_size:4 LE][type_tag:4 LE][body: body_size bytes]

type_tag 1 (BGRA): [w:4][h:4][ch:4][reserved:4][pixels: w*h*ch]

DEFAULT_TCP_PORT=9999, MAGIC=0x4D415246, FRAME_HEADER_SIZE=12
```

`body_size` = body bytes only (NOT including type_tag). Matches Rust `build_header(payload.len(), type_tag)`.

## Build Commands

```bash
cd capture  && build.cmd              # Standalone C++ tools
cd capture  && build_capture_lib.cmd  # Static lib (Rust build.rs calls this)
cd monitor_web
npm install && npm run tauri dev      # Vite HMR + Cargo watch
npm run tauri build                   # Release .exe (statically linked)
```

## Version

Version is defined in `Cargo.toml` only. `main.rs` reads it via `env!("CARGO_PKG_VERSION")`.

## Capture Pipeline

### Streaming preview (Play button → WGC + MJPEG)

```
WGC GPU FramePool (D3D11+WinRT, OBS patterns)
  → FrameArrived event → condition_variable
  → TryGetNextFrame → CopyResource(GPU) → Map(CPU readback)
  → C++ FFI (wgc_stream_read) → scaled BGRA (max 1280)
  → Rust: BGRA→RGBA swap + MJPEG push (raw BGRA store)
  → MJPEG server thread: BGRA→RGB → JPEG encode (quality 70) → HTTP multipart
  → Frontend: <img src="http://127.0.0.1:9998/stream">
```

### Single-frame capture (Camera button → GDI fallback)

GDI chain: DesktopBlt / GetWindowDC / PrintWindow / ScreenBitBlt / Auto-detect

### Transport methods

| Transport | Encoding | Transfer | Browser | Settings |
|-----------|----------|----------|---------|----------|
| MJPEG | CPU JPEG ~5ms | HTTP port 9998 | `<img>` GPU | default |
| H.264 | GPU MFT (wip) | MP4 port 9997 | `<video>` | experimental |
| Base64 | none | JSON invoke | Canvas ImageData | legacy |

Select in Settings → Transport card.

### WGC Internals

- WinRT MTA initialized on daemon thread (avoids Tauri STA conflict)
- DispatcherQueue created per capture thread (required for FrameArrived)
- Condition variable for efficient frame waiting (no busy-poll)
- Triple-buffered staging textures for GPU/CPU overlap
- FrameArrived event registered → callback sets `frame_ready_` + notifies CV
- `wait_frame()` blocks on CV with 100ms timeout
- `TryGetNextFrame` false does NOT reset `frame_ready_` (race fix)
- `signal_stop()` for non-blocking shutdown (avoids `worker.join()` hang)
- Win11 borderless capture (`IsBorderRequired(false)`)

### H.264 MFT Internals

- mpsc channel: capture thread → dedicated encoder thread (IMFSinkWriter not Send)
- `MFStartup`/`MFShutdown` via raw FFI (not in windows crate 0.58)
- SinkWriter → MP4 file, HTTP server serves for `<video>` progressive download
- `test/video.mp4` output (gitignored)

## Frontend (App.tsx)

Single-file React app. Key components:
- `TopBar` — MXU-style tabs: Dashboard | Monitor | Log + Start/Stop + Theme + Settings
- `ConnectionPanel` — Window selector, method, capture mode, IP/Port
- `ScreenshotPanel` — MJPEG `<img>` preview (streaming) + PNG `<img>` (single-frame)
- `LogPanel` — Unified log view (in-memory `[live]` + disk files)
- `DashboardView` — System info, Capture Pipeline, Update, Resources
- `SettingsPage` — Connection, Transport, Theme, Model, Update, Log, Project
- `WindowPickerModal` — Window/desktop/process selection with search

### Right panel sizing
- Default width: 324px, min: 324px, max: 400px
- Auto-collapse chain: Log → Screenshot → Connection when overflow

## Rust Backend (main.rs)

### Default window size

Defined at top:
```rust
const DEFAULT_WINDOW_W: u32 = 1280;
const DEFAULT_WINDOW_H: u32 = 720;
```

### Key commands
- `list_windows` / `list_processes` — Win32 enumeration (pure Rust)
- `capture_window(hwnd, method)` — Single-frame via C++ FFI
- `capture_stream_start(app, hwnd, tcp_port, method, transport)` — WGC stream
- `capture_stream_stop` — Signal stop + join capture thread + stop MJPEG/H.264
- `stream_poll` — Returns JSON {p: base64, w, h, m} for Canvas fallback
- `read_logs(max_files)` — Returns `[live]` (in-memory buffer) + disk files
- `log_ui_event` / `clear_log` — Frontend → disk + memory log bridge
- `benchmark_methods` — Test all methods, return timings
- `debug_dump_frames(bool)` — Toggle raw BGRA dump to test/frames/

### Logging
- `dlog!()` macro writes to BOTH file AND in-memory `LOG_MEMORY` buffer
- `read_logs()` returns `[live]` entry first (in-memory), then disk files
- In-memory buffer capped at 5000 entries
- Clear button clears both file and memory

## Known Issues

1. **WGC FPS**: Event-driven — static content = low FPS. Dynamic window = 60+.
2. **H.264 MFT**: Encoder creates MP4 for progressive download but `<video>` needs full file. MSE + fMP4 needed for true live streaming.
3. **Yellow border**: GDI FillRect flickers on window invalidation.
4. **Overlay orphan**: Yellow overlay STATIC windows may persist if app crashes.
5. **MJPEG port race**: `stop_mjpeg_server` pokes old listener to force unbind. Retry loop in bind.
6. **texture_stream.rs**: Attempted ICoreWebView2ExperimentalTextureStream — not in webview2-com-sys 0.38.2.
