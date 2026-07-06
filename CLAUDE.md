# CLAUDE.md — TicTacToe → General Visual Game AI

## Project Vision

Build a self-organizing hierarchical visual game AI. Model interface: **pixels in, actions out**.
C++ for real-time capture + future agent. Rust for monitor GUI + capture IPC.
Python for AI model training/inference.

## Architecture

```
┌─ monitor_web (Tauri 2) ──────────────────────────────────┐
│  React (TypeScript + Tailwind)  ←→  Rust (IPC)          │
│       MXU-style UI               │  Win32 API 直调       │
│       Dashboard/Monitor/Log       │  TCP server :9999     │
└──────────────────┬────────────────┴──────────────────────┘
                   │
     ┌─────────────┼──────────────┐
     ▼             ▼              ▼
  Rust            Rust           TCP :9999
  EnumWindows     GDI + WGC      (agent.exe / Python)
  (0ms)           (多方法回退)     binary frames
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
├── capture/                     # C++ screen capture tools
│   ├── src/
│   │   ├── capture_dxgi.cpp     DXGI Desktop Duplication backend
│   │   ├── capture_single.cpp   Single-frame screenshot
│   │   ├── capture_stream.cpp   Stream with frame-differ
│   │   ├── capture_h264.cpp     H.264 GPU encode (broken)
│   │   ├── capture_wgc.cpp      WGC FramePool library (GPU, 7ms/frame)
│   │   └── capture_wgc_main.cpp WGC standalone CLI (single/stream modes)
│   ├── include/
│   │   ├── capture.hpp          ICaptureBackend (DXGI + GDI)
│   │   └── capture_wgc.hpp      WGC FramePool API
│   └── build.cmd                MSVC build
├── monitor_web/                 # Tauri 2 + React desktop app
│   ├── src/
│   │   └── App.tsx              Main UI (MXU-style, Dashboard/Screenshot/Log)
│   └── src-tauri/
│       └── src/main.rs          Rust backend (WGC subprocess, overlay, TCP)
├── model/                       # Python
│   ├── __init__.py               Re-exports public API
│   ├── action_space.py           Token vocabulary + serialization (LE)
│   ├── generic_agent.py          VisionEncoder + ActionDecoder + GenericAgent
│   ├── hierarchical.py           PerceptionSpecialist + StrategicReasoner
│   └── payload/
│       └── bgra.py               Canonical BGRA pack/unpack for Python
├── examples/                    # Protocol examples + Benchmark
│   ├── wgc_bench_send.cpp       WGC→TCP benchmark (C++)
│   ├── wgc_bench_recv.rs        TCP→file benchmark (Rust)
│   └── run_bench.bat
└── log/                         # Unified logs
    ├── agent_*.log               Rust (Tauri main process)
    └── wgc_*.log                 C++ (WGC subprocess)
```

## Wire Protocol (protocol/)

```
Frame: [magic:4 "FRAM"][body_size:4 LE][type_tag:4 LE][body: body_size bytes]

type_tag 1 (BGRA): [w:4][h:4][ch:4][reserved:4][pixels: w*h*ch]

DEFAULT_TCP_PORT=9999, MAGIC=0x4D415246, FRAME_HEADER_SIZE=12
```

Note: `body_size` = body bytes only (NOT including type_tag). Matches Rust `build_header(payload.len(), type_tag)`.

### BGRA payload

Canonical implementations (use these for new code):
- C++: `common/payload/bgra.hpp` (`payload::bgra_pack/unpack`)
- Rust: `monitor_web/src-tauri/src/payload/bgra.rs` (`payload::bgra::pack/unpack`)
- Python: `model/payload/bgra.py` (`pack/unpack`)

## Build Commands

```bash
cd capture  && build.cmd          # C++ tools (capture_wgc.exe + others)
cd monitor_web
npm install && npm run tauri dev  # Vite HMR + Cargo watch
npm run tauri build               # Release .exe
```

## Capture Methods

### WGC (Windows.Graphics.Capture) — Window capture

GPU-accelerated FramePool. ~7ms/frame (140+ FPS capable).
- Works for occluded/background windows (NOT minimized)
- Event-driven: frames only produced when content changes
- `capture_stream_start` tries WGC subprocess first for hwnd≠0
- Falls back to GDI if `capture_wgc.exe` not found or fails
- Triple-buffered staging textures for GPU/CPU overlap
- C++ writes per-frame timing to `log/wgc_*.log`

### GDI — Desktop / fallback

3-method chain: `GetWindowDC → PrintWindow(magenta sentinel) → ScreenBitBlt`

### DXGI Desktop Duplication — Desktop only

Full desktop capture at monitor refresh rate. Used via `bench_send.exe 0`.

## Frontend (App.tsx)

