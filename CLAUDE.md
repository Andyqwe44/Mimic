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
│   └── dep/                  # WebView2 SDK  (构建由 scripts\Build.ps1 -Module monitor_app)
├── scripts/                  # ⭐ 全 PowerShell 构建/发布链 (替旧 .sh/.cmd/.mjs)
│   ├── lib/Common.ps1        # Enter-VsDevShell / Get-AppVersion / New-VerModuleHeader
│   ├── Build.ps1             # 编译全模块 (一个 VS Dev Shell, -Module/-Dev)
│   ├── New-VersionJson.ps1   # version.json (Get-FileHash, 替 gen_version.mjs)
│   ├── Verify.ps1            # 隔离验证 (替 verify_isolated.cmd)
│   ├── Publish.ps1           # Gitee (Invoke-RestMethod, 替 publish_release.sh)
│   └── Release.ps1           # 顶层编排 (替 release.sh + build_release.cmd)
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

**All build/release scripts are PowerShell under `scripts/`** — one VS Dev Shell
(`Enter-VsDevShell`, no `vcvars.bat`), no cmd/node/bash. Files: `lib/Common.ps1`,
`Build.ps1`, `New-VersionJson.ps1`, `Verify.ps1`, `Publish.ps1`, `Release.ps1`.

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

完整发布链：dev 验证 → prod 构建 → 打包 installer → Gitee Release → 用户点"Check Update"即可更新。

### Step 1: Dev 验证

```powershell
# 1. 改版本号（只需改这一个文件! 铁律 8）
#    编辑 monitor_app/src/version.h → APP_VERSION + APP_VERSION_RC

# 2. 构建（一个 VS Dev Shell，无 vcvars/cmd）
powershell -File scripts\Build.ps1                           # libs + updater + monitor_app(prod)
powershell -File scripts\Build.ps1 -Module monitor_app -Dev # dev exe → build_dev\bin\

# 3. 启动前端 + 测试
cd monitor_web; npm run dev                                 # 终端 1: Vite :1420
monitor_app\build_dev\bin\monitor_app.exe                   # 终端 2: Dev app

# 4. 验证: GUI 正常渲染, 版本号正确, 功能无回归
```

### Step 2 → 4: 一键发布 `Release.ps1`（推荐）

**改完 `version.h` 后只需一条命令**（PowerShell）——版本号是唯一手改点（脚本从 version.h 读，不传参）：

```powershell
powershell -File scripts\Release.ps1            # build → verify → git push → Gitee → 验 raw URL
powershell -File scripts\Release.ps1 -DryRun    # 只 build+verify（不 push/发），安全全链自测
```

`Release.ps1` **全 PowerShell 一条链，无跨语言**（无 cmd/bash/node/curl）：
1. `npm run build`（frontend）→ `Build.ps1 -Module all`（一个 VS Dev Shell 编译 libs+updater+app，stage
   `monitor_app\build\{bin,frontend,config}`）。
2. assemble `release\GameAgentMonitor`（只拷 build\{bin,frontend,config}，不含编译中间物）→ `New-VersionJson.ps1`
   （`Get-FileHash` 每文件 sha256，替 gen_version.mjs）→ ISCC installer。
3. `Verify.ps1`（拷包到 `%TEMP%\GAM_verify` repo 外启 exe，`Start-Sleep` 轮询 90s 抓 `prod: frontend served`；
   PS 原生等待，无旧 `timeout`/`ping` 坑）→ 通过才 git commit+tag+push。
4. `Publish.ps1`（`Invoke-RestMethod` 建 Gitee Release + 手搓 multipart 传 installer）→ 验 raw URL 302→200。

构建产物结构 = 发布包结构：

```
monitor_app/build/{bin,frontend,config}   # prod, exe 在 build\bin\
monitor_app/build_dev/bin/                 # dev (Vite HMR, 无 frontend\)
```

**手动分步**（调试用）：

```powershell
powershell -File scripts\Build.ps1 -Module logger            # 只编一个模块（可 -Dev）
powershell -File scripts\Verify.ps1 -Version 0.3.6           # 只隔离验证（不重新构建）
powershell -File scripts\Publish.ps1 -Version 0.3.6 -DryRun  # Gitee 干跑（列出，不发）
```


