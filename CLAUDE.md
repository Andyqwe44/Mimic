# CLAUDE.md — TicTacToe → General Visual Game AI

## ⛔ 思想钢印 — 十条铁律

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
LOG_ERROR("tag", "format", args...);  // 错误 — operation failed, must fix
LOG_WARN("tag", "format", args...);   // 警告 — fallback used, retryable
LOG("tag", "format", args...);        // INFO — status change, user action
LOG_DEBUG("tag", "format", args...);  // DEBUG — frame detail, param dump (dev only)
```

| 标签 | 用途 |
|------|------|
| `wgc` `dxgi` | 捕获 |
| `cmd` | 命令调度 |
| `main` | 主循环/启动 |
| `mjpeg` | MJPEG 服务器 |
| `ui` | 前端事件 |
| `agent` | AI Agent |

**日志等级规范（四级）：**

| 等级 | 值 | 宏 | 场景 | Dev | Prod |
|------|----|-----|------|:--:|:----:|
| DEBUG | 0 | `LOG_DEBUG()` | 帧级细节、参数dump、性能计时 | ✅ | ❌ |
| INFO | 1 | `LOG()` / `LOG_INFO()` | 状态变更、用户操作、正常流程 | ✅ | ✅ |
| WARN | 2 | `LOG_WARN()` | 可恢复问题、fallback、retry | ✅ | ✅ |
| ERROR | 3 | `LOG_ERROR()` | 硬错误、操作失败 | ✅ | ✅ |

Dev 启动时自动 `capture_log_set_level(LOG_LEVEL_DEBUG)`，prod 设 `LOG_LEVEL_INFO`。
文件格式: `[12:34:56.789] [INFO ] [tag] message`（`%-5s` 列对齐）。
JSON notify 含 `"level":"INFO","lvl":1` 字段供前端颜色区分。

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

### 铁律 7: CLAUDE.md 保持精简，详细内容写入 CLAUDE.old.md

CLAUDE.md 只放核心规则、架构概览、构建命令。以下内容**必须写入 CLAUDE.old.md**：
- 开发日志 / Recent Fixes 详细描述
- 指令的详细说明和背景故事
- 历史变更的完整记录

### 铁律 8: 版本号单一真相源 — `version.h`

**只改一个文件：`monitor_app/src/version.h`。** 其余全部自动继承：

| 消费者 | 继承方式 |
|--------|----------|
| `monitor_app/app.rc` | `#include "src/version.h"` → `APP_VERSION` / `APP_VERSION_RC` |
| `logger` 运行时 banner | monitor_app `capture_log_init("agent", APP_VERSION, …)` **运行时传参** |
| `installer/setup.iss` | `Release.ps1` 读 version.h → `ISCC /DMyAppVersion=<ver>` |
| 前端 `App.tsx` | 运行时 `hostCall('get_version')`；构建时 `vite.config` 读 version.h → `__APP_VERSION__` |
| `scripts/New-VersionJson.ps1` | `Get-AppVersion`（读 version.h）→ version.json |

**⚠️ 原生 lib/DLL（logger/capture/input，共 12 个）版本已与 APP_VERSION 脱钩**（真增量更新）：
其 VERSIONINFO 用 `Build.ps1` 的独立 `$LibVer`（模块版本，**改 lib 源码才手动 bump**），
配 `/Brepro` 确定性 PE → app 版本 bump 不改 lib 字节 → 发版只重下 `monitor_app.exe`。
**勿**把 lib 版本改回读 version.h（会退回「每版全量下载」的假增量）。

### 铁律 9: 跨进程命令行参数 — 禁止引号嵌入路径

**背景**: `ShellExecuteEx` + `lpParameters` 把参数字符串传给新进程的 `lpCmdLine`。如果调用方把路径用双引号包了（如 `"\"C:\\...\\staging\" 12345"`），新进程的 `strtok`/`__argc` 不会自动剥离引号——**引号会变成路径的一部分**，导致 `FindFirstFileA`/`CreateDirectoryA` 失败。

| 规则 | 说明 |
|------|------|
| 9a. 参数里**不加引号** | 调用 `ShellExecuteEx` / `CreateProcess` 时，`lpParameters` **不对路径加引号** |
| 9b. 被调进程**必须剥引号** | `updater` / 任何接收命令行的进程：解析参数后立即 `unquote` 两端双引号 |
| 9c. 测试「路径带空格」场景 | 本地模拟含空格文件夹名，确保全链不炸 |

