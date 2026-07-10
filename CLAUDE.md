# CLAUDE.md — TicTacToe → General Visual Game AI

## ⛔ 思想钢印 — 七条铁律

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

### 铁律 4: Tooltip 只用自定义组件

**禁止原生 HTML `title` 属性。** 只用 `<Tooltip text="...">` 包裹。

自定义 Tooltip：300ms 延迟、Portal 到 body、智能定位、统一外观。

### 铁律 5: 禁止欺骗 — 后端不骗前端，前端不骗用户

**C++ 层不得对前端透明地修改行为。** 前端命令必须原样执行——成功返回数据，失败返回 error。

| 规则 | 说明 |
|------|------|
| 5a. 不静默修改参数 | 前端根据目标选择方法，C++ 只执行 |
| 5b. 必须检查返回值 | SendInput/PostMessage/GetClientRect 等失败必须返回 error |
| 5c. 前端反馈匹配实际 | 画了什么 = 实际发了什么。没发的别画，发了的别藏 |

**简洁记忆：**
- **C++ → TS：** `{"ok":false, "error":"..."}` 比静默 `{"ok":true}` 好一万倍
- **TS → 用户：** 画了什么 = 实际发了什么

### 铁律 6: 前端交互优化 = 状态转换表 → 确认 → 改码

当用户说"前端交互方案"、"交互优化"等词语时：
1. **先分析** — 阅读当前逻辑，理解状态机
2. **给状态转换表** — `| # | 当前状态 | 事件 | 新状态 | 原因 |`
3. **等待确认** — 用户说"确认"或"开始"后才动手
4. **改代码** — 精确修改，不改表外逻辑

---

## Project Vision

Build self-organizing hierarchical visual game AI. Model interface: **pixels in, actions out**.
C++ for all real-time work: capture + WebView2 GUI + TCP + logging.
Python for AI model training/inference.

## Architecture

```
┌─ monitor_app (C++ Win32) ────────────────────────────────────┐
│  React (TypeScript + Tailwind)  ←→  C++ backend (same proc) │
│       WebView2 COM 原生           WebMessage bridge          │
│       Dev: → localhost:1420       SharedBuffer 零拷贝        │
│       Prod: → gam.local (嵌入)    BGRA→RGBA 直推            │
└──────────────────┬───────────────────────────────────────────┘
                   │
     ┌─────────────┼──────────────┐
     ▼             ▼              ▼
  C++ capture   C++ logger     TCP :9999
  per-method     (logger/)      (agent/Python)
  .lib
```

| Language | Role |
|----------|------|
| C++ | Host: Win32 window, WebView2, capture, MJPEG server, logging |
| TypeScript/React | UI (WebView2 内运行) |
| Python | AI model training/inference (TCP :9999) |

## Project Structure

```
tictactoe/
├── logger/                   # 统一 C++ 日志系统 (C API)
├── protocol/                 # 线协议 — C++/Python 共享
├── capture/                  # C++ 屏幕捕获 (per-method .lib)
│   ├── src/                  # wgc/gdi/pw/screen/desktop/dxgi
│   └── include/              # Public headers
├── input/                    # C++ 输入转发 (per-method .lib)
│   ├── include/              # InputArgs + 4 method signatures
│   └── src/                  # sendinput/winapi/postmessage/driver
├── monitor_app/              # C++ WebView2 宿主
│   ├── src/                  # main + commands + mjpeg + json_helper
│   ├── dep/                  # WebView2 SDK
│   ├── build.cmd             # Prod: /O2, 嵌入 dist, → build/
│   └── build_dev.cmd         # Dev: /Od, HMR, → build_dev/
├── monitor_web/              # React 前端 (Vite + Tailwind)
│   └── src/
│       ├── App.tsx           # 主编排 (~530 lines)
│       ├── components/       # 11 组件文件
│       ├── lib/              # bridge/types/constants
│       └── webview2.d.ts     # WebView2 类型声明
├── model/                    # Python AI
└── log/                      # 统一日志输出 (gitignored)
```

## Build Commands

