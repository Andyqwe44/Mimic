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
│   ├── Release.ps1           # 顶层编排 (替 release.sh + build_release.cmd)
│   └── Read-InstalledLogs.ps1 # 抓「装机版」运行时日志+version.json → tmp/installed-logs/ (调试)
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

> **完整自动更新逻辑 + 踩坑史(6 个坑)+ 已知隐患 → [`docs/auto-update.md`](docs/auto-update.md)**
> （全链路时序、每坑根因/修法/版本、怎么发版 + 怎么测更新链）。首次端到端跑通:0.3.12→0.3.13。

### 发布包结构（`Release.ps1` 组装）

```
release/GameAgentMonitor/
  bin/          monitor_app.exe · updater.exe · updater.new · 12 DLL (logger + capture×6 + input×5)
  frontend/     dist (index.html, assets/)          ← npm run build
  config/       settings.default.json
  version.json  schema v2 { schema, app, released, channel, min_version, mandatory, message,
                full_update, download_base, updater{path}, sig, files{ "bin/x.dll":{v,sha256,size}, ... } }
release/GameAgentMonitor_Setup_v<ver>.exe           ← Inno Setup installer → %ProgramFiles%\GameAgentMonitor
```

**增量更新下载源 = git raw URL,不是 Release 附件。** Release 附件(setup.exe)只给全新装机;增量更新逐文件从
`download_base + <path>`(= `raw/<tag>/release/GameAgentMonitor/<path>`,git 仓库文件)拉 + sha256 校验。所以
`release/GameAgentMonitor/` 必须 commit——它本身就是「增量货」。`download_base`(schema v2)= 服务端可换下载源不重编客户端。

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
| `monitor_app/app.rc` | `#include "src/version.h"` → `APP_VERSION` / `APP_VERSION_RC`（唯一嵌 app 版本的原生产物）|
| `logger` 运行时 banner | monitor_app `capture_log_init("agent", APP_VERSION, …)` **运行时传参**（DLL 字节不含版本）|
| `installer/setup.iss` | `Release.ps1` 读 version.h → `ISCC /DMyAppVersion=<ver>`，setup.iss 条件 define |
| 前端 `App.tsx` | 运行时 `hostCall('get_version')`；构建时 `vite.config` 读 version.h → `__APP_VERSION__`（splash 秒显） |
| `scripts/New-VersionJson.ps1` | `Get-AppVersion`（读 version.h）→ version.json |

**⚠️ 原生 lib/DLL（logger/capture/input，共 12 个）版本已与 APP_VERSION 脱钩**（真增量更新，见 changelog
2026-07-13）：其 VERSIONINFO 用 `Build.ps1` 的独立 `$LibVer`（模块版本，**改 lib 源码才手动 bump**），
配 `/Brepro` 确定性 PE → app 版本 bump 不改 lib 字节 → 发版只重下 `monitor_app.exe`。**勿**把 lib 版本改回读
version.h（会退回「每版全量下载」的假增量）。

**改版本号步骤：**
1. 编辑 `monitor_app/src/version.h`：修改 `APP_VERSION` 和 `APP_VERSION_RC`
2. 重建 app（`Release.ps1` 自动读新版本；lib 版本无关，除非改了 lib 源码则先 bump `$LibVer`）
3. 前端 `npm run build`（JS hash 自动更新）
4. 组装 release 目录 + 生成 `version.json` + ISCC 打包

**严禁：** `sed` 批量替换版本号。之前 12 个硬编码位点已全部消除。

### 铁律 7: CLAUDE.md 保持精简，详细内容写入 CLAUDE.old.md

CLAUDE.md 只放核心规则、架构概览、构建命令。以下内容**必须写入 CLAUDE.old.md**：
- 开发日志 / Recent Fixes 详细描述
- 指令的详细说明和背景故事
- 历史变更的完整记录

CLAUDE.md 只保留摘要和指向 CLAUDE.old.md 的引用。

### 铁律 9: 跨进程命令行参数 — 禁止引号嵌入路径