**斜杠规则（Windows）：**
- `\\` 单反斜杠：字符串中拼接路径用 `"C:\\dir\\file"` 或 `R"(C:\dir\file)"`（raw literal）
- `/` 正斜杠：传给 `ShellExecute` / `WebView2` 的 URL 只用 `/`
- 路径连接用 `snprintf(buf, sz, "%s\\%s", dir, name)` — 别手拼 `+`
- 尾反斜杠：除 `"\\*"` 外，目录路径不准以 `\\` 结尾

### 铁律 10: 防上下文污染 — 禁止编造工具返回值

**5 条规则（详见 [[anti-contamination-rules]]）：**
1. **禁止编造工具返回值** — 没调过就说没调过，不编造 Bash/Read/Grep/Glob 的返回
2. **声明必须可溯源** — 每事实要么刚调过工具，要么标"来自记忆，需核实"
3. **分歧立刻核实，不解释** — 我说 A 工具返 B → 立刻贴 B 说"A 错了"，不编"污染"之类解释
4. **关键节点锚定真实状态** — 版本号/commit hash/tag/release 状态先跑 `git log --oneline -5` + `git tag` 再给结论
5. **摘要前先核实** — compact/总结前先 git 核实锚点，写入摘要作为 ground truth

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
│   └── dep/                  # WebView2 SDK
├── scripts/                  # ⭐ 全 PowerShell 构建/发布链
│   ├── lib/Common.ps1        # Enter-VsDevShell / Get-AppVersion / New-VerModuleHeader
│   ├── Build.ps1             # 编译全模块 (一个 VS Dev Shell, -Module/-Dev)
│   ├── New-VersionJson.ps1   # version.json (Get-FileHash)
│   ├── Verify.ps1            # 隔离验证
│   ├── Publish.ps1           # Gitee (Invoke-RestMethod)
│   ├── Release.ps1           # 顶层编排
│   └── Read-InstalledLogs.ps1 # 抓装机版运行时日志
├── monitor_web/              # React 前端 (Vite + Tailwind)
│   └── src/
│       ├── App.tsx           # 主编排
│       ├── components/       # UI 组件
│       ├── lib/              # bridge/types/constants
│       └── webview2.d.ts     # WebView2 类型声明
├── model/                    # Python AI
└── log/                      # 统一日志输出 (gitignored)
```

## Build Commands

**All build/release scripts are PowerShell under `scripts/`** — one VS Dev Shell
(`Enter-VsDevShell`, no `vcvars.bat`), no cmd/node/bash.

```powershell
# 1. Native modules — logger + capture + input + updater + monitor_app (prod)
powershell -File scripts\Build.ps1                      # all, one VS Dev Shell
powershell -File scripts\Build.ps1 -Module logger       # a single module

# 2a. Dev build (Vite HMR + debug monitor_app → build_dev\bin\)
cd monitor_web; npm run dev                             # Vite :1420
powershell -File scripts\Build.ps1 -Module monitor_app -Dev

# 2b. Prod build (optimized, self-contained → monitor_app\build\{bin,frontend,config})
cd monitor_web; npm run build                           # Vite → dist/
powershell -File scripts\Build.ps1 -Module monitor_app  # stages dist into build\frontend
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

## Release Workflow — Dev → Prod → Gitee → One-Click Update

完整发布链：dev 验证 → prod 构建 → 打包 installer → Gitee Release → 用户点"Check Update"。

### 一键发布（推荐）

```powershell
# 改完 monitor_app/src/version.h 后只需一条命令:
powershell -File scripts\Release.ps1            # build → verify → git push → Gitee → 验 raw URL
powershell -File scripts\Release.ps1 -DryRun    # 只 build+verify（不 push/发），安全全链自测
```

`Release.ps1` 全 PowerShell 一条链（无 cmd/bash/node/curl）：
1. `npm run build` → `Build.ps1 -Module all`（一个 VS Dev Shell 编译全部）
2. assemble `release\GameAgentMonitor`（只拷 `build\{bin,frontend,config}`）→ `New-VersionJson.ps1`（SHA256 每文件）→ ISCC installer
3. `Verify.ps1`（拷包到 `%TEMP%\GAM_verify` repo 外启 exe，轮询 90s 抓 `prod: frontend served`）→ 通过才 git commit+tag+push
4. `Publish.ps1`（`Invoke-RestMethod` 建 Gitee Release + 手搓 multipart 传 installer）→ 验 raw URL 302→200

