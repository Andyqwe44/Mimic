# Game Agent Monitor

Desktop monitor for visual game AI — **pixels in, actions out**.

## Architecture

```
┌─ monitor_app (C++ Win32) ───────────────────────────────────┐
│  React UI (TypeScript + Tailwind)  ←→  C++ backend           │
│  WebView2 browser control            WebMessage bridge        │
│  Dashboard / Monitor / Log           SharedBuffer zero-copy   │
│  Dev:  WebView2 → localhost:1420    Stream bridge             │
│  Prod: WebView2 → gam.local         TCP protocol :9999       │
│  Prod: WebView2 → gam.local                                 │
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

# 2. Build C++ host
cd monitor_app && build.cmd

# 3. Start Vite dev server (terminal 1)
cd monitor_web && npm install && npm run dev

# 4. Launch GUI (terminal 2) — Vite HMR
cd monitor_app && build\monitor_app.exe --dev
```

`--dev`: navigate to Vite dev server (hot reload).

### Developer Mode
Enable via **Settings → General → Dev mode**. Saves captured frames as PNG:
- **Save single-frame captures** — each 📷 snapshot to disk
- **Save live preview frames** — each ▶ preview frame to disk
- Pick output directory via folder picker, open in Explorer

### Production

```bash
cd monitor_web && npm run build          # → dist/
cd monitor_app  && build.cmd             # → monitor_app.exe
```

Distribute `monitor_app.exe` + `monitor_web/dist/` together. No HTTP server — WebView2 uses `SetVirtualHostNameToFolderMapping` to load from disk.

## Project Structure

```
tictactoe/
├── logger/               C++ logging engine (capture_log_write_msg)
├── capture/              C++ screen capture (per-method .lib)
├── monitor_app/          C++ WebView2 host (main window + commands + MJPEG)
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
- **Settings** — Connection, transport, theme, model, log config
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