**背景**:`ShellExecuteEx` + `lpParameters` 会把参数字符串传给新进程的 `lpCmdLine`。如果
调用方把路径用双引号包了(如 `"\"C:\\...\\staging\" 12345"`),新进程的 `strtok`/`__argc` 不会
自动剥离引号 —— **引号会变成路径的一部分**,导致 `FindFirstFileA`/`CreateDirectoryA` 失败。

**规则**:

| 规则 | 说明 |
|------|------|
| 9a. 参数里**不加引号** | 调用 `ShellExecuteEx` / `CreateProcess` 时,`lpParameters` **不对路径加引号**。
| 9b. 被调进程**必须剥引号** | `updater` / 任何接收命令行的进程:`strtok`/解析参数后,立即 `unquote` 两端双引号(防御调用方忘记规则 9a) |
| 9c. 测试「路径带空格」场景 | `Program Files` / `Andyq` 本来没空格,但不能依赖于此;本地模拟「Program Files」= 含空格的文件夹名,确保全链不炸 |

**斜杠规则(Windows)**:

| 规则 | 说明 |
|------|------|
| `\\` 单反斜杠 | 字符串中拼接路径:`"C:\\dir\\file"` 或 `R"(C:\dir\file)"`(raw literal) |
| `/` 正斜杠 | API 调用 Windows 接受 `/` 和 `\\` 等效;传给 `ShellExecute` / `WebView2` 的 URL 只用 `/` |
| 拼接路径不用 `+ "\\" +` | Windows API `FindFirstFileA`/`CreateDirectoryA`/`CreateFileA` **不接受尾随反斜杠的目录路径**(如 `"C:\\dir\\"` 不行,但 `"C:\\dir\\*"` 可以) |
| 路径连接用 `snprintf(buf, sz, "%s\\%s", dir, name)` | 别手拼 `dir + "\\" + name`,特别是指针叠加时容易多出反斜杠或缺少反斜杠 |

简洁记忆:
- 跨进程传参:**不加引号,被调剥引号**
- 拼路径:**一个 `snprintf("%s\\%s", a, b)`,不手拼`+`**
- 尾反斜杠:**除 `"\\*"` 外,目录路径不准以 `\\` 结尾**

---

## Changelog

Full development history preserved in `CLAUDE.old.md`. Major milestones:
- **2026-07-13 (0.3.29 Check Update 必弹窗 — 已最新/检查中/出错也弹, 不只 log)**: 用户诉求(符合直觉):点
  Check Update **无论有无更新都弹弹窗**——已是最新也弹「当前已是最新版本 vX」,而非只在 log 留一条不醒目提示。
  `UpdateModal.tsx` 加 `status:'checking'|'update'|'latest'|'error'`(缺省'update'兼容):header 标题/图标随态变
  (检查更新+Loader2转 / 发现新版本+Download / 已是最新+CheckCircle2 / 检查失败+AlertTriangle);checking=居中 spinner
  「正在检查更新…」;latest=居中绿勾「当前已是最新版本 v{current}」+「知道了」;error=居中警告+错误文案。**尺寸策略**:
  `update` 态固定高 `min(560,85vh)`(防下载跳);`checking/latest/error` 态**自适应高**(去 min-h 不留空)。进度槽仅
  `update` 态渲染。footer 按态切:checking=取消/latest·error=知道了/update=稍后+全量+增量。`App.tsx checkForUpdate`
  重写:点击**立即** `setUpdateInfo({status:'checking'})` 弹窗 → `await check_update` → 按结果切
  `update`/`latest`/`error`(needs_full_installer→error 带「下完整包」文案)。`hasUpdate` 指示器判据改
  `updateInfo?.status==='update'`(否则 checking/latest/error 误点亮"有更新"红点)。纯前端。
  **注**:版本号留 0.3.x(核心视觉 AI 未实现, 远未到 1.0),把更新逻辑写完善;`version_lt` 数值分段比较、min_version
  固定基线 0.3.24、`$LibVer` 脱钩 → bump 只改 version.h 两行, 发布流程零风险。
