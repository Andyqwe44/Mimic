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

当用户说"存档"时，执行以下三件事：

1. **更新 README.md** — 如果对外接口/用法变了
2. **更新 CLAUDE.md** — 如果架构/结构/构建流程变了
3. **git commit** — 写出清晰的 commit message，描述做了什么和为什么

这三个动作是一体的，缺一不可。

### 铁律 4: Tooltip 只用自定义组件

**项目已有自定义 Tooltip 组件（`App.tsx` 顶部）。严禁使用原生 HTML `title` 属性。**

原生 `title` 问题：外观不可控、延迟不一致、在鼠标右下角弹出（常被遮挡）。

自定义 Tooltip 特性：
- 300ms 统一延迟
- Portal 到 `document.body`（不受父容器 overflow/clip 影响）
- 智能定位：上方优先，空间不足自动翻到下方，水平 clamp 防溢出
- 统一外观：深色背景、白字、圆角阴影

**唯一合法方式**：
```tsx
<Tooltip text="提示文字">
  <button ...>...</button>   {/* 单个 ReactElement，不支持多个 children */}
</Tooltip>
```

以下写法**禁止**出现在任何 TSX 文件中：
```tsx
<span title="xxx">        // ← 原生 title，禁止
<button title="xxx">      // ← 原生 title，禁止
<div title="xxx">         // ← 原生 title，禁止
```

### 铁律 5: 禁止欺骗 — 后端不骗前端，前端不骗用户

**C++ 层不得对前端透明地修改行为。** 前端发送的命令必须被原样执行——成功返回数据，失败返回错误。禁止在 C++ 内部做静默 fallback、参数改写、或结果替换。

此规则有三个层面：

#### 5a. C++ 不得静默修改参数或 Fallback

违反示例：
```cpp
// ❌ 静默回退 — 前端不知道实际用了 DesktopBlt
if (method == "WGC" && hwnd == 0) {
    size = capture_desktop_bitblt(...);  // 前端以为用了 WGC
    used = "DesktopBlt";
}
```

正确做法：前端根据目标自行选择正确方法，C++ 只执行。
```tsx
// ✅ 前端知道自己在做什么
const method = hwnd === 0 ? 'dxgi' : forceMethod
```

#### 5b. C++ 必须检查 API 返回值，失败必须返回 Error

以下 Win32 API 调用必须检查返回值，失败时返回 error，**严禁**静默返回 `{"ok":true}`：

| API | 检查 | 失败含义 |
|-----|------|---------|
| `SendInput` | `sent != count` | UIPI 阻止、权限不足 |
| `PostMessage` | `!= FALSE` | 目标窗口已销毁 |
| `GetClientRect` | `!= FALSE` | 窗口句柄失效 |
| `AttachThreadInput` | `!= FALSE` | 目标线程无消息队列 |
| `MapVirtualKey` | scan code 非零 | 未知键码 |

```cpp
// ❌ 欺骗 — API 失败却返回 ok
SendInput(2, inputs, sizeof(INPUT));  // 返回值未检查
return "{\"ok\":true}";               // 前端以为成功了

// ✅ 铁律 5b — 诚实回报
UINT sent = SendInput(2, inputs, sizeof(INPUT));
if (sent != 2) {
    return "{\"ok\":false,\"error\":\"SendInput failed (UIPI blocked)\"}";
}
```

#### 5c. 前端不得欺骗用户 — 反馈必须匹配实际行为

| 规则 | 违反示例 | 正确做法 |
|------|---------|---------|
| 视觉反馈必须在操作**实际发送后**才显示 | Defer 300ms 的 click 先画了 ripple → 若 unmount 则静默丢失 | Defer 时先不画 ripple，实际发送时才画；或存储 timeoutId 在 unmount 时 flush |
| 键盘输入必须用**同一策略**，不能混用 | Ctrl 单独发 `keydown` + C 发 `combo` → 目标收到破损序列 | 全部用个体 `keydown`/`keyup`，系统自然识别组合键 |
| Cleanup 操作必须 Log | blur 时静默发 click 释放拖拽按键，用户不知 | 加 `addLog("[Input] drag cancelled — auto-released button")` |
| 坐标映射失败必须返回 Error | `GetClientRect` 失败后静默发到 (0,0) | 返回 `"error":"failed to get client rect"` |

**简洁记忆：**
- **C++ → TS：** 返回值 = `{"ok":false, "error":"..."}` 比静默返回 `{"ok":true}` 好一万倍
- **TS → 用户：** 画了什么 = 实际发了什么。没发的别画，发了的别藏。

此规则适用于所有跨层接口：C++↔TS、C++↔Python TCP。每层对自己的决策负责，下层不替上层做决定。

### 铁律 6: 前端交互优化 = 状态转换表 → 确认 → 改码

当用户说"前端交互方案"、"交互优化"、"总结前端状态"、"前端状态机"、"给个方案"等类似词语时，执行以下流程：

1. **先分析** — 阅读当前前端交互逻辑，理解现有状态机
2. **给状态转换表** — 用表格列出所有状态转换（当前状态 + 事件 → 新状态 + 原因）
3. **等待确认** — 用户说"确认"或"开始"后才动手改代码
4. **改代码** — 精确修改，不改表外逻辑