```bash
# 1. Build static libs (once, or when C++ changes)
cd logger   && build_logger_lib.cmd
cd capture  && build_capture_lib.cmd
cd input    && build_input_lib.cmd

# 2a. Dev build (Vite HMR, debug)
cd monitor_web && npm run dev        # Vite :1420
cd monitor_app && build_dev.cmd      # → build_dev\monitor_app.exe

# 2b. Prod build (optimized, self-contained)
cd monitor_web && npm run build      # Vite → dist/
cd monitor_app && build.cmd          # embed dist → build\monitor_app.exe
```

| | Dev | Prod |
|---|---|---|
| Optimize | `/Od` | `/O2 /Gy /Gw /GS-` |
| Debug info | `/Zi /DEBUG:FULL` | None |
| CRT | `/MT` | `/MT` |
| Macro | `DEV_MODE` | `NDEBUG` |
| Binary | ~2.4 MB | ~451 KB |

### Single-instance guard

| Build | Mutex | Window class | Title |
|-------|-------|--------------|-------|
| Prod  | `Global\GameAgentMonitor_8A3F2D`     | `GameAgentMonitor`     | `Game Agent Monitor`       |
| Dev   | `Global\GameAgentMonitor_8A3F2D_Dev` | `GameAgentMonitor_Dev` | `Game Agent Monitor (Dev)` |

Exit code `2` = already running (existing window raised).

## Internal Architecture

### Communication: WebMessage bridge

```
JS:  hostCall('list_windows') → chrome.webview.postMessage('{"cmd":"list_windows","id":1}')
C++: WebMessageReceived → HandleWebMessage → dispatch → PostWebMessageAsJson({id, result})
JS:  'message' event → e.data is pre-parsed → hostCall auto-unwraps .result
```

### Command dispatch

| Command | Args | Returns |
|---------|------|---------|
| `list_windows` | — | `[{title, category, hwnd, desktop}, ...]` |
| `capture_window` | `{hwnd, method}` | `{ok, w, h, method}` — via SharedBuffer |
| `capture_stream_start` | `{hwnd, method, transport}` | `{ok:true}` |
| `capture_stream_stop` | — | `{ok:true}` |
| `read_logs` | `{max_files}` | `{files:[{name, size}, ...]}` |
| `read_log_file` | `{filename}` | `{filename, content}` |
| `read_live_log` | — | `{lines}` — ring buffer sync |
| `log_ui_event` | `{event, detail}` | `{ok:true}` — no echo back |
| `send_input` | `{hwnd, type, x_norm, y_norm, button, method}` | `{ok:true}` |
| `get_version` | — | `"0.3.0"` |
| `list_desktops` | — | `[{name, index, current}, ...]` |
| `switch_desktop` | `{index}` | `{ok:true}` |
| `benchmark_methods` | `{hwnd, method}` | `{results:[...]}` |
| `set_frame_dump` | `{capture, stream, dir}` | `{ok:true}` (Dev mode) |
| `launch_test_target` | — | `{ok, action}` — toggle test window |
| `find_test_target` | — | `{hwnd}` — 0 if not running |
| `selftest_connect` / `selftest_disconnect` | `{port}` / — | GAM→test_target TCP client (:9998) |

### Self-Test — mapping calibration (Dev)

`test_target` 开 TCP server :9998（loopback, JSON-lines）。DEV 面板 Self-Test 一键复用
真实用户回调链（选窗→预览→映射→`sendMappedClick` 密集点击），test_target 回传实收
`{x,y,gx,gy,hit}`，前端 `predict()` 按握手几何算预期 → 逐格命中率热力图 + 偏移向量 + 像素误差。
握手 `{type:"hello",client_w,client_h,grid,cell,pad,hit_margin}`。详见 CLAUDE.old.md。

### Method routing (铁律 5)

**Frontend decides method, C++ only executes.** No silent fallback.

Single-frame (`call_capture`):
| Method | Backend |
|--------|---------|
| `wgc` | `wgc_capture_single(hwnd)` — hwnd=0 → error |
| `wgc-monitor` | `wgc_capture_single_monitor(hmon)` |
| `dxgi` / `desktopblt` | DesktopBlt, returns `method="DesktopBlt"` |
| `GDI(GetWindowDC)` | `capture_gdi_getwindowdc(hwnd)` |
| `PrintWindow` | `capture_printwindow(hwnd)` |
| unknown | Returns error, no fallback |

