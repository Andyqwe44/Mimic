# CLAUDE.md — TicTacToe → General Visual Game AI

## ⛔ 思想钢印 — 三条铁律，每次写代码前过一遍

### 铁律 1: 中文思考回答

用中文思考和回答。代码、commit 信息、PR 描述用英文。

### 铁律 2: 日志只用 LOG()

**项目已配备统一日志系统。严禁使用任何裸打印函数。**

以下符号**不得出现**于 `logger/logger.cpp` 以外的任何 C++ 文件中：

```
printf          fprintf         fprintf(stdout     fprintf(stderr
std::cout       std::cerr       std::clog
puts            putchar         fputs              fwrite(..., stdout
WriteConsole    OutputDebugString
```

**唯一例外**：`logger/logger.cpp` 自身 + `game/src/` 终端 UI 渲染。

**唯一合法方式**：
```cpp
#include "logger/logger.h"
LOG("tag", "format_string", args...);
```

| 标签 | 用途 |
|------|------|
| `wgc` `dxgi` | 捕获 |
| `cmd` | 命令调度 |
| `main` | 主循环/启动 |
| `mjpeg` | MJPEG 服务器 |
| `ui` | 前端事件 |
| `agent` | AI Agent |

### 铁律 3: 存档 = 更新 README + 更新 CLAUDE.md + commit

每次有实质性改动（新功能、架构变更、重要修复）结束时：

1. **更新 README.md** — 如果对外接口/用法变了
2. **更新 CLAUDE.md** — 如果架构/结构/构建流程变了
3. **git commit** — 写出清晰的 commit message，描述做了什么和为什么

这三个动作是一体的。不要说"稍后再做"——做完代码立刻存档。

## Project Vision

Build self-organizing hierarchical visual game AI. Model interface: **pixels in, actions out**.
C++ for all real-time work: capture + WebView2 GUI + MJPEG + TCP + logging.
Python for AI model training/inference.
v0.3.0 — pure C++ WebView2 host, zero Rust.

## Architecture (post-migration: pure C++ WebView2 host)

```
┌─ monitor_app (C++ Win32) ────────────────────────────────────────────┐
│  React (TypeScript + Tailwind)  ←→  C++ backend (same process)      │
│       MXU-style UI               │  WebView2 COM 原生                 │
│       Dashboard/Monitor/Log       │  WebMessage bridge (ex-Tauri IPC) │
│                                   │  SharedBuffer 直推 (零 FFI)       │
│  Dev:  WebView2 → localhost:5173 │  MJPEG HTTP :9998                 │
│  Prod: WebView2 → localhost:8888 │  WIC JPEG encode                  │
└──────────────────┬───────────────────────────────────────────────────┘
                   │
     ┌─────────────┼──────────────┐
     ▼             ▼              ▼
  C++ capture     C++ logger     TCP :9999
  GDI+WGC+DXGI    (logger/)      (agent.exe / Python)
  per-method .lib

| Language | Role |
|----------|------|
| C++ | Host process: Win32 window, WebView2, capture, MJPEG server, logging |
| TypeScript/React | UI (runs inside WebView2, same code as when under Tauri) |
| Python | AI model training/inference (separate process, TCP :9999) |
```

## UI Guarantee

**React UI is 100% unchanged.** Proof:
1. `App.tsx` is same React + TypeScript + Tailwind code — only `invoke()` → `hostCall()` shim changed
2. WebView2 is same Chromium engine whether created by Tauri (Rust) or C++ — identical rendering
3. `chrome.webview.sharedbufferreceived` event is WebView2 standard API — C++ COM → JS, no Rust involved
4. MJPEG `<img src="...">` is browser standard — works regardless of host language
5. Vite HMR is independent of host — C++ navigates to `localhost:5173`, Vite WebSocket reloads on save

## Project Structure