- **2026-07-13 (0.3.28 更新弹窗重设计 — 对齐 Select 尺寸 + 可折叠 diff 双列 + 图标按钮)**: 用户要求重做
  Check Update 弹窗(`UpdateModal.tsx`,仅前端,App.tsx props 不动)。(1) **尺寸对齐**:`w-[420px]`→`w-[520px]`
  + 固定高 `min/max-h=min(560px,85vh)`(= `TargetPickerModal` 同款),body `flex-1 overflow-y-auto` 内滚 →
  窗口尺寸恒定。(2) **进度条预留**:body 与 footer 间**常驻 `h-[52px]` 进度槽**,idle 空占位、下载中填充 →
  加进度条元素不跳。(3) **图标按钮**:「增量更新」(`FileStack`)/「全量更新」(`Package`);`mode==='full'`(服务端/
  min_version 强制全量)时主按钮自动变「全量更新」并隐藏重复副按钮。(4) **可折叠 diff**(铁律5 画=实发):折叠态
  `本次更新 · N 个文件` + 右侧总解压/总流量;展开逐文件 `友好名徽章(w-14,核心文件 accent 色) → install 根相对
  路径(truncate) → 解压 → 流量`。`fileRole()` 映射 path→功能名(主程序/更新器/清单/日志/捕获/输入/界面/配置)。
  (5) **列对齐**:折叠条两列(w-20)+ chevron(w-5 gutter);文件行两列 + **等宽 w-5 空槽** → 数字列右边界对齐、
  chevron 恒在最右。chevron 用 **caption 占位**(上方 `&nbsp;` 空行)下移到 number 行 → 与解压/流量数字中线对齐。
  (6) **双列(大小/流量)语义**:`size`=解压磁盘占用,`dl`=压缩后下载流量(**新增可选字段,给将来压缩下载预留**);
  当前逐文件裸下载**无压缩** → `dl` 缺省 → `traffic()` 回退 `size` → **两列当前数值相同**(非 bug,version.json
  无文件级时间字段;后端产出 `dl` 后自动分离)。全量更新逻辑=增量的无 sha 过滤版(同一条逐文件管道,非重装)。
  前端 tsc 编译通过。
- **2026-07-13 (0.3.27 更新体验 — 首启自愈清单根治滞后 + 下载前 diff 预览)**: 0.3.25→0.3.26 实测下 17 文件
  (非预期 ~5),读装机日志锁定:`full=0`(P0 生效!)但 `install version.json` 冻结在 **0.3.14**(用户从 0.3.14
  全新装后一路增量,旧 updater 从不刷新它)→ check_update 用 0.3.14 旧 lib sha 比 0.3.26 脱钩 sha → 12 lib 全
  误判为变 → 17。**根治**:local 基准清单移 **appdata**(`monitor_app` asInvoker 写不了 Program Files)+
  `main.cpp` `heal_local_manifest`:首启若 `appdata\version.json` 的 app≠APP_VERSION,遍历 install
  `bin`/`frontend`/`config` 真实文件算 sha256 重建 → 清单永远跟当前 exe 一致,**不靠 updater、不重下未变文件**。
  check_update + check_and_heal_updater 均改读 appdata(回退 install)。**diff 预览**(用户需求):check_update 本就
  返回 diff(每文件 path+size),`UpdateModal` 下载前渲染「N 个文件 · 总大小」+ 逐文件清单(铁律 5 画=实发)。
  **滞后**:0.3.27 是「装上自愈码」的最后一版滞后——0.3.26→0.3.27 仍下 17(0.3.26 无自愈码),装上 0.3.27 后
  appdata 清单自愈;**0.3.27→0.3.28 起真增量 ~5 + 预览生效**。全量编译过。