表格格式：
```
| # | 当前状态 | 事件 | 新状态 | 原因 |
```

禁止直接改代码。先给方案，等确认。

## Project Vision

Build self-organizing hierarchical visual game AI. Model interface: **pixels in, actions out**.
C++ for all real-time work: capture + WebView2 GUI + TCP + logging.
Python for AI model training/inference.
v0.3.0 — pure C++ WebView2 host, zero Rust.

## Architecture (post-migration: pure C++ WebView2 host)

```
┌─ monitor_app (C++ Win32) ────────────────────────────────────────────┐
│  React (TypeScript + Tailwind)  ←→  C++ backend (same process)      │
│       MXU-style UI               │  WebView2 COM 原生                 │
│       Dashboard/Monitor/Log       │  WebMessage bridge (ex-Tauri IPC) │
│                                   │  SharedBuffer 直推 (零 FFI)       │
│  Dev:  WebView2 → localhost:1420 │  SharedBuffer 零拷贝            │
│  Prod: WebView2 → gam.local      │  BGRA→RGBA 直推                 │
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
├── input/                         # C++ input forwarding (per-method static libs — mirrors capture/)
│   ├── include/
│   │   ├── input_methods.h        InputArgs struct + 4 method signatures
│   │   └── input_common.h         Shared helpers (vk/scan/coords/parse_drag_path)
│   ├── src/
│   │   ├── input_common.cpp       JSON→InputArgs parser + all shared helpers
│   │   ├── input_sendinput.cpp    SendInput method (应用层)
│   │   ├── input_winapi.cpp       WinAPI method (OS层: AttachThreadInput+SendMessage)
│   │   ├── input_postmessage.cpp  PostMessage method (窗口消息层)
│   │   └── input_driver.cpp       Driver placeholder (驱动层)
│   └── build_input_lib.cmd        MSVC → input_common + sendinput/winapi/postmessage/driver .lib
├── monitor_app/                  # C++ WebView2 host (window + commands + MJPEG + TCP)
│   ├── src/
│   │   ├── main.cpp              Win32 window + WebView2 + message loop
│   │   ├── commands.h/cpp        Command dispatch (list_windows, capture, log, stream)
│   │   ├── mjpeg_server.h/cpp    MJPEG HTTP server (Winsock2 + WIC)
│   │   ├── json_helper.h         Minimal JSON parser for WebMessage
│   │   ├── version.h             Single canonical APP_VERSION (+ APP_VERSION_RC for VERSIONINFO)
│   │   ├── virtual_desktop.h/cpp Virtual desktop enumeration + switch (undocumented COM)
│   │   └── embedded_assets.h     GENERATED: dist/** as byte arrays (gitignored, prod only)
│   ├── tools/
│   │   ├── gen_assets.mjs        Node: monitor_web/dist/** → src/embedded_assets.h
│   │   └── gen_icon.py           One-time: favicon.svg → app.ico (svglib, PIL fallback)
│   ├── app.rc                    Icon (IDI_APPICON) + VERSIONINFO (prod exe resources)
│   ├── app.ico                   Committed exe/taskbar icon (PIL placeholder — see assets/icon/)
│   ├── assets/icon/              Icon design workflow (reserved; design deferred)
│   │   └── ICON_PROMPT.md        ChatGPT/DALL·E prompt + drop icon_source.png here → regen
│   ├── dep/                      WebView2 SDK (header + static lib)
│   │   ├── WebView2.h
│   │   ├── WebView2EnvironmentOptions.h
│   │   └── WebView2LoaderStatic.lib
│   ├── build.cmd                 MSVC → build\monitor_app.exe (prod, self-contained: dist embedded + rc)
│   └── build_dev.cmd             MSVC → build_dev\monitor_app.exe (dev)
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

Dev/prod mode set at build time via `/DDEV_MODE` preprocessor define. No runtime `--dev` flag.

```bash
# 1. Build C++ static libs (once, or when C++ changes)
cd logger   && build_logger_lib.cmd
cd capture  && build_capture_lib.cmd
cd input    && build_input_lib.cmd

# 2a. Dev build (Vite HMR, debug symbols, no optimization)
cd monitor_web && npm run dev        # Vite on :1420 (keep running)
cd monitor_app && build_dev.cmd      # → build_dev\monitor_app.exe
# Launch: build_dev\monitor_app.exe  → http://localhost:1420