**隔离验证为何关键**：白屏只在真机安装复现，本地 prod 从不复现。两个掩盖因素——
(1) 旧 HKLM `InstallPath` 把 exe 重定向到*上一次*安装的 frontend（已修 `paths.cpp`，改 exe 相对优先）；
(2) 在 repo/build 树内跑时 frontend 路径与 WebView2 数据目录恰好能解析/可写。
把包拷到 repo 外消除这些巧合，任何打包/路径 bug 在此暴露而非流到用户机。

### Step 3: Gitee 发布（已由 `Release.ps1` → `Publish.ps1` 自动化 — 下为手动/API 参考）

```bash
# 1. Commit + tag + push
V=0.3.4
git add release/GameAgentMonitor/ monitor_app/src/version.h installer/setup.iss
git commit -m "release: v$V"
git tag "v$V"
git push origin main
git push origin "refs/tags/v$V:refs/tags/v$V"

# 2. 在 Gitee 创建 Release (或 API)
#    Tag: v0.3.4, 上传 GameAgentMonitor_Setup_v0.3.4.exe 作为 attachment
#    API: POST /api/v5/repos/Andyqwe44/tictactoe/releases
#         {tag_name, name, body, target_commitish, prerelease:false}
#    Upload asset: POST /repos/.../releases/{id}/attach_files

# 3. 验证: 浏览器打开 raw URL
#    https://gitee.com/Andyqwe44/tictactoe/raw/v0.3.4/release/GameAgentMonitor/version.json
#    确认返回 200 + 版本号正确
```

### Step 4: 用户一键更新

```
用户操作: Settings → Check Update 按钮
↓
check_update:
  1. GET Gitee API /releases/latest → 获取 latest tag_name
  2. 对比本地版本号 (从 version.json 或注册表读取)
  3. 如果 remote > local → GET raw/<tag>/release/GameAgentMonitor/version.json
  4. 逐文件比对 SHA256 → 生成 diff 列表
  5. 返回 {has_update:true, diff:[...], new_version:"0.3.4"}
↓
用户点 "Download & Install":
  1. 逐个下载 diff 文件 → %LOCALAPPDATA%\GameAgentMonitor\staging\
  2. SHA256 校验每个文件
  3. 全部下载完 → 启动 updater.exe <staging_dir> <pid>
  4. monitor_app 退出
  5. updater 覆盖文件 → 重启 monitor_app → 版本号已更新 ✓
```

### Gitee 注意事项

| 问题 | 解决方案 |
|------|----------|
| **Gitee 不允许替换 Release asset** | 必须 DELETE 整个 Release → DELETE remote tag → 重建。上传同名文件只是追加第二个 asset |
| **raw URL 的文件来自 git 仓库** | `raw/<tag>/path` 读取的是 git tag 下提交的文件，不是 Release asset。所以 release 文件必须 commit 到 git |
| **China 网络** | Gitee 在国内访问稳定；raw URL 无需认证即可下载 |

## Update Mechanism & Release Package

### 发布包结构（`Release.ps1` 组装）

```
release/GameAgentMonitor/
  bin/          monitor_app.exe · updater.exe · updater.new · 12 DLL (logger + capture×6 + input×5)
  frontend/     dist (index.html, assets/)          ← npm run build
  config/       settings.default.json
  version.json  { app, released, full_update, files{ "bin/x.dll":{v,sha256,size}, ... } }
release/GameAgentMonitor_Setup_v<ver>.exe           ← Inno Setup installer → %ProgramFiles%\GameAgentMonitor
```