- **2026-07-13 (0.3.25 更新系统企业级改造 — P0 真增量 + P1 删除同步 + P2 ECDSA 签名)**: 0.3.23→0.3.24 更新仍
  「下 21 文件」(全量),排查确认**非**脱钩失败(脱钩已实测生效),真因 `New-VersionJson.ps1` 默认
  `$MinVersion=$Version` + `commands.cpp` `current<min_version→强制 full` → 每版 min_version=新版 → local 永远
  小于它 → 永远全量,sha 增量比对根本没走。审计另发现三缺陷。**三阶段全套修复**(用户选企业级):
  **P0 真增量**:`New-VersionJson.ps1` 默认 `$MinVersion='0.3.24'` 固定基线(只在更新机制不兼容时提升,不随版本走)
  → 0.3.24+ 客户端走 sha 增量,diff 只含真变的文件。
  **P1 desired-state(删除同步 + 清单刷新)**:`download_thread_func` 下完 diff 后从首个 file 的 url 反推
  download_base,下 `version.json` 进 staging(updater copy_staging 天然拷到 install → **清单刷新即时**);
  `updater.cpp` copy_staging 后加 `sync_deletions`:读刚拷的 install `version.json` 的 files 全集,遍历白名单
  `bin`/`frontend` 删清单外文件(保护 updater/monitor_app/`.old`/log;空清单则跳过,绝不「空清单删全部」)。
  解决前端 `index-XXXX.js` hash 重命名的旧 asset 无限累积。**delete 滞后一版生效**(0.3.25 装上新 updater,
  0.3.25→0.3.26 才 delete;清单刷新即时)。
  **P2 ECDSA P-256 签名**(CNG/BCrypt,**非** Ed25519——CNG 不原生支持):`New-SigningKey.ps1` 一次性生成密钥
  (私钥 `scripts/.signing/ec_priv.b64` gitignore,公钥 `BCRYPT_ECCPUBLIC_BLOB`→`monitor_app/src/update_pubkey.h`
  嵌入);`New-VersionJson.ps1` 对 files 规范化摘要(**ordinal 排序** `path\nsha256\n` 拼接 → SHA256)签名填
  `sig`;`update_verify.cpp`(新)`BCryptVerifySignature` 验签;`check_update` 灰度接入(sig 空→WARN 跳过兼容旧版;
  非空→必验过否则 `ok:false` 拒绝,铁律 5)。**round-trip 实测**(`tmp/verify_p2.ps1`):签 21 files→C++
  `signed=1 verify=1`;篡改 sha→`verify=0` 拒绝。双端 payload 字节一致(独立 ordinal 排序,避 JSON 规范化)。
  **生效**:0.3.25 客户端起验签;0.3.24(无验签码)忽略 sig 字段,0.3.24→0.3.25 安全。全量编译通过。**待发 0.3.25**
  验真增量(0.3.24→0.3.25 预期只下 ~6 文件:monitor_app.exe + updater×2 + 前端×3,12 lib + logger 不下)。
- **2026-07-13 (DLL 版本与 APP_VERSION 解耦 — 真增量更新)**: 痛点:每次 bump APP_VERSION,12 个原生
  DLL 的 VERSIONINFO 都嵌 app 版本 → sha256 全变 → 「增量更新」每版仍重下全部 DLL(假增量)。**根因三层**:
  (1) `capture`/`input` 的 `link.exe` **漏了 `/Brepro`**(logger/app/updater 有)→ 非确定 PE 时间戳,同源码每
  次 link 字节都变;(2) `New-VerModuleHeader` 给每 DLL 的 VERSIONINFO 写 `GAM_RC_STR=$Ver`(=APP_VERSION);
  (3) `logger.cpp` ring banner 硬编码 `APP_VERSION` 宏(还和 file banner 用传入 `app_version` 参数不一致=小 bug)。
  **修**:① `Build.ps1` 引入 `$LibVer`(模块版本,`logger`/`capture`/`input` 的 VERSIONINFO 用它,与 APP_VERSION
  脱钩;改 lib 源码才 bump)+ capture/input link 补 `/Brepro`;② `logger.cpp` banner 改用传入 `app_version`
  参数(monitor_app `capture_log_init("agent", APP_VERSION, …)` 运行时传,DLL 本身不含版本);③ `New-VerModuleHeader`
  删 `#define APP_VERSION`。**双重实测验证**(`tmp/verify_*.ps1`):同源码连编两次 → 12/12 DLL sha256 一致
  (`/Brepro` 生效);临时 bump version.h 0.3.23→9.9.9 重编 → 12/12 lib DLL **字节不变**(脱钩),version.h 自动恢复。
  **效果**:此后发版增量货 = `monitor_app.exe`(+前端若改),12 lib 冻结;`version.json` 逐文件 sha 比对天然只
  下变的文件,`check_update` 零改动。**一次性迁移成本**:下次发版这 12 lib 因构建方式变(去宏/`$LibVer`/`/Brepro`)
  相对线上会变一次,之后永久冻结。commit `f1fa213`(未发,随下个版本发布)。