# 2b. Prod build (optimized, no debug)
cd monitor_web && npm run build      # Vite → dist/
cd monitor_app && build.cmd          # → build\monitor_app.exe
# Launch: build\monitor_app.exe      → https://gam.local/index.html (dist embedded in exe, served from memory)
```

| | Dev (`build_dev.cmd`) | Prod (`build.cmd`) |
|---|---|---|
| Optimize | `/Od` | `/O2 /Gy /Gw /GS-` |
| Debug info | `/Zi /DEBUG:FULL` | None |
| Linker | None | `/OPT:REF /OPT:ICF` |
| CRT | `/MT` | `/MT` |
| Macro | `DEV_MODE` | `NDEBUG` |
| Binary | ~2.4 MB | ~451 KB |

### Prod asset serving (self-contained exe)

Prod embeds the built frontend into the exe — no external `dist/` folder, no HTTP
port. `build.cmd` runs `node tools/gen_assets.mjs` to compile `monitor_web/dist/**`
into `src/embedded_assets.h` (byte arrays + `g_embedded_assets[]` table), then
`rc.exe` compiles `app.rc` (icon + VERSIONINFO) into `build/app.res`.

At runtime (prod only, `#ifndef DEV_MODE`), `main.cpp`'s `WebResourceRequestedHandler`
intercepts every `https://gam.local/*` request via `AddWebResourceRequestedFilter` +
`add_WebResourceRequested`, looks the path up in `g_embedded_assets`, and answers from
memory (`SHCreateMemStream` → `CreateWebResourceResponse`). `"/"` maps to `/index.html`.
Replaces the old `SetVirtualHostNameToFolderMapping` (which needed dist/ on disk).

Result: shipping = copy the single `build\monitor_app.exe`. Only external prerequisite
is the WebView2 Runtime (system-level, Win11 built-in). Dev is unchanged (Vite :1420);
`embedded_assets.h` is `#ifndef DEV_MODE`-excluded, so dev never regenerates it.

### Single-instance guard

Named mutex in `WinMain`, **dev/prod split** (each build is single-instance within
its own kind; the two can coexist on one machine):

| Build | Mutex | Window class | Title |
|-------|-------|--------------|-------|
| Prod  | `Global\GameAgentMonitor_8A3F2D`     | `GameAgentMonitor`     | `Game Agent Monitor`       |
| Dev   | `Global\GameAgentMonitor_8A3F2D_Dev` | `GameAgentMonitor_Dev` | `Game Agent Monitor (Dev)` |

If another instance is running, activates its window (restore + foreground) and exits.

**Exit code = the only signal (no log, no console).** The guard runs BEFORE
`capture_log_init` on purpose — a short-lived second process must NOT spawn a new
log session file (would pollute the front-end's history list), and there's no
attached console. So it communicates purely via return code:

| Exit code | Meaning |
|-----------|---------|
| `2` | Instance already running — raised the existing window, then exited |
| other | Normal run (0 on clean exit; window stayed open until closed) |

**Launching the app from a terminal (Claude): check `$?`.** A GUI exe blocks bash
until it exits. Probe first, then decide:

```bash
build_dev/monitor_app.exe; echo "exit=$?"
# exit=2  → already running, existing window was raised (do NOT relaunch)
# blocks  → fresh instance is alive; Ctrl-C and relaunch with run_in_background
```

Practical flow: run once to read `$?`. If `2`, it's already up — nothing to do.
Otherwise it's a fresh launch that stays open → run it backgrounded instead.

### Developer Mode

Enable via Settings → General → Dev mode toggle. Shows "Developer Mode" tile with:
- **Save single-frame captures**: saves each 📷 snapshot as PNG to chosen directory
- **Save live preview frames**: saves each ▶ preview frame as PNG to chosen directory
- **Dump dir**: folder picker + open folder buttons

C++ commands: `set_frame_dump {capture, stream, dir}` / `pick_dir` / `open_dir {dir}`.
Frames saved as `snap_YYYYMMDD_HHMMSS_ms.png` or `stream_YYYYMMDD_HHMMSS_ms.png`.

# 4. Prod mode
cd monitor_web && npm run build      # Vite → dist/
cd monitor_app && build.cmd          # embeds dist + compiles rc → self-contained build\monitor_app.exe
build\monitor_app.exe                # → https://gam.local (dist served from memory, no external files)
```

## Internal Architecture (C++ host)

### Communication: WebMessage bridge (replaces Tauri invoke)

```
JS:  hostCall('list_windows') → chrome.webview.postMessage('{"cmd":"list_windows","id":1,"args":{}}')
C++: WebMessageReceived → HandleWebMessage → dispatch_command
       → wraps result as {"id":1,"result":{...}} → PostWebMessageAsJson
JS:  'message' event → e.data is pre-parsed object → match by msg.id → hostCall auto-unwraps .result
```

Key: `hostCall` internally extracts `.result` from the `{id, result}` envelope, so callers receive the raw command result directly.

### Command dispatch (commands.cpp)

| Command | Args | Returns |
|---------|------|---------|
| `list_windows` | — | `[{title, category, hwnd, desktop}, ...]` (绝对编号 D1/D2=任务视图左右顺序, 注册表获取) |
| `list_processes` | — | `[{title, category:"process", hwnd:pid}, ...]` |
| `capture_window` | `{hwnd, method}` | `{ok, w, h, method}` — frame via SharedBuffer, no base64 |
| `capture_stream_start` | `{hwnd, method, transport}` | `{ok:true}` |
| `capture_stream_stop` | — | `{ok:true}` |
| `read_logs` | `{max_files}` | `{files:[{name, size}, ...]}` (不含当前 session) |
| `read_log_file` | `{filename}` | `{filename, content}` (按需加载历史文件内容) |
| `open_log_dir` | — | `{ok:true}` (ShellExecute 打开日志目录) |
| `clear_log` | — | `{ok:true}` |
| `log_ui_event` | `{event, detail}` | `{ok:true}` (→ `capture_log_write_ui`, no echo back) |
| `get_version` | — | `"0.3.0"` (canonical version, single source of truth) |
| `get_log_dir` | — | `{dir}` — absolute log directory path |
| `pick_log_dir` | — | `{dir}` — Windows folder picker, returns selected path |
| `read_live_log` | — | `{lines}` — ring buffer content (init sync only) |
| `benchmark_methods` | `{hwnd, method}` | `{results:[{method, time_ms, size, ok},...]}` |
| `list_desktops` | — | `[{name, index, current}, ...]` (undocumented COM) |
| `switch_desktop` | `{index}` | `{ok:true}` (switches entire desktop — user visible) |
| `set_frame_dump` | `{capture, stream, dir}` | `{ok:true}` |
| `pick_dir` | — | `{dir}` (Windows folder picker) |
| `open_dir` | `{dir}` | `{ok:true}` (ShellExecute open) |

### Method routing (铁律 5 enforced)

**Frontend decides method, C++ only executes.** No silent fallback, no parameter rewriting.

**Single-frame** (`call_capture`):
| Method | Backend | Notes |
|--------|---------|-------|
| `wgc` | `wgc_capture_single(hwnd)` | Requires valid HWND; hwnd=0 returns error |
| `wgc-monitor` | `wgc_capture_single_monitor(hmon)` | Desktop via primary monitor |
| `dxgi` / `desktopblt` | `capture_desktop_bitblt()` | GDI DesktopBlt, returns method="DesktopBlt" |
| `GDI(GetWindowDC)` | `capture_gdi_getwindowdc(hwnd)` | |
| `PrintWindow` | `capture_printwindow(hwnd)` | |
| `ScreenBitBlt` | `capture_screen_bitblt(hwnd)` | |
| `DesktopBlt` | `capture_desktop_bitblt()` | Canonical name |
| unknown | — | Returns error, no fallback chain |

**Streaming** (`capture_stream_start`):
| Method | Backend | Notes |
|--------|---------|-------|
| `wgc` | `wgc_stream_start(hwnd)` / `wgc_stream_start_monitor(hmon)` | hwnd=0 → monitor mode |
| `dxgi` | — | Returns error: "DXGI stream not implemented" |
| unknown | — | Returns error |

**Frontend decision logic** (`App.tsx:takeSnapshot`):
```tsx
const method = hwnd === 0 ? 'dxgi' : 'wgc'
// desktop → DesktopBlt (fast, reliable single-frame)
// window  → WGC (GPU capture)
```

### Logging architecture (event-driven, zero polling)

All logs (C++ LOG + TS addLog) flow through the same pipeline — ring buffer + log file.
Three views (right panel, Log tab, disk files) show identical content in real-time.

```
TS addLog(msg)
  ├─ immediate: LogManager.entries → React → GUI (0ms)
  └─ hostCall('log_ui_event') → capture_log_write_ui(msg)
       └─ ring buffer + file (no echo back to TS)

C++ LOG(tag, msg)
  ├─ ring buffer + file
  └─ on_log_notify(tag, msg) → PostWebMessage({type:'log',...})
       └─ TS 'message' event → logMgr.addRemote(ts, tag, msg) → GUI

Startup: hostCall('read_live_log') — one-time catch-up of entries before WebView2 was ready.
```

Key logger functions:
| Function | Purpose |
|----------|---------|
| `capture_log_write_msg(tag, msg)` | C++ LOG() — writes file + ring, triggers notify callback |
| `capture_log_write_ui(msg)` | TS log_ui_event — writes file + ring, NO notify (TS already knows) |
| `capture_log_set_notify(cb)` | Register callback for C++ → TS real-time push |
| `capture_log_get_dir()` | Return absolute log directory path (set at init) |

### Log lifecycle

Single log session from app start to close/crash. No truncation/split in UI.
`capture_log_init` at startup, `capture_log_shutdown` at exit.
`clear_log` C++ command preserved (archives + rotates) but not exposed in UI.
Ring buffer + file cleared on each new session.
History files accessible via Log tab; each tile has refresh + copy buttons.

### Streaming pipeline

```
WGC → condition_variable → TryGetNextFrame → CopyResource(GPU) → Map(CPU)
  → BGRA pixels
  → [stream thread, MTA] stream_bridge_push_frame → PostMessage(WM_STREAM_FRAME)
  → [main thread, STA] WndProc → shared_buffer_push_frame → PostSharedBufferToScript
  → [JS] sharedbufferreceived → ImageData → Canvas putImageData

Single-frame: capture_window → call_capture → shared_buffer_push_frame (main STA thread directly)
```

### Stream bridge (cross-thread SharedBuffer)

WebView2 interfaces (`ICoreWebView2Environment12`, `ICoreWebView2_17`) are STA-created.
Stream thread runs WGC on MTA (WinRT requirement). Direct cross-apartment COM calls fail
because WebView2 has no proxy/stub registered (CoMarshalInterThreadInterfaceInStream → 0x80040155).

**Bridge**: `stream_bridge_push_frame(bgra, w, h)` — stream thread copies pixels to mutex-guarded
global buffer, posts `WM_STREAM_FRAME` to main window. WndProc handler calls
`shared_buffer_push_frame` on the STA thread where the interfaces are valid.

### SharedBuffer (zero-copy, no FFI)

C++ native COM, BGRA→RGBA inline:
```cpp
ICoreWebView2Environment12* env12;
ComPtr<ICoreWebView2SharedBuffer> buf;
env12->CreateSharedBuffer(w * h * 4, &buf);
BYTE* dst;
buf->get_Buffer(&dst);  // COM method, not Open()
// BGRA→RGBA conversion inline
for (int i = 0; i < w * h; i++) { /* swap R↔B */ }
ICoreWebView2_17* wv17;
wv17->PostSharedBufferToScript(buf.Get(), COREWEBVIEW2_SHARED_BUFFER_ACCESS_READ_ONLY, meta);
buf->Close();  // AFTER Post — buffer must remain open when posted
```

JS handler:
```tsx
// e.additionalData is already parsed object (not JSON string)
const meta = typeof e.additionalData === 'string' ? JSON.parse(e.additionalData) : e.additionalData
const buf = e.getBuffer()
new ImageData(new Uint8ClampedArray(buf, 0, meta.w * meta.h * 4), meta.w, meta.h)
ctx.putImageData(imgData, 0, 0)
```

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
3. **Chromium background tab throttling**: WebView2 may throttle when app loses focus.
4. **WebView2 cross-thread COM**: `ICoreWebView2Environment12`/`ICoreWebView2_17` are STA-only;
   COM marshaling fails (no proxy/stub, 0x80040155). Stream uses PostMessage bridge to
   push SharedBuffer from main STA thread. Single-frame capture works directly on main thread.
5. **WGC same-window session conflict**: Starting WGC stream while previous session still
   alive crashes. TS state machine prevents this; C++ `cmd_capture_stream_start` auto-stop
   removed to surface TS bugs. `WgcStreamHandle::stop()` now `join()`s worker thread.

## Frontend Type System

**WebView2 host objects** (`window.chrome.webview`) have no `@types/*` npm package.
Local type declarations in `monitor_web/src/webview2.d.ts` cover:
- `SharedBufferReceivedEvent` — `getBuffer()`, `additionalData` (object|string), `source`
- `WebView2Host` — `postMessage()`, `addEventListener`/`removeEventListener` for `sharedbufferreceived` and `message`

Never use `any` for WebView2 event handlers — the `.d.ts` enables compile-time checking
of method names (e.g. `e.additionalData` not `e.getAdditionalData()`).

## Recent Fixes (2026-07-09)

### Two-color theme system + Dev Mode override + lightning-bolt swatches (major)
- 8 theme pairs (7 normal + 1 Dev), each with c1 (primary) and c2 (secondary) colors
- c1 → primary buttons (Start/Select/Snapshot), c2 → disconnect/warning via `bg-accent-dev`/`text-accent-dev`
- Lightning-bolt diagonal-cut swatch (20×20 + top/bottom indicator lines), SVG viewBox=px for pixel-aligned strokes
- Dev Mode ON → auto-switch to Dev pair (Red #EF4444 / Yellow #EAB308), force override all theme colors
- Dev Mode OFF → restore user's normal theme selection (stored in normalAccent/normalDevAccent)
- Dev swatch shown inline with `ml-3` gap, disabled when Dev Mode OFF
- All toggles/radios use Tailwind classes (`bg-accent`, `border-accent`) instead of inline styles — auto-follow CSS variable overrides
- Disconnect button changed from hardcoded amber to `accent-dev` colors
- `--color-accent-dev` registered in Tailwind `@theme` + CSS `:root`
- Color picker Tooltip labels: Ocean/Twilight/Lagoon/Sunset/Orchid/Mint/Nebula/Dev

### ActionBtn golden-ratio sizing + className audit + UI height unification (major)
- Button sizing: golden-ratio modular scale (×√φ≈1.272), 5 tiers: xs(64)/sm(80)/md(104)/lg(132)/xl(168), all h-7(28px)
- `size` prop optional — auto-detects from `label.length`; explicit override still supported
- Star button refactored from raw `<button>` to ActionBtn, label "Star"→"Star on GitHub"
- All inputs/selects unified to h-7 (28px): ConnectionPanel (title, disconnect, IP, port), SettingsView (model, adapter, log dir, keep files, dump dir), TargetPickerModal (search)
- className audit: zero silent override issues found across all 11 TSX files
- `break-all`→`break-words` in LogPanel for readable log line wrapping
- `monitor_web/CLAUDE.md` updated with full ActionBtn size table

### MonitorView remote-control mode — continuous input forwarding (major)
Monitor tab preview now works like remote desktop (RDP/VNC). Mouse movement
continuously forwarded at 60fps, clicks are immediate, keyboard engaged on canvas
focus.

| Interaction | Old | New |
|-------------|-----|-----|
| Mouse move | Only during drag (50ms sampling) | Continuous 60fps forwarding when mouse on canvas |
| Click | Deferred 300ms (dblclick suppression) | Immediate (dblclick suppresses second mouseup) |
| Hint | "点击捕捉焦点 → sendinput" | "悬停移动光标 · 点击控制" / "远程控制中 · Esc 释放" |
| Focus ring | Gray when unfocused | accent/40 when unfocused, hover accent/70 |

**Dblclick flow**: first mouseup sends `click` immediately, `onDoubleClick` sets
`dblclickSuppressRef=true` and sends `dblclick`, second mouseup reads the ref and
skips click. No defer, no duplicate clicks.

**Monitor toolbar** simplified: left = target title (fixed 144px, CSS `truncate` for
ellipsis) + state badge (Connection-style: `text-accent bg-accent/10`), middle =
`flex-1` spacer, right = Snapshot + Preview/Stop buttons (fixed widths: `w-[88px]`
+ `w-[76px]` to prevent layout shift on toggle).

**Removed from toolbar**: snapshot/stream method badges, separator, "Last: XXX" label.
Unused props prefixed with underscore: `_capMethod`, `_snapMethod`, `_streamMethod`,
`_snapshotLatency`.

### App icon — ChatGPT-generated source
`monitor_app/assets/icon/icon_source.png` (1024×1024 PNG from DALL·E).
Regenerated `app.ico` via PIL: `icon_source.png → app.ico` (6 sizes 16-256px).
Exe icon updates on next prod build.

### Component decomposition — 1798-line App.tsx → 11 modular files (major)
Split monolithic App.tsx into reusable components under `src/components/` and
shared lib under `src/lib/`:

| File | Exports |
|------|---------|
| `lib/bridge.ts` | `hostCall`, `LogManager`, `logMgr`, `addLog`, `applyTheme` |
| `lib/types.ts` | `WindowInfo`, `HistoryFile`, `LogEntry` |
| `lib/constants.ts` | `CAPTURE_METHODS`, `RENDER_METHODS`, `INPUT_METHODS`, `STATE_LABEL`, etc. |
| `components/Toolkit.tsx` | `Tooltip`, `ActionBtn`, `ThemeBtn` |
| `components/TopBar.tsx` | Tab bar + Start/Stop + ThemeBtn |
| `components/BottomBar.tsx` | Status strip (target, method, agent TCP, version) |
| `components/TargetPickerModal.tsx` | Window list + capture mode picker |
| `components/ConnectionPanel.tsx` | Target connection (blue accent) |
| `components/ScreenshotPanel.tsx` | SharedBuffer canvas (violet accent) + `bare` mode |
| `components/LogPanel.tsx` | Log viewer (amber accent) — compact + full modes |
| `components/SettingsView.tsx` | Settings page with SettingsCard + StatusBar |
| `components/MonitorView.tsx` | Main workspace: large preview + mouse input mapping |

Zero TS errors. Vite HMR unchanged. App.tsx now ~530 lines (pure orchestration).

### Input mapping — C++ send_input + frontend mouse forwarding (major)
**C++ `cmd_send_input`** (`commands.cpp`): three-tier input injection:
| Method | Implementation | Notes |
|--------|---------------|-------|
| `sendinput` | `SendInput` API, MOUSEEVENTF_ABSOLUTE (0-65535) | Recommended default |
| `postmessage` | `PostMessageW(WM_LBUTTONDOWN/UP)` directly to window | May bypass some protections |
| `driver` | — | Returns error, not implemented |

Coordinate flow: norm(0-1) → GetClientRect → client pixels → ClientToScreen(screen) → SendInput absolute / PostMessage LPARAM.
New `json_get_double` in `json_helper.h` for floating-point args.
**Frontend**: Settings → Capture → Input Method selector (🖱). MonitorView click handler calls
`hostCall('send_input', {hwnd, type, x_norm, y_norm, button, method})`.
Crosshair cursor + overlay hint when previewing non-desktop target.

### Monitor tab redesign + BottomBar status strip
Monitor tab now shows: toolbar (target + method badges + Snapshot/Preview buttons) +
large preview area (ScreenshotPanel bare mode) + mouse click-to-forward overlay.
BottomBar redesigned from `Idle | FPS:0 | Lat:0ms` to real status strip:
`🖥 window │ 📷 WGC ▶ WGC 60fps ● │ 1920×1080 │ TCP :9999 ● Agent在线 │ v0.3.0`
FPS/dims flow via `onFps`/`onDims` callbacks from ScreenshotPanel → App → BottomBar.

### Panel color differentiation + scrollbar thinning
Right-sidebar panels now have colored icon backgrounds:
Connection (blue-400/15), Screenshot (violet-400/15), Log (amber-400/15).
Scrollbar: 6px → 4px, added Firefox `scrollbar-width: thin`.

### Connection header: state/recommend badges moved to title
状态/推荐 labels removed; 桌面 + WGC badges moved next to Connection title on the left.
Old markup deleted (JSX `{/* */}` comment caused OXc parse error).

### Dark theme — VSCode-inspired deep blue-gray palette
Replaced harsh pure-black palette (`#09090b`/`#18181b`/`#27272a`) with VSCode Dark+
inspired colors (`#1e1e1e`/`#252526`/`#2d2d2d`). Reduced text contrast from pure white
(`#fafafa`) to VSCode default gray (`#cccccc`). Accent tint: `#1e3a5f` → `#264f78`
(VSCode selection blue). All colors in `index.css` `.dark` block, zero code changes.