构建产物结构 = 发布包结构：
```
monitor_app/build/{bin,frontend,config}   # prod, exe 在 build\bin\
monitor_app/build_dev/bin/                 # dev (Vite HMR)
```

**隔离验证为何关键**：白屏只在真机安装复现，本地 prod 从不复现。把包拷到 repo 外消除路径巧合。

### 用户一键更新

```
Settings → Check Update
  → GET Gitee API /releases/latest → 对比版本号
  → 如果 remote > local → GET raw/<tag>/release/GameAgentMonitor/version.json
  → 逐文件比对 SHA256 → 生成 diff → 下载 → sha256 校验 → updater 覆盖 → 重启
```

## Update Mechanism & Release Package

> **完整自动更新逻辑 + 踩坑史(6 个坑)+ 已知隐患 → [`docs/auto-update.md`](docs/auto-update.md)**

### 发布包结构

```
release/GameAgentMonitor/
  bin/          monitor_app.exe · updater.exe · updater.new · 12 DLL
  frontend/     dist (index.html, assets/)
  config/       settings.default.json
  version.json  schema v2 { schema, app, released, channel, min_version, mandatory, message,
                full_update, download_base, updater{path}, sig, files{...} }
release/GameAgentMonitor_Setup_v<ver>.exe  ← Inno Setup installer
```

**增量更新下载源 = git raw URL**（不是 Release 附件）。Release 附件(setup.exe)只给全新装机；增量更新逐文件从
`download_base + <path>` 拉 + sha256 校验。`release/GameAgentMonitor/` 必须 commit——它本身就是「增量货」。
`download_base`(schema v2)= 服务端可换下载源不重编客户端。

### Updater 运作（`updater/updater.cpp`，requireAdministrator manifest）

1. **覆盖更新** `updater.exe <staging_dir> <old_pid>`：等旧进程退出 → 递归拷 staging → install（CopyFileA overwrite，additive）
   → 遇目标==自己：`MoveFileExA(→.old)` 再拷新 → 启 monitor_app → remove_tree(staging)
2. **自装** `updater.exe --self-install`：把自己拷成同目录 updater.exe → `MoveFileEx(DELAY_UNTIL_REBOOT)` 删 .new

**完整更新链**：`download_update`（后台 std::thread 逐文件下+sha256 校验→staging）→ 启 updater 覆盖 → 启新 monitor_app
→ 首启 `check_and_heal_updater`（比 install updater.exe sha vs version.json；旧则启 `updater.new --self-install`）。

### Gitee 注意事项

| 问题 | 解决方案 |
|------|----------|
| **Gitee 不允许替换 Release asset** | 必须 DELETE 整个 Release → DELETE remote tag → 重建 |
| **raw URL 的文件来自 git 仓库** | `raw/<tag>/path` 读取的是 git tag 下提交的文件，不是 Release asset |
| **China 网络** | Gitee 在国内访问稳定；raw URL 无需认证即可下载 |

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
| `get_version` | — | `"0.3.29"` |
| `list_desktops` | — | `[{name, index, current}, ...]` |
| `switch_desktop` | `{index}` | `{ok:true}` |
| `benchmark_methods` | `{hwnd, method}` | `{results:[...]}` |
| `set_frame_dump` | `{capture, stream, dir}` | `{ok:true}` (Dev mode) |
| `launch_test_target` | — | `{ok, action}` — toggle test window |
| `find_test_target` | — | `{hwnd}` — 0 if not running |
| `selftest_connect` / `selftest_disconnect` | `{port}` / — | TCP client (:9998) |

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

Mouse modes:
| Mode | Move | Click method | Description |
|------|------|-------------|-------------|
| Background (default) | ❌ virtual only | PostMessage | 全后台不抢鼠标 |
| Semi | ❌ virtual only | SendInput | 点击时短暂抢鼠标 |
| Seize | ✅ 60fps | SendInput | 前台完全抢鼠标 |

Keyboard modes:
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

## 完整历史

开发日志、Recent Fixes 详细描述、历史变更完整记录 → **`CLAUDE.old.md`**（铁律 7）。