- **2026-07-13 (0.3.23 降级终于通 — Raymond Chen explorer-shellview)**: 0.3.22 `CoCreateInstance(CLSID_Shell)`
  实测炸 `0x80040154 REGDB_E_CLASSNOTREG`——CLSID_Shell 只注册 InProcServer32,无 LOCAL_SERVER;且 in-proc Shell
  对象跑在**自己 High IL**,`ShellExecute` 出来仍是管理员,根本不降级。**正解**(`commands.cpp`
  `shell_execute_via_explorer`):不建新 Shell,伸进**已在跑的桌面 explorer(恒 Medium IL)**要它 ShellExecute →
  子进程继承 Medium。链:`CoCreateInstance(CLSID_ShellWindows)`→`FindWindowSW(CSIDL_DESKTOP,SWC_DESKTOP,
  SWFO_NEEDDISPATCH)`→`QI IServiceProvider`→`QueryService(SID_STopLevelBrowser→IShellBrowser)`→
  `QueryActiveShellView`→`GetItemObject(SVGIO_BACKGROUND→IDispatch)`→`QI IShellFolderViewDual`→`get_Application`
  →`QI IShellDispatch2`→`ShellExecute(exe,"","","open",SW_SHOWNORMAL)`。每步各自 LOG_ERROR+hr 便于定位。
  **编译坑**:`<shellapi.h>` `#define ShellExecute ShellExecuteA` 宏污染,把 `psd->ShellExecute` 改写成
  `psd->ShellExecuteA`(IShellDispatch2 无此成员,C2039)→ 函数前 `#undef ShellExecute`(显式 `ShellExecuteExA`
  不受影响)。头加 `exdisp.h`/`shldisp.h`/`servprov.h`,libs 不变(GUID 走 uuid.lib)。**实测通过**:0.3.23
  管理员态点「普通」→ 旧进程退、新进程起、任务管理器/徽章确认 Medium。降级历经 7 版(0.3.16—0.3.23)终结。
- **2026-07-12 (0.3.13 权限切换重启修复 — 单实例锁)**: 0.3.12 权限切换现象:逻辑全对(flag/token 提权降级都对,手动
  双击能以正确身份开),但**程序自己拉不起新窗口**(点了就关、不重开)。根因:`cmd_switch_permission` 启新进程后立即
  `PostMessage WM_CLOSE`,旧进程还没退、**单实例 mutex 还占着** → 新进程撞单实例守卫 `return 2` 自杀(手动双击时旧进程
  已退、mutex 释放 → 能起)。修:mutex 存全局 `g_singleton_mutex` + `app_release_singleton()`(`main.cpp`),
  `cmd_switch_permission` relaunch 前先释放 → 新进程能起。发 0.3.13,兼作 0.3.12→0.3.13 更新链测试目标。