### LogPanel lazy load — skip history file list in compact mode
Compact LogPanel (right sidebar) no longer calls `loadHistory` on mount. Only
the full Log tab loads history file names, and content is loaded on expand click.
Compact mode only shows ring buffer entries — history files are irrelevant there.
Removes unnecessary `read_logs` C++ call that was generating noise in the log.

### Pin lock + safe setter for right-sidebar panels (major)
Right-sidebar panels (Connection / Screenshot / Log) now each have a 📌 Pin button
on the header right side. Pin locks the panel at its current expanded/collapsed state
against ALL subsequent changes — manual toggle, auto-layout, and content-driven events
(📷 snapshot, ▶ stream, ⏹ stop) all go through a centralized safe setter that returns
`false` when pinned, making pin enforcement low-coupling (no scattered guard clauses).

**Architecture**: `xxxPinLocked` ref (`null | boolean`) + safe setter `setXxxExpanded(v): boolean`.
Pin toggle records `expandedRef.current` into the lock ref; safe setter checks lock ref
before calling React setState. All call sites (onToggle, takeSnapshot, startStream,
stopStream, auto-layout) use the safe setter — double-checking is harmless.

**Auto-layout priority** (after pin refactor):
| Direction | Priority |
|-----------|----------|
| Collapse (shrink) | Log → Connection → Screenshot(empty) → Screenshot(has content) |
| Expand (grow) | Screenshot(has content) → Connection → Screenshot(empty) → Log |

