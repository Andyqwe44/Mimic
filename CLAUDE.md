# CLAUDE.md — TicTacToe → General Visual Game AI

## Project Vision

Build a self-organizing hierarchical visual game AI. Model interface: **pixels in, actions out**.
C++ for real-time Windows operations (capture, input, window enumeration).
Tauri 2 + React + Tailwind CSS for monitor GUI (MaaEnd/MXU style).
Python for AI model training/inference.

## Architecture

```
C++ Agent (performance-critical)
  ├── capture/       DXGI/GDI screen capture, window enumeration
  ├── input/         Interception driver / SendInput simulation
  └── agent/         pixels→actions agent loop

Rust (Tauri glue)
  └── monitor_web/src-tauri/   IPC bridge: Rust ↔ React, calls C++ subprocesses

React (GUI, monitor only — not in the work loop)
  └── monitor_web/src/         App.tsx + index.css
```

## Key Design Decisions

- **GUI is monitor-only**: C++ does actual capture+AI work. Rust reads data from C++ and displays in React.
- **C++ via subprocess**: Rust calls C++ .exe tools (window_list.exe, capture) via `std::process::Command`
- **No Slint**: tried Slint v1.17, too restrictive. Switched to Tauri+React.
- **Release builds**: `npm run tauri build` bundles frontend into exe, no localhost needed.
- **Tooltip system**: Custom React component, portal to body, 300ms delay, auto-flip, smart positioning.
- **IconBtn/ActionBtn**: `title: string` required, TypeScript compile-time enforcement.

## Project Structure

```
tictactoe/
├── common/              # Shared C++: types.hpp, signals.hpp/cpp
├── game/                # TicTacToe TUI (arrow keys, ANSI, blinking cursor)
│   ├── src/             .cpp files
│   ├── include/         .hpp files  
│   ├── build/           .obj
│   ├── main.exe
│   └── build.cmd        MSVC build
├── capture/             # Screen capture + window enumeration
│   ├── src/             capture_dxgi.cpp, window_list.cpp, process_list.cpp
│   ├── include/         capture.hpp, preprocess.hpp
│   ├── build/           window_list.exe, process_list.exe, capture_test.exe
│   └── build.cmd
├── input/               # Input simulation (SendInput + Interception)
├── agent/               # Visual agent (pixels→actions)
├── monitor_web/         # Tauri 2 + React desktop app
│   ├── src/
│   │   ├── App.tsx      # Main UI (250+ lines, all components inline)
│   │   ├── index.css    # Tailwind + CSS variables (dark/light theme)
│   │   └── main.tsx     # React entry
│   ├── src-tauri/       # Rust backend
│   │   ├── Cargo.toml   # tauri, serde, chrono, miniz_oxide
│   │   ├── src/main.rs  # list_windows, list_processes, capture_single, capture_window, logging
│   │   └── tauri.conf.json  # 1200x780 window, devUrl:1420
│   ├── package.json     # react, tailwindcss, lucide-react, clsx, tailwind-merge
│   └── vite.config.ts   # Vite + Tailwind + Tauri env
├── model/               # Python: generic_agent.py, hierarchical.py
├── ai/                  # Python AI server (MLP, TCP text protocol)
├── train/               # Training data collector
└── README.md
```

## Build Commands

```bash
# C++ modules (MSVC 2022, C:\Program Files\Microsoft Visual Studio\18\)
cd game     && build.cmd    # main.exe
cd capture  && build.cmd    # window_list.exe + process_list.exe + capture_single.exe + capture_stream.exe + capture_test.exe
# capture_stream.exe needs: d3d11.lib dxgi.lib dwmapi.lib windowsapp.lib
cd input    && build.cmd    # input_test.exe
cd agent    && build.cmd    # agent.exe

# Tauri monitor (needs Node.js + Rust)
cd monitor_web
npm install
npm run tauri dev     # dev mode (Vite HMR via port 1420)
npm run tauri build   # release .exe (bundled frontend, no localhost)

# Rust only (no frontend rebuild)
cargo build --manifest-path monitor_web/src-tauri/Cargo.toml --release

# Python
pip install torch onnx onnxruntime numpy opencv-python
cd ai && python train.py --iters 50 --games 100
```

## Tauri Dev HMR (Hot Module Replacement)

`npm run tauri dev` 启动流程：
1. Vite dev server → `http://localhost:1420` (HMR websocket)
2. Rust cargo run → 打开 WebView2 窗口，加载 Vite 地址
3. 编辑 `.tsx`/`.css` → Vite 检测变化 → 增量编译 → 推送 WebView → 即时刷新

