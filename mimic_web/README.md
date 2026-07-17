# Game Agent Monitor v0.3.0

Pure C++ WebView2 host for screen capture + monitoring. Part of the TicTacToe → Visual Game AI project.

## Quick start

```bash
cd monitor_web
npm install
npm run dev
```

## Architecture

```
Rust (Tauri IPC)  ←→  C++ capture_lib.lib (static linked)
  webview backend       WGC GPU FramePool (GDI disabled for streaming)
  MJPEG server :9998    H.264 MFT encoder (experimental)
  TCP broadcast :9999   overlay / log

React (TypeScript + Tailwind)
  MXU-style UI: Dashboard / Monitor / Log / Settings
```

Capture: WGC only (GDI kept for single-frame Camera button, but streaming always uses WGC).
Transport: MJPEG (default, JPEG HTTP stream) / Base64 (legacy) / H.264 (GPU MFT, experimental).

```
monitor_web/
├── src/
│   └── App.tsx                       # React UI (LogManager, ScreenshotPanel, Settings)
└── src-tauri/
    ├── src/
    │   ├── main.rs                   # Rust backend (FFI, WGC stream, MJPEG, H.264, log)
    │   ├── mjpeg_server.rs           # MJPEG HTTP server + JPEG encoder
    │   ├── h264_encoder.rs           # H.264 MFT GPU encoder (mpsc channel)
    │   └── transport/pipe.rs         # TCP frame transport
    ├── build.rs                      # Invokes MSVC → capture_lib.lib
    ├── tauri.conf.json               # App metadata + window config
    └── Cargo.toml                    # v0.3.0 (removed — migrated to C++ WebView2 host)

capture/
├── src/
│   ├── capture_wgc.cpp               # WGC FramePool (D3D11 + WinRT, OBS patterns)
│   ├── capture_wgc_ffi.cpp           # WGC stream FFI + DispatcherQueue
│   ├── capture_dxgi.cpp              # DXGI Desktop Duplication
│   ├── capture_common.cpp            # Content validation + window state
│   ├── capture_gdi.cpp               # GetWindowDC (DPI-aware)
│   ├── capture_pw.cpp                # PrintWindow (magenta sentinel)
│   ├── capture_screen.cpp            # ScreenBitBlt (virtual screen DC)
│   ├── capture_desktop.cpp           # DesktopBlt (virtual screen DC)
│   └── capture_auto.cpp              # Auto-detect fallback chain
├── include/
│   ├── capture_wgc.hpp               # WGC C++ class (FrameArrived + CV)
│   ├── capture_wgc_ffi.h             # WGC stream FFI header
│   ├── capture_methods.h             # Public FFI (all GDI methods)
│   └── capture_internal.h            # Shared GDI helpers + DPI guard
├── build.cmd                         # Standalone h264/dxgi experiment exe (aux)
└── (DLLs built by scripts\Build.ps1 -Module capture)
```

## Transport Methods

| Transport | Encode | Transfer | Decode | FPS |
|-----------|--------|----------|--------|-----|
| **MJPEG** (default) | CPU JPEG ~5ms | HTTP multipart :9998 | Browser GPU `<img>` | 60+ |
| H.264 MFT (wip) | GPU MFT H.264 | MP4 file :9997 | Browser GPU `<video>` | - |
| Base64 (legacy) | base64 ~200ms | JSON invoke | JS atob+ImageData | 5 |

Select in Settings → Transport.

## Default window size

Defined at top of `src-tauri/src/main.rs`:

```rust
const DEFAULT_WINDOW_W: u32 = 1280;
const DEFAULT_WINDOW_H: u32 = 720;
```

**To change default size, edit the Rust consts — NOT `tauri.conf.json`.**

Minimum window size: 324×216 (in `tauri.conf.json`).

## Logging

- `log/agent_*.log` — Rust backend (version auto from Cargo.toml)
- `log/wgc_*.log` — C++ WGC per-frame timing
- `dlog!()` writes to BOTH file and in-memory buffer
- `read_logs()` returns `[live]` (in-memory) + disk files
- LogManager class in App.tsx unifies all views
- `log/` is gitignored

## Test artifacts

- `test/frames/` — debug frame dumps (`.bgra`, gitignored)
- `test/view_frame.py` — Python frame viewer
- `test/video.mp4` — H.264 encoder output (gitignored)

## Version

Version is defined in `Cargo.toml` only. `main.rs` reads it via `env!("CARGO_PKG_VERSION")`. No other files need updating.