`Release.ps1` 链：`npm run build`(frontend) → `Build.ps1 -Module all`（一个 VS Dev Shell 编译 logger→capture→input
→updater→monitor_app(prod)，`Build-MonitorApp` 顺带 stage `build\{bin,frontend,config}`：拷 12 DLL+updater.exe→bin、
`updater.exe→updater.new`、dist→frontend、settings.default→config）→ **assemble** `release\`（只拷 build\{bin,frontend,
config} 三夹，不含编译中间物）→ **version.json**（`New-VersionJson.ps1` `Get-FileHash` 遍历 release 算每文件 sha256）
+ **installer**（ISCC setup.iss）→ `Verify.ps1`（轮询 frontend served）→ git commit+tag+push → `Publish.ps1`(Gitee)。
**`build\{bin,frontend,config}` == `release\` == 用户装机结构**（dev==prod==包，消除相对路径假通过）。`version.json`
每文件 sha256 = 增量比对依据；`updater.new` = updater.exe 字节副本（死循环破冰用）。

### Updater 运作（`updater/updater.cpp`，requireAdministrator manifest）

两种调用：
1. **覆盖更新** `updater.exe <staging_dir> <old_pid>`（download_update 完成后主线程 CreateProcess）：
   等 old_pid 退出(≤30s，超时 Terminate)+Sleep 500ms → 注册表 `HKLM\SOFTWARE\GameAgentMonitor\InstallPath` 读 install
   → `copy_staging` 递归拷 staging 全部 → install（CopyFileA overwrite，**additive**：不删 staging 缺的文件）
   → 遇目标==自己：`MoveFileExA(updater.exe→.old)` 再拷新（改名技巧=自替换）→ 启 monitor_app.exe → `remove_tree(staging)`。
2. **自装** `updater.exe --self-install`（monitor_app 首启把 `bin\updater.new` 拉起时）：把自己拷成同目录 updater.exe
   （updater.exe 此刻没跑）→ `MoveFileEx(self,NULL,DELAY_UNTIL_REBOOT)` 重启删 updater.new。不等 pid、不拷 staging、不启 app。
   启动均先删 `updater.exe.old` 残留。

**完整更新链**：`download_update`（后台 std::thread 逐文件下+sha256 校验→staging，节流 `WM_UPDATE_PROGRESS` 进度）
→ done 启 updater.exe 覆盖（除自身）→ 启新 monitor_app → 首启 `check_and_heal_updater`（比 install updater.exe sha
vs version.json；旧则启 `updater.new --self-install`，monitor_app 仅启动器）→ updater 升级。
**0.3.3→0.3.5 一跳修好 updater；0.3.5+ 每次由上一版 updater 的改名技巧自替换，monitor_app 不再参与。**

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

### 铁律 8: 版本号单一真相源 — `version.h`

**只改一个文件：`monitor_app/src/version.h`。** 其余全部自动继承：

| 消费者 | 继承方式 |
|--------|----------|
| `monitor_app/app.rc` | `#include "src/version.h"` → `APP_VERSION` / `APP_VERSION_RC` |
| `logger/logger.cpp` + 各 DLL | `Build.ps1`（`New-VerModuleHeader`）从 version.h 生成 `build/_ver_module.h`（给 RC 用） |
| `installer/setup.iss` | `Release.ps1` 读 version.h → `ISCC /DMyAppVersion=<ver>`，setup.iss 条件 define |
| 前端 `App.tsx` | 运行时 `hostCall('get_version')`；构建时 `vite.config` 读 version.h → `__APP_VERSION__`（splash 秒显） |
| `scripts/New-VersionJson.ps1` | `Get-AppVersion`（读 version.h）→ version.json |

**改版本号步骤：**
1. 编辑 `monitor_app/src/version.h`：修改 `APP_VERSION` 和 `APP_VERSION_RC`
2. 重建所有 lib + app（构建脚本自动读取新版本）
3. 前端 `npm run build`（JS hash 自动更新）
4. 组装 release 目录 + 生成 `version.json` + ISCC 打包

**严禁：** `sed` 批量替换版本号。之前 12 个硬编码位点已全部消除。

### 铁律 7: CLAUDE.md 保持精简，详细内容写入 CLAUDE.old.md

CLAUDE.md 只放核心规则、架构概览、构建命令。以下内容**必须写入 CLAUDE.old.md**：
- 开发日志 / Recent Fixes 详细描述
- 指令的详细说明和背景故事
- 历史变更的完整记录

