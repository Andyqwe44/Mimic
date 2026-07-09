# Game Agent Monitor

Desktop monitor for visual game AI — **pixels in, actions out**.

## Architecture

```
┌─ monitor_app (C++ Win32) ───────────────────────────────────┐
│  React UI (TypeScript + Tailwind)  ←→  C++ backend           │
│  WebView2 browser control            WebMessage bridge        │
│  Dashboard / Monitor / Log           SharedBuffer zero-copy   │
│  Dev:  build_dev\monitor_app.exe → localhost:1420  SharedBuffer zero-copy         │
│  Prod: build\monitor_app.exe → gam.local          Stream bridge                   │
└──────────┬───────────────────────────────────────────────────┘
           │
    ┌──────┼────────┐
    ▼      ▼        ▼
  C++      C++      TCP :9999
  capture  logger   (agent / Python)
  WGC+DI  file+mem  binary frames
```

**Zero Rust. Single MSVC command builds everything.**

| Language | Role |
|----------|------|
| C++17 | Host: Win32 window, WebView2, capture, SharedBuffer, stream bridge, TCP, logging |
| TypeScript/React | UI inside WebView2 (same code regardless of host) |
| Python | AI model training/inference (separate process, TCP :9999) |

## Quick Start

### Prerequisites
- Windows 10/11 with Visual Studio 2022 (C++ tools)
- Node.js 18+, WebView2 Runtime (pre-installed on Win11)

### Dev Mode

```bash
# 1. Build C++ static libs (first time only, re-run after C++ changes)
cd logger   && build_logger_lib.cmd
cd capture  && build_capture_lib.cmd

# 2. Dev build (Vite HMR, debug symbols)
cd monitor_web && npm install && npm run dev   # terminal 1: Vite :1420
cd monitor_app && build_dev.cmd                # terminal 2: -> build_dev\monitor_app.exe
# Navigates to http://localhost:1420 (hot reload)
```

### Production

```bash
# 3. Prod build (optimized, self-contained)
cd monitor_web && npm run build          # Vite -> dist/
cd monitor_app && build.cmd              # embeds dist + compiles icon/version -> build\monitor_app.exe
# Navigates to https://gam.local/index.html (dist embedded in exe, served from memory)
```

Mode set at build time via `/DDEV_MODE` preprocessor define. No runtime `--dev` flag.

Distribute the **single** `monitor_app.exe` — the frontend `dist/` is compiled into the
exe (byte arrays served from memory via WebResourceRequested), and the WebView2 loader is
statically linked. Only external prerequisite is the WebView2 Runtime (system-level, Win11
built-in). No HTTP server, no external files.

## Project Structure

```
tictactoe/
├── logger/               C++ logging engine (capture_log_write_msg)
├── capture/              C++ screen capture (per-method .lib)
├── monitor_app/          C++ WebView2 host (main window + commands + single-instance)
│   ├── build.cmd         Production build (optimized)
│   ├── build_dev.cmd     Dev build (debug symbols, no opt)
│   └── dep/              WebView2 SDK
├── monitor_web/          React frontend (Vite + TypeScript + Tailwind)
├── protocol/             Wire format (C++/Python)
├── model/                Python AI
└── test/                 Benchmarks + frame viewer
```

## Capture Methods

| Method | .lib | Description |
|--------|------|-------------|
| WGC | wgc.lib | GPU FramePool, D3D11+WinRT, 60+ FPS (window or monitor) |
| WGC Monitor | wgc.lib | GPU desktop capture via monitor handle |
| DesktopBlt | desktop.lib | GDI full desktop capture, fast single-frame (<10ms) |
| GetWindowDC | gdi.lib | Window DC capture |
| PrintWindow | pw.lib | WM_PRINT-based, magenta detection |
| ScreenBitBlt | screen.lib | Virtual screen BitBlt |

**Frontend decides, C++ executes.** Method passed via `{hwnd, method}` — no silent fallback.
Desktop single-frame → `dxgi` (DesktopBlt). Window single-frame → `wgc`. Streaming → `wgc` only.

## Transport Methods

| Transport | Port | Description |
|-----------|------|-------------|
| SharedBuffer | COM | Zero-copy GPU→Canvas, main path |
| MJPEG | 9998 | JPEG over HTTP multipart, fallback |
| TCP | 9999 | Wire protocol, external agent/Python |

## Wire Protocol (TCP :9999)

```
Frame: [magic:4 "FRAM"][body_size:4 LE][type_tag:4 LE][body]

type_tag 1 (BGRA): [w:4][h:4][ch:4][reserved:4][pixels: w*h*ch]
```

## Features

- **Dashboard** — System info, capture pipeline status, update check
- **Monitor** — Window/desktop capture, streaming preview, FPS counter
- **Log** — Live in-memory ring buffer + disk log tiles
- **Settings** — Connection, capture method (snapshot/stream/render), theme, model, log config
- **Virtual desktop** — Cross-desktop window enumeration, absolute numbering (Task View order via registry), desktop switching
- **Window picker** — EnumWindows with search, type filter (All/Desktop/Window), D1/D2 badges, ⚡ for remote-desktop windows
- **Single-frame** — WGC/GDI multi-method capture with PNG output

## WGC Internals

- MTA daemon thread (avoids STA conflict with WebView2)
- DispatcherQueue per capture thread
- Condition variable frame wait, 100ms timeout
- Triple-buffered staging textures
- `TryGetNextFrame` false → does NOT reset `frame_ready_` (race fix)
- `signal_stop()` → non-blocking shutdown
- Win11 borderless capture (`IsBorderRequired(false)`)

## License

MIT