- **2026-07-13 (0.3.16—0.3.22 降级 debug 马拉松 + 终极 IShellDispatch)**: 管理员→普通降级反复试了 6 个版本——0.3.16
  (`SetTokenInformation` 连续缓冲区,编译过但降 IL 未生效)、0.3.17-0.3.18(`TokenLinkedToken`+`DuplicateTokenEx`
  →主,err=1346 `ERROR_BAD_IMPERSONATION_LEVEL`)、0.3.19-0.3.20(CreateProcessAsUserW+CreateProcessWithTokenW
  双保险,双 1346)、0.3.21(`DuplicateToken` 升身份模拟级别→`DuplicateTokenEx`→仍 1346,**TokenLinkedToken 本质
  不能转主 token**)。**0.3.22 终极方案 `IShellDispatch` COM**:`CoCreateInstance(CLSID_Shell)`→`IDispatch::Invoke
  (ShellExecute)` —— 通过 explorer.exe(始终 Medium IL)的 COM 自动化代理启动,explorer 创建子进程 = 普通权限。
  Token 操作(~70 行)全删,~30 行 COM 替代。这是 Inno Setup / Visual Studio / Chrome 的标准做法。附带:
  双 UAC 防重入(`update_launch_updater` static guard)、`/Brepro` 确定性 PE 时间戳、深灰页静态控件提示、
  installer 自动清 `RUNASADMIN`、`updater.log` 自写日志、铁律 9(跨进程参数引号+斜杠规范)。**待测:0.3.22 降级**。
- **2026-07-12 (0.3.15 降级失败修复 + 深灰兜底提示 + 引号 bug 铁律 9)**: 0.3.12→0.3.13 更新链覆盖环节失败——下载+sha 全过,但 updater
  `copy_staging` 拷贝 0 文件,staging 23 文件完好(未被删)。**根因**:`update_launch_updater` 给 staging 参数加了
  双引号 → `strtok` 把引号当路径一部分 → `FindFirstFileA` 非法 → 0 文件。手动不引号跑 updater 验证成功。**修**:
  `commands.cpp` params 不加引号(铁律 9a);`updater.cpp` `unquote` 剥两端双引号防御(铁律 9b);CLAUDE.md 加
  **铁律 9**(跨进程参数 + 斜杠规范)。发 0.3.14,用户 0.3.13→0.3.14 更新链再测。
- **2026-07-12 (0.3.13 更新链端到端跑通 + 权限切换修复)**: 权限切换单实例锁修复(`app_release_singleton`
  + `app_acquire_singleton` UAC 拒绝重抓)+ updater.log 自写日志 + `docs/auto-update.md` 权威文档。
  0.3.12→0.3.13 下载/sha 全通,手动跑 updater 验证覆盖成功。发 0.3.13。