CLAUDE.md 只保留摘要和指向 CLAUDE.old.md 的引用。

---

## Changelog

Full development history preserved in `CLAUDE.old.md`. Major milestones:
- **2026-07-12 (build/release 全 PowerShell 化)**: 治理散乱脚本 —— 旧发布链是 `release.sh`(bash)→`build_release.cmd`(cmd)
  →各 `build_*.cmd`(cmd)+`gen_version.mjs`(node)+curl，**三生态跨语言链**，脆、难懂、git-bash 下踩 timeout/PATH 坑。
  全部迁到 **`scripts/*.ps1`**（一种语言，一条链，无 cmd/bash/node/curl）：`lib/Common.ps1`(`Enter-VsDevShell` 替
  vcvars、`Get-AppVersion`、`New-VerModuleHeader`、日志)；`Build.ps1`(一个 VS Dev Shell 编译 logger/capture/input/
  updater/monitor_app，`-Module`/`-Dev`)；`New-VersionJson.ps1`(`Get-FileHash` 替 gen_version.mjs 的 node)；
  `Verify.ps1`(`Start-Sleep` 轮询，无 timeout/ping 坑)；`Publish.ps1`(`Invoke-RestMethod`+手搓 multipart 替 curl)；
  `Release.ps1`(顶层编排，`-DryRun` 安全自测)。**逐个迁移逐个验证**：各模块产物比对、version.json 21 文件 sha256 与
  node 版逐一致、`Release.ps1 -DryRun` 全链跑通(frontend served + VERIFICATION PASSED)。删 14 个旧脚本(release.sh、
  build_release.cmd、publish_release.sh、verify_isolated.cmd、各 build_*.cmd、`_bld.cmd`×3、gen_version.mjs)。
  保留 `capture/build.cmd`(h264 实验)、`input/build.cmd`(input test)、game/agent/test_target `build.cmd`、`ai/run_*`
  ——非发布链的独立辅助/实验，未迁。
- **2026-07-11 (startup white-screen → hidden-window + skeleton screen，未发布)**: 治真机开屏白屏 2-4s。
  诊断(埋计时 LOG 实测)：**不是** DLL 拆分(原生加载毫秒级)、**不是** React(bundle 365KB 本地供给 ~100ms)、
  `backend_init` 仅 **16ms**(先前「backend 串行拖」假设推翻)；2-4s 几乎全是 **WebView2 env 创建**(固有,削不掉)。
  参考 `codes/MXU`(MaaEnd 的 Tauri 前端)做法 —— `tauri.conf visible:false` 窗口先隐藏、前端就绪再 `show()`。
  移植：`main.cpp` 窗口建时**隐藏**(去 `ShowWindow`)+ 深色 `hbrBackground` 兜底；`show_window` 命令
  (`WM_APP_SHOW_WINDOW`)由前端首帧 double-rAF 后调用 → `show_main_window()` 幂等揭窗；`WM_TIMER` **8s 兜底**
  (前端崩了也弹窗)。隔离 Win32 test 实证「隐藏窗→SetTimer→WM_TIMER→ShowWindow」机制成立(headless 跑不了
  webview,揭窗真机验)。**骨架屏**(Alipay 式)：新 `LoadingScreen.tsx` overlay(z-50)镜像真实默认屏结构
  (顶栏 3tab+Start/主题、左列 StatusBar 条+5 卡、分隔、右列 3 卡、底栏),内容换 `.skeleton` shimmer 块
  (`gam-shimmer` keyframes + 主题变量,深浅自适应,`index.css`)；手写近似非组件派生(布局大改需手动同步)。
  `App.tsx`：`appReady` gate + `SPLASH_TEST_MS=1500`(**故意** test 停留,prod 设 0)+ `?splash` query 冻结预览。
  **版本注入**(铁律 8)：`vite.config` 读 `version.h` → `__APP_VERSION__` define,splash/UI 秒显版本(不再 `...` 闪)。
  计时 LOG 保留(INFO,可筛)。**保留想法**：给首页真异步零件(日志/窗口列表/读盘)做 per-widget 骨架 —— 毫秒级暂缓。
  并入 **0.3.6**(version.h 已升),待真机验证后 `release.sh 0.3.6` 发布。