Pinned panels are skipped by auto-layout (`xxxPinLocked.current === null` check).
All-three-pinned overflow: do nothing, user's responsibility.

**Screenshot `ssHasContentRef`** tracks whether canvas has rendered content,
used by auto-layout to prioritize content-bearing Screenshot over empty placeholder.

### UI operation conflict resolution — TS-side state machine (major)
All capture operation conflicts resolved in TypeScript (铁律 5 推广: C++ 不替前端做决策).
**Principle**: last action wins — newer operation auto-cancels older one.

| Old state | New action | Behavior |
|-----------|-----------|----------|
| ▶ streaming | 📷 snapshot | Auto-stop stream → take snapshot |
| ▶ streaming | 🔄 change target | Auto-stop stream → switch target |
| ▶ streaming | 🔌 disconnect | Auto-stop stream → reset to desktop |
| 📷 snapshotting | ▶ start stream | Cancel snapshot → start stream |
| 📷 snapshotting | 📷 snapshot | Ignore (re-entry guard) |

**Architecture**: `ScreenshotPanel` → pure view. Operation state machine (`opStateRef` +
`snapCancelRef`) + `takeSnapshot`/`startStream`/`stopStream` live in `App`.
`ScreenshotPanel` receives refs (`previewingRef`, `snapshotRef`, `snapshotStartRef`)
as props for the SharedBuffer handler.