**前端代码（React/TSX/CSS）编辑后无需重启 Rust，即时生效。**
仅改 Rust 代码时需重新 `cargo run`（Tauri 会 watch `src-tauri/` 自动重编译）。

Release 模式下 HMR 不可用 — 前端打包嵌入 exe，无网络请求。

## Release EXE Location
`monitor_web/src-tauri/target/release/game-agent-monitor.exe`
(Bundled HTML/CSS/JS, no network needed)

## Debug Log
Each launch creates: `agent_YYYYMMDD_HHMMSS.log` next to the exe.
Max 5 log files kept. `dlog!()` macro in Rust logs before every operation with flush().

## Slint v1.17 Incompatibilities Discovered
- `padding: a b;` (multi-value) NOT supported
- `alignment: center;` on Layout NOT supported
- `vertical-alignment:` on Rectangle NOT supported  
- `@children` can only appear once at component top level
- `em` units NOT supported (use px)
- `drop-shadow-*`, `focus-ring-*` NOT supported
- `horizontal-stretch:`, `overflow:`, `opacity:` NOT supported
- `animate` blocks NOT supported
- `:=` for components deprecated
- `for` loop `idx` binding broken
- `BOOL` type not in windows 0.60 crate
- `background` on `Text` element NOT supported
- `color` on `LineEdit` NOT supported
- `placeholder-color` on `LineEdit` NOT supported
- `text.length`, `substring()`, `to-upper-case()` NOT supported
- `max()` NOT supported
- `%` width only for `width`/`height` properties, not custom properties
- `float()` conversion NOT supported

## Frontend Component Architecture (App.tsx)

All components in one file (App.tsx, ~500 lines):
- `Tooltip` — portal to body, 300ms delay, smart positioning, z-index 9999
- `IconBtn` — icon button, `title: string` REQUIRED
- `ActionBtn` — labeled button (primary/danger/outline), `title: string` REQUIRED, `min-w-[120px]`
- `ThemeBtn` — light/dark toggle
- `TopBar` — tabs (Monitor/Log) + Start/Stop + Theme + Settings gear icon
- `BottomBar` — status bar (Running/Idle, FPS, Lat, GitHub link)
- `WindowPickerModal` — categorized window selector with search + filter tabs
- `ConnectionPanel` — window title input + Select button
- `ScreenshotPanel` — Camera (single frame) + Preview button (20fps TODO)
- `LogPanel` — real-time operation log
- `SettingsPage` — Connection, Theme(6 colors), Model Context, Update, Log config, Star, Credits
- `WindowInfo` interface: `{ title, category, hwnd }`

## C++ Window Tools

### window_list.exe
- Enumerates taskbar-visible windows only (DwmGetWindowAttribute + style checks)
- JSON output: `{"hwnd":"...", "category":"desktop|window", "title":"..."}`
- Fast, small payload — loaded on every modal open

### process_list.exe
- Lists ALL visible windows (including background processes)
- Used on demand when user clicks "Process" filter tab
- JSON output: `{"hwnd":"...", "category":"process", "title":"..."}`
- No desktop entry — desktop handled by window_list.exe

### capture_single.exe
- Single-frame screenshot, raw BGRA pixels via binary stdout
- Usage: `capture_single.exe <hwnd>` (0=desktop, other=window)
- Desktop: DXGI (skip virtual displays) → GDI fallback if solid
- Window: PrintWindow(PW_RENDERFULLCONTENT|PW_CLIENTONLY) → DXGI crop → GDI
- Binary format: `[w:4][h:4][ch:4][BGRA pixels...]` (little-endian)
- Rust reads binary, does BGRA→RGBA, scale, PNG encode, base64 → frontend

### capture_stream.exe
- Persistent capture process, frame-differenced stream
- Usage: `capture_stream.exe <hwnd>` (0=desktop, other=window)
- Window: FramePool (GPU via Windows.Graphics.Capture) → PrintWindow fallback
- Desktop: GDI direct (DXGI blocked by virtual display)
- C++ scales to max 640px before output (9x data reduction)
- Protocol: first line = method name (text), then `[w:4][h:4][ch:4][size:4][BGRA pixels]`
- size=0 = unchanged frame (Rust reuses previous)
- Stdin: "q\n" → quit
- Performance: C++ side 33fps (GDI desktop)

## Current State & Next Steps

