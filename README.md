# Mimic — Game Agent Monitor

Desktop monitor for visual game AI — **pixels in, actions out**.

> **Repo moved (2026-07)**: canonical home is now
> [gitee.com/Andyqwe44/mimic](https://gitee.com/Andyqwe44/mimic)
> (mirror: [github.com/Andyqwe44/Mimic](https://github.com/Andyqwe44/Mimic)).
> The old `tictactoe` repo is frozen at v0.3.31 — a migration release whose
> binaries point here, so pre-0.3.31 installs auto-update across.

## Architecture

**Thin agent client (2026-07 turn):** PC host = capture + execute. Vision / human control is external.

```
┌─ monitor_app (Windows thin agent) ──────────────────────────┐
│  Capture (WGC GPU) → HW H.264 → outbound WS to server       │
│  Gates: 发送画面 / 接受控制   ·  atomic send_input              │
│  Local UI: select target + connect server + gates            │
└──────────┬───────────────────────────────────────────────────┘
           │ outbound WebSocket
           ▼
┌─ controller_server.exe (deployable) ────────────────────────┐
│  HTTP www/ + WS relay   ·  browser ↔ agent                  │
└──────────┬───────────────────────────────────────────────────┘
           │
    ┌──────┴──────────────────────────────┐
    ▼                                     ▼
 controller_web (browser)              (future: desktop controller)
 settings: capture / H.264 / input
```

| Setting (server web) | Agent applies |
|----------------------|---------------|
| Capture WGC | Stream method |
| Input seize / postmsg | `sendinput` / `postmessage` |

**Zero Rust. Single MSVC command builds everything.**

Agent context: Cursor loads `.cursor/rules/*.mdc`. Long-form: `CLAUDE.md` / `AGENTS.md`.

| Language | Role |
|----------|------|
| C++17 | Thin agent + standalone `controller_server` relay |
| TypeScript/React | Agent UI (`monitor_web`); controller UI (`controller_web`) |
| Python | Vision model (optional TCP `:9999`) |

## Peer / MimicServer (UU-style)

Same **MimicClient** on each PC; cloud runs **MimicServer** (signaling only).

**Update shelf (CDN):** `http://47.107.43.5/mimic/client/` — binaries are **not** in git.
Gitee Release only ships two thin Setups that download `payload.zip` at install time.

```powershell
# Signaling (dev)
cd signaling_server; npm install; npm start   # :8443  demo/demo

# Full release: build → CDN sync → thin Setups → Gitee (2 exes)
powershell -File scripts\Release.ps1 -DryRun
```

| Package | Artifact |
|---------|----------|
| MimicClient | `MimicClient_Setup_v*.exe` (Gitee) → CDN `client/payload.zip` |
| MimicServer | `MimicServer_Setup_v*.exe` (Gitee) → CDN `server/payload.zip` |
| Incremental | `http://47.107.43.5/mimic/client/version.json` |

See [signaling_server/README.md](signaling_server/README.md) and [docs/auto-update.md](docs/auto-update.md).

## Quick Start

### Prerequisites
- Windows 10/11 with Visual Studio 2022 (C++ tools)
- Node.js 18+, WebView2 Runtime (pre-installed on Win11)

### Dev Mode

```powershell
# 1. Build C++ modules (first time / after C++ changes)
powershell -File scripts\Build.ps1 -Module logger,capture,input,controller_server,monitor_app -Dev

# 2. Start relay (separate machine or same PC)
.\controller_server\build\controller_server.exe          # http://0.0.0.0:9997

# 3. Agent UI (or double-click dev.bat)
cd monitor_web; npm run dev                              # Vite :1420
# monitor_app Dev exe → localhost:1420
# Connection panel: fill server IP:9997 → Connect → open stream/control gates

# 4. Browser on any PC: http://<server-ip>:9997
```

### Production

```powershell
# 3. Prod build (optimized, self-contained)
cd monitor_web; npm run build                    # Vite -> dist/
powershell -File scripts\Build.ps1 -Module monitor_app   # embeds dist -> build\bin\monitor_app.exe
# Navigates to https://gam.local/index.html (dist embedded in exe, served from memory)
```

> All build/release scripts are PowerShell under `scripts/` (`Build.ps1`, `Release.ps1`,
> `Verify.ps1`, `Publish.ps1`, `New-VersionJson.ps1`). One release command:
> `powershell -File scripts\Release.ps1`.

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
├── input/                C++ input forwarding (per-method .lib)
│   ├── include/          input_methods.h + input_common.h
│   ├── src/              input_common + sendinput/winapi/postmessage/driver
│   └── build/            output .lib files
├── monitor_app/          C++ WebView2 host (main window + commands + single-instance)
│   └── dep/              WebView2 SDK
├── scripts/              PowerShell build/release pipeline (Build/Release/Verify/Publish)
├── monitor_web/          Agent React UI (Vite + TypeScript + Tailwind)
├── controller_web/       Controller React UI (served by controller_server)
├── controller_server/    Standalone HTTP+WS relay (deployable)
├── protocol/             Wire format (C++/Python)
├── model/                Python AI
├── test_target/          Standalone input-test window (TCP :9998 self-test feedback)
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
| SharedBuffer | COM | Legacy/local BGRA preview (non-thin / debug) |
| TCP FRAM | 9999 | Optional local debug: H.264 out + JSON control in |
| controller_server WS | 9997 | Deployable relay: serves `controller_web`, agents connect outbound |

**Thin-client gates (Monitor tab):** connect to server first; **发送画面** pushes H.264; **接受控制** allows remote actions. Both gates default closed.

Capture / codec / input mode: configure on the **server web UI** (pushed to agent as config).

Remote Web UI: see [`controller_web/README.md`](controller_web/README.md). Relay: [`controller_server/README.md`](controller_server/README.md).

## Features

- **Dashboard** — System info, capture pipeline status, update check
- **Monitor** — Window/desktop capture, streaming preview, FPS counter
- **Log** — Live in-memory ring buffer + disk log tiles
- **Settings** — Connection, capture method (snapshot/stream/render), theme, **language** (en / zh-CN / zh-TW), model, log config
- **TopBar shortcuts** — locale dropdown (En/简/繁), permission toggle (User/Shield), theme toggle
- **i18n** — UI strings via i18next; locale + theme/accents/hotkey persisted in AppData `settings.json` (Prod: `%LOCALAPPDATA%\GameAgentMonitor\`; Dev build: `%LOCALAPPDATA%\GameAgentMonitor_Dev\`)
- **Virtual desktop** — Cross-desktop window enumeration, absolute numbering (Task View order via registry), desktop switching
- **Window picker** — EnumWindows with search, type filter (All/Desktop/Window), D1/D2 badges, ⚡ for remote-desktop windows
- **Single-frame** — WGC/GDI multi-method capture with PNG output
- **Dev mode** — DevTools UI demos use overlay layer only; leaving Dev mode clears overlays and refreshes agent status via get_agent_status (no blind rollback)
- **Input forwarding** — Click/drag/wheel/keyboard input injected into target window via 3 simulation layers (see below)

## Input Forwarding (send_input)

Monitor preview canvas operates like **remote desktop** (RDP/VNC): mouse movements
continuously forwarded at 60fps, clicks sent immediately, keyboard engaged on canvas focus.

Canvas mouse/keyboard events → coordinates normalized → `hostCall('send_input', {hwnd, type, ...})` →
C++ `cmd_send_input` executes the input against the target window.

### Interaction Model

| Action | Behavior |
|--------|----------|
| Mouse hover over preview | Cursor position forwarded to target (60fps `move` events) |
| Click | Immediate `click` (no defer); target responds instantly |
| Double-click | First click fires immediately, `dblclick` handled separately (second click suppressed) |
| Drag | Path sampled at 50ms, sent as `drag` with all points |
| Wheel | Scroll delta normalized to WHEEL_DELTA (±120/notch), sign-corrected |
| Keyboard | Canvas focus required (click to engage); Esc/blur auto-releases held keys |
| Right-click | Browser context menu suppressed, forwarded as `click` with `button: "right"` |

### Input Types

| Type | Description | Key Parameters |
|------|-------------|---------------|
| `click` | Single click | `x_norm, y_norm, button` (left/right/middle) |
| `dblclick` | Double click | `x_norm, y_norm, button` |
| `move` | Mouse move | `x_norm, y_norm` |
| `drag` | Click-drag-release | `button, path: [{x,y},...]` (sampled at 50ms) |
| `wheel` | Scroll wheel | `x_norm, y_norm, delta` (±120/notch, sign-corrected) |
| `keydown` | Key press | `key, code, vk` (virtual key code) |
| `keyup` | Key release | `key, code, vk` |
| `keypress` | Key down+up | `key, code, vk` |
| `combo` | Modifier+key | `ctrlKey, shiftKey, altKey, metaKey, key, vk` |
| `text` | Unicode string | `text` (UTF-8, sent char-by-char via `KEYEVENTF_UNICODE`) |

Keyboard uses individual `keydown`/`keyup` events — the system naturally recognizes
combinations (Ctrl+C) because modifier keys are held down from prior keydown events.
No manual combo synthesis needed for user input.

### Simulation Methods (4 layers)

| Method | Layer | Mechanism | UIPI Bypass | Status |
|--------|-------|-----------|-------------|--------|
| `sendinput` | 应用层 | `SendInput` API — synthesized system input, same path as hardware | ❌ | ✅ 推荐 |
| `winapi` | OS层 | `AttachThreadInput` + `SetForegroundWindow` + `SendMessage` synchronous | 部分 | ✅ 进阶 |
| `postmessage` | 窗口消息层 | `PostMessage` — direct window queue, asynchronous | 部分 | ✅ 备选 |
| `driver` | 驱动层 | Interception / virtual HID kernel driver | ✅ | 🔒 未实现 |

### Coordinate Pipeline

```
Browser click (px) → getImageCoords (letterbox-aware normalize 0-1)
  → hostCall('send_input', {x_norm, y_norm, ...})
    → cmd_send_input
      ├─ sendinput:     norm_to_screen → 0-65535 absolute → SendInput
      ├─ winapi:        norm_to_client → client px → SendMessage
      └─ postmessage:   norm_to_client → client px → PostMessage
```

### Keyboard Event Flow

```
User presses Ctrl+C in preview canvas (focused):
  keydown Ctrl  → vk=17 → SendInput KEYDOWN VK_CONTROL
  keydown c     → vk=67 → SendInput KEYDOWN 'C'        (system sees Ctrl+C)
  keyup   c     → vk=67 → SendInput KEYUP   'C'
  keyup   Ctrl  → vk=17 → SendInput KEYUP   VK_CONTROL

On blur/Escape: all pressed keys auto-released via keyup events.
```

## Self-Test — mapping calibration (Dev)

`test_target/test_target.exe` — standalone 5×5 grid (shrunk inner hit-zone) plus a
real multiline IME text box. It reports every received click back to GAM over TCP
(loopback **:9998**, JSON-lines), so a mapping can be validated against ground truth.

One-click **Self-Test** (Settings → Dev mode → Developer Mode card) drives the *real*
user path end-to-end and compares expected vs actual landings:

1. launch/find test_target + connect TCP — *only genuinely new logic*
2. select it as capture target — reuses the window-select callback
3. Monitor → Preview → mapping ON — reuses the preview + mapping toggles
4. dense sweep — per-cell N×N clicks via the same `sendMappedClick` a user fires

test_target reports `{x,y,gx,gy,hit}` per click; GAM predicts the expected cell/hit
from the handshake geometry and computes per-cell match rate, systematic offset
vector, and pixel error → heatmap report. Reveals constant offset, scale error, axis
flip, DPI mismatch.

Wire (loopback :9998, JSON-lines):
```
hello: {type,"hello", client_w, client_h, grid, cell, pad, hit_margin}   # on connect
click: {type,"click", seq, btn, x, y, gx, gy, hit}                       # per button-down
```
Commands: `find_test_target` → `{hwnd}`, `selftest_connect {port}`, `selftest_disconnect`.

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