```
tictactoe/
├── logger/                       # Unified C++ logging engine (C API)
│   ├── logger.h                  capture_log_write_msg — THE ONE write function
│   ├── logger.cpp                Thread-safe file + ring buffer implementation
│   └── build_logger_lib.cmd      MSVC → logger.lib
├── protocol/                     # Wire format — shared across C++/Python
│   ├── protocol.h / .py
├── capture/                      # C++ screen capture (per-method static libs)
│   ├── src/
│   │   ├── capture_common.cpp    Content validation + window state
│   │   ├── capture_gdi.cpp       GetWindowDC (DPI-aware)
│   │   ├── capture_pw.cpp        PrintWindow + magenta sentinel
│   │   ├── capture_screen.cpp    ScreenBitBlt (virtual screen DC)
│   │   ├── capture_desktop.cpp   DesktopBlt (virtual screen DC)
│   │   ├── capture_wgc.cpp       WGC GPU FramePool (D3D11+WinRT)
│   │   ├── capture_wgc_ffi.cpp   WGC stream FFI wrapper
│   │   ├── capture_dxgi.cpp      DXGI Desktop Duplication backend
│   │   └── capture_*.cpp         Standalone tools
│   ├── include/                  Public headers
│   ├── build.cmd                 Standalone exes
│   └── build_capture_lib.cmd     Per-method .lib: common/wgc/gdi/pw/screen/desktop
├── monitor_app/                  # C++ WebView2 host (window + commands + MJPEG + TCP)
│   ├── src/
│   │   ├── main.cpp              Win32 window + WebView2 + message loop
│   │   ├── commands.h/cpp        Command dispatch (list_windows, capture, log, stream)
│   │   ├── mjpeg_server.h/cpp    MJPEG HTTP server (Winsock2 + WIC)
│   │   └── json_helper.h         Minimal JSON parser for WebMessage
│   ├── dep/                      WebView2 SDK (header + static lib)
│   │   ├── WebView2.h
│   │   ├── WebView2EnvironmentOptions.h
│   │   └── WebView2LoaderStatic.lib
│   └── build.cmd                 MSVC → monitor_app.exe
├── monitor_web/                  # React frontend (KEEP — shared by C++ host)
│   ├── src/
│   │   └── App.tsx               MXU-style UI (hostCall bridge, no Tauri deps)
│   ├── package.json              Vite + React + Tailwind
│   └── vite.config.ts
├── model/                        # Python
│   ├── action_space.py           Token vocabulary + serialization (LE)
│   ├── generic_agent.py          VisionEncoder + ActionDecoder + GenericAgent
│   └── payload/bgra.py           Canonical BGRA pack/unpack
├── test/                         # Test artifacts
│   ├── frames/                   Debug BGRA dumps (gitignored)
│   ├── wgc_bench_capture.cpp     WGC capture-only benchmark
│   └── analyze_bench.py          Benchmark result analyzer
└── log/                          # Unified logs (gitignored)
```

## Build Commands

```bash
# 1. Build C++ static libs (once, or when C++ changes)
cd logger   && build_logger_lib.cmd
cd capture  && build_capture_lib.cmd

# 2. Build C++ WebView2 host
cd monitor_app && build.cmd          # → monitor_app.exe

# 3. Dev mode (Vite HMR)
cd monitor_web && npm run dev        # Vite on :5173
cd monitor_app && build\monitor_app.exe --dev   # WebView2 → localhost:5173

# 4. Prod mode
cd monitor_web && npm run build      # Vite → dist/
cd monitor_app && build\monitor_app.exe         # WebView2 → localhost:8888
```

## Internal Architecture (C++ host)

### Communication: WebMessage bridge (replaces Tauri invoke)

```
JS:  hostCall('list_windows') → chrome.webview.postMessage('{"cmd":"list_windows","id":1,"args":{}}')
C++: WebMessageReceived → HandleWebMessage → dispatch_command → PostWebMessageAsJson('{"id":1,"result":[...]}')
JS:  'message' event → resolve promise → return result
```

### Command dispatch (commands.cpp)

| Command | Args | Returns |
|---------|------|---------|
| `list_windows` | — | `[{title, category, hwnd}, ...]` |
| `list_processes` | — | `[{title, category:"process", hwnd:pid}, ...]` |
| `capture_window` | `{hwnd, method}` | PNG base64 + dimensions |
| `capture_stream_start` | `{hwnd, method, transport}` | `{ok:true}` |
| `capture_stream_stop` | — | `{ok:true}` |
| `read_logs` | `{max_files}` | `{live:"...", files:[...]}` |
| `clear_log` | — | `{ok:true}` |
| `log_ui_event` | `{event, detail}` | `{ok:true}` |
| `benchmark_methods` | `{hwnd, method}` | `{results:[{method, time_ms, size, ok},...]}` |
| `debug_dump_frames` | `{enable}` | `{ok:true}` |

### Streaming pipeline

```
WGC → condition_variable → TryGetNextFrame → CopyResource(GPU) → Map(CPU)
  → BGRA pixels
  → SharedBuffer: CreateSharedBuffer(w*h*4) → memcpy → PostSharedBufferToScript
  → MJPEG fallback: WIC JPEG encode → HTTP multipart → <img src=":9998/stream">
```