- **2026-07-11 (runtime data → LOCALAPPDATA + Inno 托管，未发布)**: 统一所有运行时写入到
  `%LOCALAPPDATA%\GameAgentMonitor`，装 C:/D: 都不散落、卸载删干净。审计发现只剩一个泄漏点：prod 日志走 exe 相对
  `{app}\bin\log`（`backend_init`，Program Files 下标准用户无写权限，白屏同源）—— WebView2/config(settings)/staging
  早已在 appdata。修：`commands.cpp` `backend_init` 日志目录改 `#ifdef DEV_MODE` 分支（dev 保留 `exe_dir\log` 供
  `devprobe.bat`，prod 用 `paths_get_appdata_dir()+"\\log"`）；`open_log_dir`/`clear_log`/`get_log_dir` 读
  `capture_log_get_dir()` 自动跟随。`setup.iss` 加 `[Dirs]`（装机建 appdata 夹）+ `[UninstallDelete]`（卸载递归清
  `{localappdata}\GameAgentMonitor` 全夹 + `{app}` 全夹，兜底 updater 增量残留 DLL/`.old`）。已知限：admin 装时
  `{localappdata}` = 提权账户（单用户 Win11 Home 匹配）。dev+prod build 验证、prod 实测日志落 appdata 无泄漏。
  **并入 0.3.6**，随该版发布。
- **2026-07-11 (update system overhaul + v0.3.5)**: 自动更新三大改造 —— (1) **真增量**：`check_update` 改按
  `sha256` 比对（version.json 每文件已自动算），只下内容变的文件，零手动版本号；(2) **全量兜底**：version.json
  `full_update` 标志 / UI「完整更新」按钮（`force_full`）强制全下；(3) **进度条+活动文字**：`download_update` 移后台
  `std::thread`（即时返回，`g_up.active` 防双击），`winhttp_get` 加 per-chunk `ProgressCb`，`WM_UPDATE_PROGRESS`
  桥到 WndProc→`PostJsonToWebView`（仿 `WM_STREAM_FRAME`），前端 `onUpdateProgress` 订阅 + 复用 `SelfTestModal`
  进度条 markup；每文件 `sha256` 校验（bcrypt，新 `sha256_util`）。**破 updater 自替换死循环**：updater.exe 加改名
  技巧（`MoveFileEx` 自己→`.old` 再拷）+ `--self-install`；monitor_app 首启比对 updater.exe sha 与 version.json，
  不符则拉起 `bin\updater.new`（0.3.5 updater 副本）`--self-install`（monitor_app 仅启动器）。updater 加
  `requireAdministrator` manifest。0.3.3→0.3.5 一跳修好 updater；0.3.5+ 由上一版 updater 自替换，monitor_app 不再参与。
  **注意**：进度条 + updater 破冰需真机验证。发 Gitee v0.3.5。详见 CLAUDE.old.md。