### WGC thread cleanup: detach → join
`WgcStreamHandle::stop()`: `detach()` → `join()` + `ShutdownQueueAsync()`.
Old code detached worker thread and immediately `delete h`, destroying `WgcCapture cap`
while worker was still using it (use-after-free). Old DispatcherQueue never shut down,
leaving stale WGC session that crashed next capture on same window.
Added defensive LOG before `cap.shutdown()` and `dq.ShutdownQueueAsync()`.

### C++ stream auto-stop removed
`cmd_capture_stream_start` no longer calls `cmd_capture_stream_stop()` internally.
TS handles conflict resolution exclusively. `cmd_clear_log` + `backend_shutdown`
safety nets preserved.

### STATE_LABEL desktop → 桌面 + method badge in Connection header
`STATE_LABEL['desktop']` changed from `'Desktop'` to `'桌面'` (matches 前台/后台/最小化).
Connection header method (推荐) now uses ScreenshotPanel badge style:
`text-[11px] font-medium text-accent bg-accent/10 px-1.5 py-0.5 rounded`.

### Split capture method: snapshot vs stream + render method selector
Settings → Capture card now has three independent selectors:
- **📷 Snapshot** + **▶ Stream** side-by-side with vertical separator, each with Auto toggle
- **🎨 Render Method**: SharedBuffer (current) / H.264 (planned) / H.265 (planned, grayed out)
- `forceMethod`/`autoMethod` split into `snapMethod`/`streamMethod` + `autoSnap`/`autoStream` states
- `renderMethod` state controls transport param in `capture_stream_start`
- Method selectors moved from ConnectionPanel to Capture SettingsCard