Streaming (`capture_stream_start`):
| Method | Backend |
|--------|---------|
| `wgc` | WGC stream (hwnd or monitor mode) |
| `dxgi` | Returns error — not implemented |
| unknown | Returns error |

### Input methods (3-mode mouse + keyboard)

Mouse modes (Settings → Capture → Mouse Mode):
| Mode | Move | Click method | Description |
|------|------|-------------|-------------|
| Background (default) | ❌ virtual only | PostMessage | 全后台不抢鼠标 |
| Semi | ❌ virtual only | SendInput | 点击时短暂抢鼠标 |
| Seize | ✅ 60fps | SendInput | 前台完全抢鼠标 |

Keyboard modes (Settings → Capture → Keyboard Mode):
| Mode | Method | Description |
|------|--------|-------------|
| PostMsg (default) | PostMessage | 异步高效 |
| SendMsg | WinAPI | 同步稳定 |
| Seize | SendInput | 前台独占 |

### Streaming pipeline

```
WGC → condition_variable → TryGetNextFrame → CopyResource(GPU) → Map(CPU)
  → BGRA → stream_bridge_push_frame (MTA thread)
  → PostMessage(WM_STREAM_FRAME)
  → [STA main thread] shared_buffer_push_frame → PostSharedBufferToScript
  → [JS] sharedbufferreceived → ImageData → Canvas putImageData
```

Stream bridge uses PostMessage because WebView2 SharedBuffer interfaces are STA-only,
WGC requires MTA, and COM marshaling fails (0x80040155 — no proxy/stub).

### SharedBuffer (zero-copy)

```cpp
env12->CreateSharedBuffer(w * h * 4, &buf);
buf->get_Buffer(&dst);  // COM method
// BGRA→RGBA inline swap
wv17->PostSharedBufferToScript(buf, READ_ONLY, meta);
buf->Close();  // AFTER Post — buffer must stay open when posted
```

### Logging architecture

```
TS addLog(msg)
  ├─ immediate: LogManager.entries → React (0ms)
  └─ hostCall('log_ui_event') → file + ring buffer (no echo back)

C++ LOG(tag, msg)
  ├─ file + ring buffer
  └─ on_log_notify → PostWebMessage({type:'log',...})
       └─ TS addRemote → GUI

Startup: hostCall('read_live_log') — catch-up ring buffer entries
```

Key: all logs flow through same pipeline. Ring buffer + file. TS and C++ views identical.

### Log collapse — consecutive duplicate aggregation

Continuous identical (tag, msg) entries collapsed to single entry with `[firstTs → lastTs] ×N`.

| Layer | Strategy |
|-------|----------|
| C++ ring buffer | In-place update (no duplicate added) |
| C++ log file | Write-then-collapse (crash-safe: raw first, then overwrite + truncate) |
| TS addRemote | C++ notify sends count/firstTs, TS stores as-is |
| TS add (UI) | Independent check-then-update |

### Capture methods

| Method | Lib | Sys deps |
|--------|-----|----------|
| WGC | wgc.lib | d3d11, dxgi, windowsapp |
| GetWindowDC | gdi.lib | user32, gdi32 |
| PrintWindow | pw.lib | user32, gdi32 |
| ScreenBitBlt | screen.lib | user32, gdi32 |
| DesktopBlt | desktop.lib | user32, gdi32 |
| Common | common.lib | user32, dwmapi |

### Wire protocol (protocol/)

```
Frame: [magic:4 "FRAM"][body_size:4 LE][type_tag:4 LE][body]
type_tag 1 (BGRA): [w:4][h:4][ch:4][reserved:4][pixels]
DEFAULT_TCP_PORT=9999
```

### Dev mode + two-color theme

8 theme pairs (7 normal + 1 Dev red/green). Dev mode ON → auto-switch Dev pair.
Settings → General → Dev mode toggle enables frame dump to disk.
Mapping key: sequence-based (Ctrl+K ≠ K+Ctrl), modifier-only warning, test indicator.