- **2026-07-11 (release pipeline fixes + v0.3.3/v0.3.4 published)**: 修 `build_release.cmd` 在 git-bash 下必挂的三个环境 bug — (1) `NoDefaultCurrentDirectoryInExePath=1` 令 cmd 不搜 cwd → 所有 `call build_x.cmd` 报"不是内部命令"（wrapper 清空该变量）；(2) 各子构建脚本无条件 `call vcvars64.bat` 逐步追加 VS 路径撑爆 PATH 8191 上限 → 第 5-6 步 cmd 静默死（5 个构建步 `call`→`cmd /c` 子进程隔离，PATH 不累积）；(3) `verify_isolated.cmd --auto` 原固定 6s 单次查日志，但 WebView2 首启建 env 耗时 0.4~36s 剧烈波动常错过 `frontend served` 标记，加单实例 mutex + msedgewebview2 文件锁掩盖 → 改：拷包前先 kill 旧实例+webview2、轮询 90s 抓标记、结束清理子进程。隔离验证实打实抓到 `prod: frontend served` + React 启动调用 = 白屏真修。发 Gitee v0.3.3/v0.3.4，raw URL 302→200 校验通过。详见 CLAUDE.old.md。
- **2026-07-11 (white-screen root fix + isolated verify)**: 根治真机白屏 — `CreateCoreWebView2EnvironmentWithOptions` 第2参 `userDataFolder` 原为 `nullptr` → WebView2 默认在 exe 旁（`C:\Program Files\...\bin\`）建数据夹 → 标准用户无写权限 → env 创建失败 → 白屏。改为显式 `LOCALAPPDATA\GameAgentMonitor\WebView2`（恒可写）。env/controller 失败加 `LOG_ERROR`+`MessageBox`（杜绝静默）。修 `paths_get_install_dir` 注册表泄漏：旧 HKLM `InstallPath` 会把 exe 重定向到*上次*安装的 frontend，掩盖打包 bug → 改 exe 相对优先（exe 父目录，叶名==`bin`），注册表降级兜底，只读不写。统一构建产物为发布包结构（`build/{bin,frontend,config}`、`build_dev/bin/`），dev==prod==包，消除相对路径假通过。新增 `verify_isolated.cmd`：拷包到 `%TEMP%\GAM_verify`（repo 外）启动 → Y/N gate；`build_release.cmd` git push 前调用，失败即中止。
- **2026-07-11 (release automation)**: 消除 setup.iss 版本号手动同步（条件 define + ISCC `/D` 传参）。修复 prod-only 白屏双 bug：WinHTTP 302 重定向 + `SetVirtualHostNameToFolderMapping` 路径缺尾 `\`。建立 prod 本地验证步骤（`build_release.cmd` 后先 `release\...\monitor_app.exe` 实测再发布）。发布 v0.3.3/v0.3.4。
- **2026-07-10 (version unification)**: 铁律 8 — 版本号单一真相源 `version.h`。消除 12 个硬编码版本位点，构建脚本自动从 version.h 解析版本，logger.cpp 用 APP_VERSION 宏，App.tsx 运行时 get_version，setup.iss 从 version.h 同步。建立标准化发布流程（dev验证→prod构建→Gitee发布→一键更新）。
- **2026-07-10 (Phase 2)**: Modular DLL build (12 DLLs with VERSIONINFO), InnoSetup installer, settings persistence, multi-file incremental update, paths system, settings auto-save/load
- **2026-07-10 (auto-update)**: Phase 1 full-EXE update — WinHTTP `check_update` (Gitee API) + `download_update` (download + swap.bat + self-replace); UpdateModal + BottomBar indicator; fix GitHub→Gitee link in SettingsView
- **2026-07-10 (gitignore)**: Rewrite `.gitignore` — cover `build_dev/`, WebView2 runtime cache (`*.exe.WebView2/`), nested log dirs, `*.res`, `tmp/`; remove stale Rust `target/` rule
- **2026-07-10 (self-test)**: test_target 判定区缩小(inner hit-margin) + 真实 IME 输入框(EDIT child); TCP self-test 通道(:9998 JSON-lines) — DEV 面板一键映射校准, 复用真实点击回调 sendMappedClick, predict vs 实收 → 命中率热力图/偏移向量/像素误差; 新组件 SelfTestModal + lib/selftest.ts (13 组件)
- **2026-07-10**: Log collapse, CSS rename accent-dev→accent-secondary, MonitorView clear canvas on stop, real-screen cursor overlay (C++ WS_EX_LAYERED UpdateLayeredWindow), self-target detection + exclude toggle, 3-mode input (mouse/keyboard Seize/Semi/Background), WDA_EXCLUDEFROMCAPTURE, desktop input support, test_target EXE, WGC crash fixes (out_ch nullptr, timing)
- **2026-07-09**: Two-color theme + Dev mode, MonitorView remote-control, component decomposition (1→11 files), input mapping
- **2026-07-08**: Method routing 铁律 5 enforcement, stream bridge, SharedBuffer pipeline, log UX
- Earlier: Rust→C++ migration complete, WGC/DXGI capture, MJPEG server, TCP protocol