Single-file React app. Key components:
- `TopBar` — MXU-style tabs: Dashboard | Monitor | Log + Start/Stop + Theme + Settings
- `ConnectionPanel` — Fixed-width layout, `justify-between`. Title(144px)+X(32px) left, Select right. IP(184px) left, Port(80px) right
- `ScreenshotPanel` — Canvas rendering (BGRA→RGBA, no BMP). Dynamic `aspectRatio` from screen resolution. Play/Stop streaming + Camera single-shot. Capture method selector in header.
- `LogPanel` — Dual mode: compact right-sidebar shows current session (max 100); Log tab shows full-card layout with historical files from disk via `read_logs`
- `DashboardView` — System info, Capture Pipeline, Update (check + source selector), Resources
- `SettingsPage` — `SettingsCard` collapsible cards: Connection (with capture method selector), Theme, Model, Update, Log, Project
- `WindowPickerModal` — Window/desktop/process selection with search

### Capture method selector
User can explicitly choose capture method in Settings → Connection:
- `Auto` — current fallback chain (WGC → GetWindowDC → PrintWindow → ScreenBitBlt)
- `WGC` — GPU FramePool (stream via subprocess, single-frame via `--single`)
- `DXGI` — desktop GDI BitBlt
- `GDI` — GetWindowDC only, no solid-color check
- `PrintWindow` / `ScreenBlt` — single method, no fallback

Method flows as single variable `forceMethod` through: App state → ScreenshotPanel props → invoke calls → Rust backend. All user actions logged via `addLog()`.

### User action logging
Every interaction logged: tab switches, Start/Stop, theme toggle, method selection, capture preview, screenshot, clear logs. Log tab loads historical `agent_*.log` files as full-card tiles via `read_logs` Rust command.

### MXU-style design patterns
- Cards: `bg-bg-secondary rounded-xl ring-1 ring-inset ring-border overflow-hidden`
- Header: `<div role="button">` with icon + title + status + chevron
- Expand/collapse: `grid` animation (`grid-template-rows: 1fr ↔ 0fr`)
- Action buttons: individual `e.stopPropagation()`, chevron without (allows toggle)
- TopBar tabs: `border-b-[3px]` active indicator, icons + labels

### Right panel sizing
- Default width: 324px, min: 324px, max: 400px
- Drag resize capped by `Math.max(324, Math.min(400, w))`

## Rust Backend (main.rs)

### Key commands
- `list_windows` / `list_processes` — Win32 enumeration (0ms)
- `capture_window(hwnd, method)` — Single-frame. Auto uses 3-method chain; explicit method bypasses fallback
- `capture_stream_start(app, hwnd, tcp_port, method)` — Stream preview. WGC subprocess or GDI loop. Explicit method forces specific capture path.
- `stream_poll` — Returns JSON `{p: base64, w, h, m: method}` for Canvas
- `highlight_window` — Yellow border overlay on target window
- `screen_info` — Returns `{w, h}` for screen resolution
- `read_logs(max_files)` — Reads newest N `agent_*.log` files, returns `[{name, lines}]`
- `h264_stream_start/stop` — H.264 GPU encode (broken on AMD)

### WGC integration
Stream: `capture_stream_start(hwnd, method="wgc")` spawns `capture_wgc.exe --stream --scale 1280` → reads BGRA frames from stdout.
Single-frame: `capture_window(hwnd, method="wgc")` spawns `capture_wgc.exe --single --scale 1280` → reads one frame.
Falls back to GDI if exe not found and method is "auto". If method explicitly "wgc" and exe missing → returns error.

### Yellow border overlay
- 4 thin STATIC popup windows (top/bottom/left/right, 3px, yellow GDI FillRect)
- `SetWinEventHook(EVENT_SYSTEM_MOVESIZEEND)` — event-driven, fires on mouse release
- Z-order: `SetWindowPos(h, target)` — follows target, covered when target is covered
- Inset: `BORDER_INSET=1` shrinks border inward for tight fit
- Clamped to screen edges for maximized windows

### Frame pipeline
- WGC: BGRA pixels → `bgra_to_rgba()` (swap R↔B) → store in `STREAM_FRAME`
- `stream_poll()`: base64-encode raw RGBA → JSON → frontend Canvas ImageData
- GDI: same pipeline, no BMP conversion needed
- TCP broadcast: raw BGRA frames on `127.0.0.1:9999` for external consumers

## C++ File Logging

`capture_wgc_main.cpp` writes `wgc_log()` to both stderr and `../../log/wgc_*.log`.
Format: `[seconds.ms.us] message`. Falls back to same directory if `log/` unreachable.

Rust `init_log()` walks up from exe dir looking for existing `log/` directory.
If found (at project root), writes `agent_*.log` there.

## Known Issues

1. **MF H.264 encoder**: AMD CLSID ADC9BC80 rejects all input types
2. **DXGI desktop**: returns solid black on virtual display adapters
3. **process_list.exe**: 5s spawn (low priority)
4. **Yellow border color**: GDI FillRect on STATIC — repaints white on window invalidation. Tracker repaints on every reposition, but may flicker
5. **WGC init latency**: ~300ms for first frame after subprocess spawn
6. **WGC FPS**: Limited by window content change rate. Static window = 0-5 FPS. Dynamic = 60+ FPS. This is by design (event-driven)
7. **Subprocess cleanup**: WGC subprocess killed 500ms after stream stop. Occasionally leaves orphan `capture_wgc.exe` processes
8. **Overlay orphan risk**: Yellow overlay STATIC windows may persist if app crashes without `destroy_overlay_bars()` cleanup.