### SharedBuffer (zero-copy, no FFI)

C++ native COM — no Rust transmute overhead:
```cpp
ICoreWebView2Environment12* env12;
env->QueryInterface(IID_PPV_ARGS(&env12));
ComPtr<ICoreWebView2SharedBuffer> buf;
env12->CreateSharedBuffer(w * h * 4, &buf);
BYTE* dst;
buf->Open(&dst);
memcpy(dst, bgra, w * h * 4);
buf->Close();

ICoreWebView2_17* wv17;
webview->QueryInterface(IID_PPV_ARGS(&wv17));
wv17->PostSharedBufferToScript(buf.Get(), COREWEBVIEW2_SHARED_BUFFER_ACCESS_READ_ONLY, L"{}");
```

### MJPEG server (port 9998)
- Winsock2 accept loop + per-client send thread
- WIC (Windows Imaging Component) BGRA→JPEG encode, quality 0.70
- Multipart/x-mixed-replace format: `--frame\r\nContent-Type: image/jpeg\r\nContent-Length: N\r\n\r\n<bytes>\r\n`

### Capture methods

| Method | Lib | Sys deps |
|--------|-----|----------|
| WGC | wgc.lib | d3d11, dxgi, windowsapp |
| GetWindowDC | gdi.lib | user32, gdi32 |
| PrintWindow | pw.lib | user32, gdi32 |
| ScreenBitBlt | screen.lib | user32, gdi32 |
| DesktopBlt | desktop.lib | user32, gdi32 |
| Common | common.lib | user32, dwmapi |

Fallback chain (in commands.cpp): DesktopBlt → GetWindowDC → PrintWindow → ScreenBitBlt.

## Wire Protocol (protocol/)

```
Frame: [magic:4 "FRAM"][body_size:4 LE][type_tag:4 LE][body: body_size bytes]

type_tag 1 (BGRA): [w:4][h:4][ch:4][reserved:4][pixels: w*h*ch]
DEFAULT_TCP_PORT=9999, MAGIC=0x4D415246, FRAME_HEADER_SIZE=12
```

## WGC Internals

- WinRT MTA initialized on daemon thread
- DispatcherQueue created per capture thread (required for FrameArrived)
- Condition variable for efficient frame waiting (no busy-poll)
- Triple-buffered staging textures for GPU/CPU overlap
- `TryGetNextFrame` false does NOT reset `frame_ready_` (race fix)
- `signal_stop()` for non-blocking shutdown
- Win11 borderless capture (`IsBorderRequired(false)`)

## Data Flow (future: pure C++)

```
Start button → hostCall('capture_stream_start', {hwnd, method, transport})
  → commands.cpp launches WGC stream thread
  → Each frame: wgc_stream_read → BGRA
    → SharedBuffer: PostSharedBufferToScript → JS 'sharedbufferreceived' → Canvas
    → MJPEG: mjpeg_server_push_frame → WIC JPEG → HTTP multipart → <img>
Stop button → hostCall('capture_stream_stop')
  → signal_stop → join thread → stop MJPEG server
```

## Migration Status

**COMPLETE — Rust/Tauri fully removed. Project is pure C++ + TypeScript.**

- [x] logger/ — unified C++ logging engine
- [x] capture/ — per-method static libs, system libs separated
- [x] monitor_app/ — C++ WebView2 host: window, WebMessage bridge, command dispatch
- [x] monitor_app/src/mjpeg_server — MJPEG HTTP server (Winsock2 + WIC, port 9998)
- [x] monitor_app/src/commands.cpp — all backend commands (list_windows, capture, stream, log, benchmark)
- [x] monitor_web/src/App.tsx — Tauri invoke → WebView2 hostCall bridge
- [x] Remove monitor_web/src-tauri/ — deleted (Rust/Tauri)
- [x] Remove logger/logger.rs — deleted (Rust FFI)
- [x] Remove protocol/protocol.rs — deleted (Rust protocol)
- [x] Remove examples/*.rs — deleted (Rust examples)
- [x] Clean package.json — removed @tauri-apps/* dependencies

## Known Issues

1. **WGC FPS**: Event-driven — static content = low FPS. Dynamic window = 60+.
2. **H.264 MFT**: Encoder creates MP4 for progressive download, `<video>` needs full file.
3. **Yellow border**: GDI FillRect flickers on window invalidation.
4. **Overlay orphan**: Yellow overlay STATIC windows may persist if app crashes.
5. **Chromium background tab throttling**: WebView2 may throttle when app loses focus.