- **2026-07-12 (0.3.12 更新链最后一环:updater 提权 + install 定位)**: 0.3.10→0.3.11 首次跑到 download 末段——21 文件
  下载 + sha256 校验**全通过**,卡在启 updater:`update_launch_updater: CreateProcess failed err=740`
  (ERROR_ELEVATION_REQUIRED)。根因:`updater.exe` 是 requireAdministrator(覆盖 Program Files),`CreateProcess` 从
  非提权进程起不了提权 exe。**修**:`ShellExecuteExA` + `runas` 弹 UAC 提权。**顺带审 updater**:原只读注册表
  `HKLM\SOFTWARE\GameAgentMonitor\InstallPath` 定位 install(缺则失败)→ 改 **exe-relative 优先**(updater 在
  `<install>\bin\`,install=父父),注册表 fallback,与 monitor_app `paths_get_install_dir` 一致。**已知隐患(非阻断)**:
  staging 不含 version.json,updater 不更新 install\version.json → 陈旧;当前 min_version=self 每次强制 full 掩盖
  (不增量但能更新)。**另加功能3 运行权限切换**(Settings General「运行权限」普通/管理员 + 当前徽章):`get_elevation`
  查当前进程 IL;`switch_permission {admin}` 写/删 `AppCompatFlags\Layers` RUNASADMIN 持久(下次双击按上次选择自动提权/普通)
  + 重启到目标权限——升 `ShellExecuteEx runas`(UAC),降 `DuplicateTokenEx`+`SetTokenInformation`(Medium IL)+
  `CreateProcessAsUser`(**自包含,不依赖 explorer**)。`Build.ps1` 加 `advapi32.lib`。发 0.3.12,手动装,测 0.3.12→0.3.13。
- **2026-07-12 (0.3.11 更新链验证版)**: 纯 bump,作 0.3.10→0.3.11 一键更新全链的测试目标——验证 download diff 转义
  修复 + 完整下载/sha256 校验/updater 覆盖/重启。无功能改动。
- **2026-07-12 (0.3.10 更新下载 diff 双重转义修复)**: 0.3.8→0.3.9 首次真跑 download,暴露隐藏 bug——check_update
  正确算出 21 文件 diff,`download_update` 却报「no files to download」。**根因**:前端
  `hostCall('download_update',{diff:JSON.stringify(diff)})` 把 diff 数组**双重 JSON 编码**,里层引号被转义成 `\"`;
  后端 dispatch bracket-match 提取到 `[{\"path\":...}]`(带反斜杠),`cmd_download_update` 的 `find("\"path\"")` 找真引号
  找不到 → totalFiles=0。这是 download **第一次**被执行(0.3.5-0.3.8 全挂在 check_update 之前),故一直没暴露。**修**:
  (1) 前端 `{ diff }` 不双重 stringify;(2) 后端 dispatch 提取后反转义 `\"`→`"`(兜底,对干净 array 是 no-op)。发 0.3.10。
  **注意**:装的 0.3.8 的 download 逻辑坏,须**手动装 0.3.10**;之后 0.3.10→0.3.11 才能测通完整更新链。
- **2026-07-12 (0.3.9 骨架屏预览开关 + 窗口/任务栏图标修复 + 换logo工具)**: (1) Settings 开发人员区加「预览骨架屏 (3s)」
  按钮(方案A:点一下盖 3 秒自动消失,规避「全屏遮罩关不掉」)；`App.tsx` `previewSkeleton` state,骨架屏渲染改
  `(!appReady || previewSkeleton)`。(2) **图标修复**:`main.cpp` WNDCLASS 加 `hIcon`/`hIconSm`(按 `GetSystemMetricsForDpi`
  DPI 尺寸 `LoadImageW`,**避开会加载失败→fallback 16px 的 256**)+ 建窗后 `WM_SETICON`。**关键洞察**:任务栏/Explorer
  图标来自 **exe 资源图标(`app.rc`→`app.ico`),不是 window HICON** —— 改 window icon 对任务栏无效;真因是 app.ico 的
  logo 只占画布 60%(留白多)→ 显小一圈。修:`tools/make_app_icon.py`(PIL crop 去留白→居中填满~90%→多尺寸
  16/20/24/32/48/64/128/256,24=Win 任务栏原生)重生成 app.ico→重编→清 icon cache(`ie4uinit -show` 或重启 explorer
  删 `iconcache*.db`)。**换 logo 流程见 `tools/make_app_icon.py` 头注释 + memory app-icon-howto。** (3) `Build.ps1` 全 cl
  加 `/source-charset:utf-8` 消 C4819 刷屏(验证 count 0)。发 0.3.9,兼作 0.3.8→0.3.9 更新链测试目标。
- **2026-07-12 (0.3.8 真机深灰卡死修复 + installer 运行检测)**: 0.3.7 真机装上打不开——窗口隐藏后揭开是**深灰空窗**,
  无骨架屏无主 UI。**根因**(读装机日志锁定,`Read-InstalledLogs`):隐藏窗口 → Chromium 合成器暂停 → 前端
  `requestAnimationFrame` 回调**永不触发**(`setTimeout`/命令照跑,故 get_settings 发得出)→ 前端 `rAF→rAF→show_window`
  揭窗信号卡死 → 靠 8s 兜底揭窗;且揭窗只 `ShowWindow`,没通知 WebView2 重画 → controller 停在隐藏期空白帧 → 深灰。
  此 bug 由 0.3.6 引入隐藏窗机制时带入,0.3.7 继承(0.3.7 只改更新逻辑没碰启动)。**修**(`main.cpp`):(1) 揭窗信号改用
  C++ `NavigationCompleted` 事件(日志证 t+2.4s 可靠触发),不再依赖跑不了的前端 rAF;(2) `show_main_window` 揭窗后
  `put_IsVisible(TRUE)`+重设 `put_Bounds` 强制重画。`App.tsx` `SPLASH_TEST_MS`→0。实测 `window shown` 从 8031ms→2437ms
  且无 8s 兜底告警,用户肉眼确认 UI 正常。**installer**:`setup.iss` 加 `AppMutex=Global\GameAgentMonitor_8A3F2D`(Inno
  内置,复用 prod 单实例锁)——装/卸载时程序在跑则弹标准提示要用户先关闭;旧版升级走 AppId 自动覆盖(内置)。未做
  「检测到旧版弹框」(需手写 `[Code]`,非必要)。发 0.3.8,用户手动装(0.3.7 UI 坏没法自更新)。
- **2026-07-12 (0.3.7 更新系统企业化改造 + 装机日志调试工作流)**: 治「0.3.5→0.3.6 更新升不上去」——症状:弹窗提示可升
  0.3.6,点更新却「无需升级」(矛盾)。**根因(源码+实测锁定,铁律 5 违背)**:`check_update` 里 `hasUpdate` 由版本串
  独立算,`diff` 由拉取 `raw/<tag>/version.json` 逐文件比对算 → 一旦远端清单拉取失败(CDN 传播延迟/瞬断/302),
  `winhttp_get` **把非 2xx 的错误页当数据返回**,`find("\"files\"")` 落空 → `diff=[]`,而 `hasUpdate` 仍 true → 前端
  「有更新但 0 文件」=「无需升级」的假象。**服务端无辜**:curl(`GAM/1.0` UA)+ WinHTTP COM 同栈实测均 200+合法
  JSON;全盘扫描证实装机已被卸载(无日志可读),故直接**复现网络路径**代替读日志定位。**P1 修静默**(`commands.cpp`):
  (1) `winhttp_get` 非 2xx → `LOG_WARN`+返回空(不再把错误页当数据);(2) `check_update` 清单拉取加 3 次重试
  (治传播延迟),空/无 `"files"` → 返回 `{ok:false,error}` 而非 `has_update+空 diff`(前端已有 `ok===false` 分支报错);
  (3) 富日志(清单长度/HTTP 码/diff 数/`hasUpdate 但 0 diff` 告警)。**P2 服务端可控更新(manifest schema v2)**:借
  Sparkle/Omaha/Tauri 通行做法——**策略放服务端 manifest,客户端只留「打不死的最小引导」**。`New-VersionJson.ps1` 产出
  `schema:2 / channel / min_version / mandatory / message / download_base / updater / sig`(全向后兼容,旧端只读 `files`)。
  客户端(`check_update`)遵:`schema>2` → 优雅提示「下完整包」(`needs_full_installer`);`download_base` 拼下载 URL
  (**换主机/仓库/CDN 免重编客户端**);`min_version` 高于当前 → 强制 full;`mandatory`/`message` 透传前端。bootstrap 铁律:
  只有「拉 manifest→校验→启 updater」不能坏(服务端够不着,0.3.5 正死于此)。**P3 dev 测试通道**:环境变量
  `GAM_UPDATE_TAG` 指定任意 tag、跳过 releases API → 不发公开版即可测更新链。**前端**(铁律 6):UpdateModal 显 `message`、
  `mandatory` 藏「稍后」+ 禁关、`mode=full` 提示完整更新;`checkUpdate` 处理 `ok:false`/`needs_full_installer`。**签名**
  (`sig`):0.3.7 只预留字段,Ed25519 验签留 0.3.8+(验签坏=更新全炸,先稳机制)。**新增调试工作流**
  `scripts\Read-InstalledLogs.ps1`:定位装机目录(注册表→Uninstall→扫盘)→ 抓 `{install}\bin\log` + `%LOCALAPPDATA%\...\log`
  最新日志 + 双 version.json → `tmp/installed-logs/`(gitignore),让 Claude 读装机版真日志排查。`Release.ps1 -DryRun` 全链过
  (VERIFICATION PASSED + version.json 21 文件 schema v2)。**发 0.3.7**;用户须**全新装 0.3.7**(0.3.5 updater 已坏且已卸载),
  之后 0.3.7→未来走新逻辑。测更新链需发 0.3.8(或用 `GAM_UPDATE_TAG` dev 测)。
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