### MonitorView remote-control mode

Mouse: 3-mode (Seize/Semi/Background) with virtual cursor overlay (OBS-style dot+ring).
Self-target detection: red cursor + warning when mapped position overlaps GAM window.
Keyboard: 3-mode (Seize/PostMsg/SendMsg). Canvas focus for keyboard forwarding.
Settings: self-target avoidance (warn vs WDA_EXCLUDEFROMCAPTURE exclude).
Toolbar: target title + state badge + Snapshot + Preview/Stop + 映射 toggle.

### UI component decomposition

```
App.tsx (~530 lines) → 11 components:
  Toolkit (Tooltip, ActionBtn, ThemeBtn)
  TopBar, BottomBar
  TargetPickerModal, ConnectionPanel
  ScreenshotPanel, LogPanel
  SettingsView, MonitorView
lib/: bridge.ts, types.ts, constants.ts
```

---

## Known Issues

1. **WGC FPS**: Event-driven — static content = low FPS. Dynamic window = 60+.
2. **H.264 MFT**: Encoder creates MP4 for progressive download, `<video>` needs full file.
3. **Chromium background tab throttling**: WebView2 may throttle when app loses focus.
4. **WebView2 cross-thread COM**: STA-only interfaces, COM marshaling fails. Stream uses PostMessage bridge.
5. **Async break-point jitter**: TS `hostCall('log_ui_event')` arrives async — may split C++ log runs. Cosmetic only.

---

### 铁律 7: CLAUDE.md 保持精简，详细内容写入 CLAUDE.old.md

CLAUDE.md 只放核心规则、架构概览、构建命令。以下内容**必须写入 CLAUDE.old.md**：
- 开发日志 / Recent Fixes 详细描述
- 指令的详细说明和背景故事
- 历史变更的完整记录

CLAUDE.md 只保留摘要和指向 CLAUDE.old.md 的引用。

---

## Changelog

Full development history preserved in `CLAUDE.old.md`. Major milestones:
- **2026-07-10 (Phase 2)**: Modular DLL build (12 DLLs with VERSIONINFO), InnoSetup installer, settings persistence (get/set_setting), multi-file incremental update (version.json manifest + updater.exe + Gitee raw file download), paths system (install dir + appdata), settings auto-save/load
- **2026-07-10 (auto-update)**: Phase 1 full-EXE update — WinHTTP `check_update` (Gitee API) + `download_update` (download + swap.bat + self-replace); UpdateModal + BottomBar indicator; fix GitHub→Gitee link in SettingsView
- **2026-07-10 (gitignore)**: Rewrite `.gitignore` — cover `build_dev/`, WebView2 runtime cache (`*.exe.WebView2/`), nested log dirs, `*.res`, `tmp/`; remove stale Rust `target/` rule
- **2026-07-10 (self-test)**: test_target 判定区缩小(inner hit-margin) + 真实 IME 输入框(EDIT child); TCP self-test 通道(:9998 JSON-lines) — DEV 面板一键映射校准, 复用真实点击回调 sendMappedClick, predict vs 实收 → 命中率热力图/偏移向量/像素误差; 新组件 SelfTestModal + lib/selftest.ts (13 组件)
- **2026-07-10**: Log collapse, CSS rename accent-dev→accent-secondary, MonitorView clear canvas on stop, real-screen cursor overlay (C++ WS_EX_LAYERED UpdateLayeredWindow), self-target detection + exclude toggle, 3-mode input (mouse/keyboard Seize/Semi/Background), WDA_EXCLUDEFROMCAPTURE, desktop input support, test_target EXE, WGC crash fixes (out_ch nullptr, timing)
- **2026-07-09**: Two-color theme + Dev mode, MonitorView remote-control, component decomposition (1→11 files), input mapping
- **2026-07-08**: Method routing 铁律 5 enforcement, stream bridge, SharedBuffer pipeline, log UX
- Earlier: Rust→C++ migration complete, WGC/DXGI capture, MJPEG server, TCP protocol