## Recent Fixes (2026-07-08)

### Method routing — 铁律 5 full enforcement (major)
C++ no longer makes method decisions. All silent mappings removed:
- `capture_stream_start`: respects `method` param (was hardcoded WGC)
- `call_capture`: `'dxgi'` + `'desktopblt'` both map to DesktopBlt, returns `method="DesktopBlt"` (was silent lie)
- `'wgc'` with hwnd=0 returns error (was silently switching to monitor — now use `'wgc-monitor'` explicitly)
- Unknown methods return error (was fallback chain)
- Frontend `takeSnapshot`: `hwnd === 0 ? 'dxgi' : 'wgc'` — frontend owns the decision
- Comprehensive LOG() coverage in `wgc_capture_single` and `wgc_capture_single_monitor`

### Screenshot header UI: method badge + latency/FPS
Header always shows capture method as accent badge (`[WGC]` / `[DXGI]`).
Single-frame 📷 shows end-to-end latency (button press → Canvas render) in ms.
Streaming ▶ shows FPS counter (unchanged).
`capMethod` no longer cleared on stream start.

### Screenshot panel SharedBuffer pipeline (major)
JS handler had 3 silent bugs all swallowed by `catch(_){}`:
1. `e.getAdditionalData()` is not a function → `e.additionalData` (COM `get_AdditionalData` → JS property)
2. `e.additionalData` is already a parsed object (WebView2 auto-parses JSON) → `typeof === 'string' ? JSON.parse : as-is`
3. `json_get_int` couldn't parse `true`/`false` booleans → added string comparison for JSON literals