1. **DONE**: TicTacToe TUI game, C++ capture, C++ window enumeration, Tauri GUI
2. **DONE**: Tooltip system, Settings page, Config merged into Settings
3. **DONE**: Capture single frame via C++ capture_single.exe (PrintWindow/DXGI/GDI)
4. **DONE**: Split window_list/process_list, Process filter tab + refresh button
5. **DONE**: Preview stream via capture_stream.exe (C++ persistent process → pipe → Rust → BMP → frontend)
6. **DONE**: FramePool (Windows.Graphics.Capture) for window capture, PrintWindow fallback
7. **DONE**: Binary IPC: BMP data URI (zero-encode, ~0.5ms) replaces PNG encoding (~3ms)
8. **DONE**: Self-window disabled in picker (opacity-40, cursor-not-allowed)
9. **DONE**: IP::port split inputs with `::` auto-parse
10. **DONE**: ActionBtn auto-width: ≤10 chars = w-20, >10 = min-w-[120px]
11. **DONE**: Connect GUI Log with C++ stderr → Rust dlog forwarding
12. **TODO**: C++ Agent directly communicates with AI model (not via GUI)
13. **TODO**: Media Foundation H.264 GPU encoding for Preview (plan Step 3)
14. **TODO**: Auto-update mechanism

## Key Performance Numbers (2026-07-05)

| Metric | Value | Method |
|--------|-------|--------|
| C++ desktop capture | 33 fps | GDI (DXGI blocked by virtual display adapter) |
| C++ window capture | varies | FramePool (GPU, 7fps on static frames) / PrintWindow (CPU, ~15ms) |
| C++ scale 1920→640px | ~2ms | nearest-neighbor |
| Rust BMP encode | ~0.1ms | trivial header, no compression |
| Rust base64 encode | ~0.5ms | 900KB → 1.2MB |
| Preview (C++ pipe only) | ~33fps desktop | GDI + scale + fwrite |
| Preview (end-to-end) | ~15-25fps | +BMP base64 + Tauri IPC + img render |

## Known Issues

1. **DXGI desktop returns solid black** — GameViewer Virtual Display Adapter is output[0], 
   our code now skips virtual/small outputs in dxgi_cap, but only GDI works reliably for desktop.
2. **FramePool 0 frames on static windows** — DWM doesn't re-composite static content.
   Empty frame fallback sends size=0, Rust reuses previous frame (frontend shows last good image).
3. **PrintWindow solid content detection** — uses sampling to detect black/magenta sentinel.
   Falls back to DXGI crop on solid detection.
4. **Tauri IPC bottleneck** — BMP data URI is ~1.5MB per frame. Base64 + JSON serialization
   takes ~5ms per frame. Next step: raw binary IPC to skip base64+JSON entirely.

## GPU Hardware Encoding Plan (from C:\Users\Andyq\.claude\plans\glittery-drifting-seal.md)

### Step 1: Binary direct transfer (done 2026-07-05)
- BMP data URI replaces PNG, saves 3ms encode
- stream-tick event for lightweight signal, stream_poll() for frame data

### Step 2: JPEG hardware encoding (short term)
- C++ WIC JPEG encode GPU-accelerated
- JPEG 640×360 ≈ 15KB (vs BMP 900KB)

### Step 3: H.264 video stream (medium term)
- Media Foundation MFT hardware encoder
- GPU capture → GPU encode → MSE <video> decode
- Expected: 1-2ms per frame, 60fps+

### Step 4: Agent direct path (future)
- Agent.exe: DXGI → GPU texture → ONNX inference → action
- Monitor GUI: reads Agent state via IPC, low frequency (100ms)

## Stream Protocol

**C++ capture_stream.exe → Rust stdout pipe**:
- Line 1 (text): capture method name (e.g., "GDI", "FramePool", "PrintWindow", "DXGI")
- Each frame: `[w:4 LE][h:4 LE][ch:4 LE][size:4 LE]` then `size` bytes of BGRA pixels
- `size=0` → unchanged frame (Rust reuses previous)
- Stdin: "q\n" → clean exit

**Rust → Frontend**: 
- `capture_stream_start(hwnd)` → spawns C++ process + reader thread
- Reader builds BMP in-memory (BGRA→BGR, trivial header), base64 encodes
- Emits "stream-tick" event (lightweight signal, no pixel data)
- `stream_poll()` command returns BMP data URI string (`data:image/bmp;base64,...`)
- `capture_stream_stop()` → sends "q\n" to stdin, waits for process exit

## Key Gotchas
- Rust release build: `current_dir()` is `src-tauri/`, NOT `monitor_web/`. Use `current_exe()`.
- Old exe must be killed before `cargo build --release` or get "access denied"
- `tauri dev` sometimes exits 127 (WebView2 conflict) — launch exe directly instead
- Test window_list.exe standalone: `capture/build/window_list.exe` prints JSON to stdout
- Frontend `npm run build` must succeed before Tauri build for bundled frontend