### Stream bridge (PostMessage-based cross-thread SharedBuffer)
Stream thread runs MTA (WGC/WinRT requirement). WebView2 SharedBuffer interfaces are STA-only.
GIT and CoMarshalInterThreadInterfaceInStream both fail (0x80040155 — no COM proxy/stub).
Solution: `stream_bridge_push_frame()` copies pixels to mutex-guarded global buffer,
posts `WM_STREAM_FRAME` to main window. WndProc handler calls `shared_buffer_push_frame`
on the STA thread.

### Developer mode + frame dump
Settings → General → Dev mode toggle. Enables frame dump to disk as PNG:
- `set_frame_dump {capture, stream, dir}` — enable per-type + set directory
- `pick_dir` / `open_dir {dir}` — folder picker / open in Explorer
- Frames saved as `snap_YYYYMMDD_HHMMSS_ms.png` or `stream_YYYYMMDD_HHMMSS_ms.png`
- `bgra_to_png` now logs every failure step with HRESULT

### --console flag removed
AllocConsole/freopen removed — debug output only goes to log files (fflush on every write).

### WebView2 types (webview2.d.ts)
Local `.d.ts` declarations for `chrome.webview` host objects.
Catches method name errors at compile time (e.g. `getAdditionalData` vs `additionalData`).

### WebMessage bridge response wrapping
`HandleWebMessage` wraps every response as `{"id":N,"result":{...}}`.
C++ sends via `PostWebMessageAsJson` (pre-parsed object → JS `e.data` is already an object).
Frontend `hostCall` auto-unwraps `.result` internally — all callers receive raw command results.
`json_get_str` handles escaped quotes (`\"`) in JSON values.

### TargetPickerModal — unified window + mode picker
`WindowPickerModal` + `CaptureModeModal` merged into `TargetPickerModal`.
Two pages (window list / capture mode) slide horizontally via `translateX` transition.
`animReady` guard prevents stale page state animation on re-open.
Both pages share same dimensions: `w-[520px] max-h-[min(560px,85vh)]`.
Back button (`ChevronLeft`) on mode page returns to window list.

### Auto Method toggle
`autoMethod` state (default ON) in App. When ON, `forceMethod` auto-syncs to `winState`:
`minimized → dxgi`, else `→ wgc`. `useEffect([winState, autoMethod])` drives sync.
Toggle switch in Capture Method section. Auto ON → active method gets amber border,
all radios disabled. Auto OFF → manual blue-border selection.
GDI/PrintWindow/ScreenBitBlt removed from UI (C++ impl preserved).

### Connection header restructure
Header split: `[Connection]` left, `[状态 actual] [推荐 WGC/DXGI] [▼]` right.
`recommendedMethod` derived from `winState` (not `forceMethod`) — always reactive.
`expectedCaptureState` tracked from mode picker; amber warning when actual state differs.
Method short names: `wgc→WGC`, `dxgi→DXGI`, `DesktopBlt→DXGI`.


### Vite HMR fix
Explicit `hmr: { protocol: 'ws', host: 'localhost' }` in vite.config.ts for WebView2 compatibility.

### Log UX: remove truncation, add copy + refresh (2026-07-08)
Removed 截断 (scissors) button from Current Session + right panel Log.
Replaced with copy button (checkmark animation) in right panel Log.
Added refresh button (with spin animation) to each history file tile.
History tile buttons ordered: refresh → copy → expand (copy rightmost).
Removed unused `IconBtn` component; fixed TS errors (`_setPendingWin`, `STATE_LABEL` index).
