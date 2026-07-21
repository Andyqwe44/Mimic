# CLAUDE.md — Mimic (visual game AI / peer control)

> **2026-07 目录重整：** `pc/`（Windows）· `server/` · `android/`（骨架）· `shared/web`+`shared/protocol`。
> UI 共用 WebView；Android = Capacitor（非 Electron）。发版与路径以 [README.md](README.md) 为准。

## ⛔ 思想钢印 — 十二条铁律

### 铁律 1: 中文思考回答

用中文思考和回答。代码、commit 信息、PR 描述用英文。

### 铁律 2: 日志只用 LOG()

**项目已配备统一日志系统。严禁使用任何裸打印函数。**

以下符号**不得出现**于 `pc/logger/logger.cpp` 以外的任何 C++ 文件中：

```
printf          fprintf         fprintf(stdout     fprintf(stderr
std::cout       std::cerr       std::clog
puts            putchar         fputs              fwrite(..., stdout
WriteConsole    OutputDebugString
```

**唯一例外**：`pc/logger/logger.cpp` 自身。

**唯一合法方式**：

```cpp
#include "logger/logger.h"  // from pc/logger
LOG_ERROR("tag", "format", args...);  // 错误 — operation failed, must fix
LOG_WARN("tag", "format", args...);   // 警告 — fallback used, retryable
LOG("tag", "format", args...);        // INFO — status change, user action
LOG_DEBUG("tag", "format", args...);  // DEBUG — frame detail, param dump (dev only)
```


| 标签           | 用途        |
| ------------ | --------- |
| `wgc` `dxgi` | 捕获        |
| `cmd`        | 命令调度      |
| `main`       | 主循环/启动    |
| `mjpeg`      | MJPEG 服务器 |
| `ui`         | 前端事件      |
| `agent`      | AI Agent  |


**日志等级规范（四级）：**


| 等级    | 值   | 宏                      | 场景                   | Dev | Prod |
| ----- | --- | ---------------------- | -------------------- | --- | ---- |
| DEBUG | 0   | `LOG_DEBUG()`          | 帧级细节、参数dump、性能计时     | ✅   | ❌    |
| INFO  | 1   | `LOG()` / `LOG_INFO()` | 状态变更、用户操作、正常流程       | ✅   | ✅    |
| WARN  | 2   | `LOG_WARN()`           | 可恢复问题、fallback、retry | ✅   | ✅    |
| ERROR | 3   | `LOG_ERROR()`          | 硬错误、操作失败             | ✅   | ✅    |


Dev 启动时自动 `capture_log_set_level(LOG_LEVEL_DEBUG)`，prod 设 `LOG_LEVEL_INFO`。
文件格式: `[12:34:56.789] [INFO ] [tag] message`（`%-5s` 列对齐）。
JSON notify 含 `"level":"INFO","lvl":1` 字段供前端颜色区分。

### 铁律 3: 存档 = 更新 README + 更新 CLAUDE.md + commit



### 铁律 4: Tooltip 只用自定义组件

**禁止原生 HTML** `title` **属性。** 只用 `<Tooltip text="...">` 包裹。

自定义 Tooltip：300ms 延迟、Portal 到 body、智能定位、统一外观。

### 铁律 5: 禁止欺骗 — 后端不骗前端，前端不骗用户

**C++ 层不得对前端透明地修改行为。** 前端命令必须原样执行——成功返回数据，失败返回 error。


| 规则           | 说明                                                |
| ------------ | ------------------------------------------------- |
| 5a. 不静默修改参数  | 前端根据目标选择方法，C++ 只执行                                |
| 5b. 必须检查返回值  | SendInput/PostMessage/GetClientRect 等失败必须返回 error |
| 5c. 前端反馈匹配实际 | 画了什么 = 实际发了什么。没发的别画，发了的别藏                         |


**简洁记忆：**

- **C++ → TS：** `{"ok":false, "error":"..."}` 比静默 `{"ok":true}` 好一万倍
- **TS → 用户：** 画了什么 = 实际发了什么



### 铁律 6: 前端交互优化 = 状态转换表 → 确认 → 改码 → 同步 README

当用户说"前端交互方案"、"交互优化"等词语时：

1. **先分析** — 阅读当前逻辑，理解状态机
2. **给状态转换表** — `| # | 当前状态 | 事件 | 新状态 | 原因 |`
3. **等待确认** — 用户说"确认"或"开始"后才动手
4. **改代码** — 精确修改，不改表外逻辑
5. **同步 README** — 见铁律 12；表变更必须写入根 [README.md](README.md)「状态转换表」章节

### 铁律 7: CLAUDE.md 保持精简，详细内容写入 [CLAUDE.old.md](http://CLAUDE.old.md)

CLAUDE.md 只放核心规则、架构概览、构建命令。以下内容**必须写入 CLAUDE.old.md**：

- 开发日志 / Recent Fixes 详细描述
- 指令的详细说明和背景故事
- 历史变更的完整记录

**Cursor 上下文**：可执行细则在 `.cursor/rules/*.mdc`（`project-core` alwaysApply；`cpp-backend` / `monitor-web` / `build-release` 按 glob）。与本文件互补，不替代铁律。

### 铁律 8: 版本号单一真相源 — `version.h`

**PC Client：**`pc/client/src/version.h`**。** Server：`server/package.json`。Android：`android/version.json`。其余 PC 消费者自动继承：


| 消费者                           | 继承方式                                                                            |
| ----------------------------- | ------------------------------------------------------------------------------- |
| `mimic_client/app.rc`         | `#include "src/version.h"` → `APP_VERSION` / `APP_VERSION_RC`                   |
| `logger` 运行时 banner           | mimic_client `capture_log_init("agent", APP_VERSION, …)` **运行时传参**              |
| `installer/setup.iss`         | `Release.ps1` 读 version.h → `ISCC /DMyAppVersion=<ver>`                         |
| 前端                            | 运行时 `hostCall('get_version')`；构建时 `vite.config` 读 version.h → `__APP_VERSION__` |
| `scripts/New-VersionJson.ps1` | `Get-AppVersion`（读 version.h）→ version.json                                     |


**⚠️ 原生 lib/DLL（logger/capture/input，共 12 个）版本已与 APP_VERSION 脱钩**（真增量更新）：
其 VERSIONINFO 用 `Build.ps1` 的独立 `$LibVer`（模块版本，**改 lib 源码才手动 bump**），
配 `/Brepro` 确定性 PE → app 版本 bump 不改 lib 字节 → 发版只重下 `mimic_client.exe`。
**勿**把 lib 版本改回读 version.h（会退回「每版全量下载」的假增量）。

### 铁律 9: 跨进程命令行参数 — 禁止引号嵌入路径

**背景**: `ShellExecuteEx` + `lpParameters` 把参数字符串传给新进程的 `lpCmdLine`。如果调用方把路径用双引号包了（如 `"\"C:\\...\\staging\" 12345"`），新进程的 `strtok`/`__argc` 不会自动剥离引号——**引号会变成路径的一部分**，导致 `FindFirstFileA`/`CreateDirectoryA` 失败。


| 规则                | 说明                                                                 |
| ----------------- | ------------------------------------------------------------------ |
| 9a. 参数里**不加引号**   | 调用 `ShellExecuteEx` / `CreateProcess` 时，`lpParameters` **不对路径加引号** |
| 9b. 被调进程**必须剥引号** | `updater` / 任何接收命令行的进程：解析参数后立即 `unquote` 两端双引号                     |
| 9c. 测试「路径带空格」场景   | 本地模拟含空格文件夹名，确保全链不炸                                                 |


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



### 铁律 11: 改完直接发版 — 发行版验收

功能/修复合入后**默认** bump 版本并跑 `scripts\Release.ps1`（按改动端用 `-ClientOnly` / `-AndroidOnly` / `-ServerOnly` / 全量）。
**不以 Dev / Vite HMR / 未装机包验收**；只测 CDN 应用内更新后的发行版。用户未说「先别发」则发。

### 铁律 12: 状态转换表进 README — 改表必同步

根 [README.md](README.md) 维护 **「状态转换表」** 章节（认证 / 通话 / 设备目录 / 导航）。

| 何时 | 做什么 |
|------|--------|
| 新功能改变登录、通话、设备在线、页面跳转 | 先按铁律 6 出表 → 确认 → 改码 → **把新/改行写入 README** |
| 修边界（杀进程、超时、重连）导致转换变化 | 同步更新 README 对应行号 |
| 仅内部重构、对外状态不变 | 可不改表，但不得 silently drift |

**禁止**只改代码不更新表，或只在聊天里留表不落 README。查缺补漏以 README 为准。

---



## Project Vision

Build self-organizing hierarchical visual game AI / peer control. Model interface: **pixels in, actions out**.
C++ for real-time capture + WebView2 + peer; Node MimicServer for signaling (Bootstrap mesh).

## Architecture

详图与 roadmap → 根目录 [README.md](README.md)。

```
pc/client + shared/web  →  Windows WebView2
android   + shared/web  →  Capacitor WebView
server/                 →  Bootstrap signaling
```


| Language         | Role                                                     |
| ---------------- | -------------------------------------------------------- |
| C++              | PC host: Win32, WebView2, capture, peer, updater (`pc/`) |
| TypeScript/React | Shared UI (`shared/web`)                                 |
| Node             | MimicServer (`server/`)                                  |
| Kotlin           | Android MimicHost plugin (`android/plugins`)             |




## Project Structure

```
pc/ client capture input logger updater …
server/
android/
shared/web  shared/protocol
scripts/ installer/ docs/
```



## Build Commands

全 PowerShell，**仅 Prod**（无 Vite HMR / `-Dev`；测试用装机包或应用内更新）。

```powershell
cd shared\web; npm run build
powershell -File scripts\Build.ps1
powershell -File scripts\Build.ps1 -Module mimic_client
```


|              | Value                         |
| ------------ | ----------------------------- |
| Mutex        | `Global\MimicClient_8A3F2D`   |
| Window class | `MimicClient`                 |
| Title        | `Mimic Client`                |
| AppData      | `%LOCALAPPDATA%\MimicClient\` |


Exit code `2` = already running.

## Release Workflow

```powershell
# PC: bump pc/client/src/version.h
# Server: bump server/package.json
powershell -File scripts\Release.ps1              # PC + Server + Android CDN
powershell -File scripts\Release.ps1 -ClientOnly  # PC only
powershell -File scripts\Release.ps1 -AndroidOnly # Android only (no C++)
powershell -File scripts\Release.ps1 -ServerOnly
powershell -File scripts\Release.ps1 -DryRun
powershell -File scripts\Release.ps1 -PublishGitee  # 可选：薄 Setup → Gitee
```

货架 CDN：`http://47.107.43.5/mimic/{client,server,android}/`。默认不传 Gitee Setup（旧 Setup + 应用内更新）；`-PublishGitee` 才挂 thin Setup。

**仓库迁移（2026-07，v0.3.31）**：发布仓 `gitee.com/Andyqwe44/mimic`；旧 `tictactoe` 仓冻结于 v0.3.31 跳板，勿删。

### 用户一键更新

```
Settings → Check Update
  → GET Gitee API /releases/latest → 对比版本号
  → 如果 remote > local → GET raw/<tag>/release/GameAgentMonitor/version.json
  → 逐文件比对 SHA256 → 生成 diff → 下载 → sha256 校验 → updater 覆盖 → 重启
```



## Update Mechanism & Release Package

> **完整自动更新逻辑 + 踩坑史(6 个坑)+ 已知隐患 →** `[docs/auto-update.md](docs/auto-update.md)`



### 发布包结构

```
release/GameAgentMonitor/
  bin/          monitor_app.exe · updater.exe · updater.new · 12 DLL
  frontend/     dist (index.html, assets/)
  config/       settings.default.json
  version.json  schema v3 { schema, app, released, channel, min_version, mandatory, message,
                full_update, download_base, sources[], updater{path}, sig, files{...} }
release/GameAgentMonitor_Setup_v<ver>.exe  ← Inno Setup installer
```

**增量更新下载源 = git raw URL**（不是 Release 附件）。Release 附件(setup.exe)只给全新装机；增量更新逐文件从
`download_base + <path>` 拉 + sha256 校验。`release/GameAgentMonitor/` 必须 commit——它本身就是「增量货」。
`download_base` + `sources[]`(schema v3)= 多源 discovery（Gitee/GitHub raw manifests）；ECDSA 摘要覆盖 download_base+sources；服务端可换源不重编客户端。

### Updater 运作（`updater/updater.cpp`，requireAdministrator manifest）

1. **覆盖更新** `updater.exe <staging_dir> <old_pid>`：等旧进程退出 → 递归拷 staging → install（CopyFileA overwrite，additive）
  → 遇目标==自己：`MoveFileExA(→.old)` 再拷新 → 启 monitor_app → remove_tree(staging)
2. **自装** `updater.exe --self-install`：把自己拷成同目录 updater.exe → `MoveFileEx(DELAY_UNTIL_REBOOT)` 删 .new

**完整更新链**：`download_update`（后台 std::thread 逐文件下+sha256 校验→staging）→ 启 updater 覆盖 → 启新 monitor_app
→ 首启 `check_and_heal_updater`（比 install updater.exe sha vs version.json；旧则启 `updater.new --self-install`）。

### Gitee 注意事项


| 问题                            | 解决方案                                                  |
| ----------------------------- | ----------------------------------------------------- |
| **Gitee 不允许替换 Release asset** | 必须 DELETE 整个 Release → DELETE remote tag → 重建         |
| **raw URL 的文件来自 git 仓库**      | `raw/<tag>/path` 读取的是 git tag 下提交的文件，不是 Release asset |
| **China 网络**                  | Gitee 在国内访问稳定；raw URL 无需认证即可下载                        |




## Internal Architecture



### Communication: WebMessage bridge

```
JS:  hostCall('list_windows') → chrome.webview.postMessage('{"cmd":"list_windows","id":1}')
C++: WebMessageReceived → HandleWebMessage → dispatch → PostWebMessageAsJson({id, result})
JS:  'message' event → e.data is pre-parsed → hostCall auto-unwraps .result
```



### Command dispatch


| Command                                    | Args                                           | Returns                                   |
| ------------------------------------------ | ---------------------------------------------- | ----------------------------------------- |
| `list_windows`                             | —                                              | `[{title, category, hwnd, desktop}, ...]` |
| `capture_window`                           | `{hwnd, method}`                               | `{ok, w, h, method}` — via SharedBuffer   |
| `capture_stream_start`                     | `{hwnd, method, transport}`                    | `{ok:true}`                               |
| `capture_stream_stop`                      | —                                              | `{ok:true}`                               |
| `read_logs`                                | `{max_files}`                                  | `{files:[{name, size}, ...]}`             |
| `read_log_file`                            | `{filename}`                                   | `{filename, content}`                     |
| `read_live_log`                            | —                                              | `{lines}` — ring buffer sync              |
| `log_ui_event`                             | `{event, detail}`                              | `{ok:true}` — no echo back                |
| `send_input`                               | `{hwnd, type, x_norm, y_norm, button, method}` | `{ok:true}`                               |
| `get_version`                              | —                                              | `"0.3.29"`                                |
| `get_settings` / `set_settings`            | — / `{settings:{...}}`                         | load / atomic save user prefs (AppData)   |
| `list_desktops`                            | —                                              | `[{name, index, current}, ...]`           |
| `switch_desktop`                           | `{index}`                                      | `{ok:true}`                               |
| `benchmark_methods`                        | `{hwnd, method}`                               | `{results:[...]}`                         |
| `set_frame_dump`                           | `{capture, stream, dir}`                       | `{ok:true}` (Dev mode)                    |
| `launch_test_target`                       | —                                              | `{ok, action}` — toggle test window       |
| `find_test_target`                         | —                                              | `{hwnd}` — 0 if not running               |
| `selftest_connect` / `selftest_disconnect` | `{port}` / —                                   | TCP client (:9998)                        |




### Method routing (铁律 5)

**Frontend decides method, C++ only executes.** No silent fallback.

Single-frame (`call_capture`):


| Method                | Backend                                     |
| --------------------- | ------------------------------------------- |
| `wgc`                 | `wgc_capture_single(hwnd)` — hwnd=0 → error |
| `wgc-monitor`         | `wgc_capture_single_monitor(hmon)`          |
| `dxgi` / `desktopblt` | DesktopBlt, returns `method="DesktopBlt"`   |
| `GDI(GetWindowDC)`    | `capture_gdi_getwindowdc(hwnd)`             |
| `PrintWindow`         | `capture_printwindow(hwnd)`                 |
| unknown               | Returns error, no fallback                  |


Streaming (`capture_stream_start`):


| Method  | Backend                           |
| ------- | --------------------------------- |
| `wgc`   | WGC stream (hwnd or monitor mode) |
| `dxgi`  | Returns error — not implemented   |
| unknown | Returns error                     |




### Input policy (thin client · 2026-07)


| Target           | Policy                                         | Wire method   |
| ---------------- | ---------------------------------------------- | ------------- |
| Desktop `hwnd=0` | Foreground (may occupy user mouse/keyboard)    | `sendinput`   |
| Window           | Background; coords must stay in window `[0,1]` | `sendmessage` |


Remote `CONTROL_MSG` / WS actions are forced onto the active stream `hwnd` (cannot retarget). Atomic types: `mousedown`/`mouseup`/`move(+held)`/`keydown`/`keyup`/`text`. Local Mapping/IME scaffolding is off when `THIN_CLIENT` (`monitor_web/src/lib/features.ts`).

**Gates:** `allow_stream` / `accept_control` (UI: 发送画面 / 接受控制). Stream push and remote input only when the matching gate is open.

**Remote video (thin):** WGC GPU texture → MF **hardware** H.264 (DXGI; `MF_TRANSFORM_ASYNC_UNLOCK` + NeedInput/HaveOutput) → TCP `:9999` + embedded WS `:9997`. Soft MFT is fallback only (scaled ≤1280). **Not MJPEG.** No Python `bridge.py`. `controller_web` (React) is built by `Build.ps1` into `build(_dev)\controller\` and served by `ws_server`.

**HW encode proof:** `test/h264_hw_bench.cpp` + `test/h264_recv_bench.mjs` (`Build.ps1 -Module h264_bench`) — WGC→HW H.264→TCP :19999.

### Streaming pipeline

```
Remote (thin / gates open):
  WGC GPU tex → H264Encoder::encode_texture → TCP/WS Annex-B  (no local SharedBuffer)

Local preview (non-thin / debug):
  WGC → Map(CPU) → stream_bridge PostMessage → SharedBuffer → Canvas
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


| Layer           | Strategy                                                               |
| --------------- | ---------------------------------------------------------------------- |
| C++ ring buffer | In-place update (no duplicate added)                                   |
| C++ log file    | Write-then-collapse (crash-safe: raw first, then overwrite + truncate) |
| TS addRemote    | C++ notify sends count/firstTs, TS stores as-is                        |
| TS add (UI)     | Independent check-then-update                                          |




### Capture methods


| Method       | Lib         | Sys deps                |
| ------------ | ----------- | ----------------------- |
| WGC          | wgc.lib     | d3d11, dxgi, windowsapp |
| GetWindowDC  | gdi.lib     | user32, gdi32           |
| PrintWindow  | pw.lib      | user32, gdi32           |
| ScreenBitBlt | screen.lib  | user32, gdi32           |
| DesktopBlt   | desktop.lib | user32, gdi32           |
| Common       | common.lib  | user32, dwmapi          |




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
lib/: bridge.ts, types.ts, constants.ts, i18n.ts, windowTitle.ts, bootSettings.ts
locales/: en.json, zh-CN.json, zh-TW.json
```

**i18n（摘要）**：TopBar 语言下拉 + Settings → General；`settings.json` 键 `locale`（经 `set_settings`）；插值用 `{var}`（非 `{{var}}`）；`ActionBtn` 宽度按拉丁=1 / CJK≈2 单位自动选档。细则 → `.cursor/rules/monitor-web.mdc`；变更史 → `CLAUDE.old.md`。

**Settings / boot（摘要）**：AppData Dev=`GameAgentMonitor_Dev` / Prod=`GameAgentMonitor`；`set_settings` 整包原子写；C++ `AddScriptToExecuteOnDocumentCreated` 注入 `__BOOT_SETTINGS__` 消主题闪烁。史 → `CLAUDE.old.md`。

**DevMode overlays（摘要 · 铁律 5）**：`display = demoOverlay ?? SSOT`。Demo（Agent/更新弹窗/自检）只写 overlay，禁止写真相。关 Dev = 清 overlay → 关能力（dump/Test Target/真自检）→ `get_agent_status` 重检。假更新弹窗纯前端，不调下载 API。细则 → `monitor-web.mdc`；史 → `CLAUDE.old.md`。

**PagePager（摘要）**：皇室战争式单 offset（`translate3d`）；跟手手写、松手/底栏唯一 `settleTo`（rAF+bezier）。状态表 → 根 [README.md](README.md) C1–C6；史 → `CLAUDE.old.md`。

**Peer 媒体（摘要 · 2026-07-20）**：同网 H.264 走 **UDP MPC2**（FEC 4+1 + NACK，80ms 重组）；控制 JSON 仍 LAN TCP。drop-old / 丢片 → force IDR + 依赖门闸；解码冻帧。详表 → README「Peer 媒体传输」。

---



## Known Issues

1. **WGC FPS**: Event-driven — static content = low FPS. Dynamic window = 60+.
2. **H.264 remote path**: Prefer HW DXGI MFT; soft fallback is capped. WebCodecs needs Baseline Level 4.0 (`avc1.42E028`) for 1080p.
3. **Chromium background tab throttling**: WebView2 may throttle when app loses focus.
4. **WebView2 cross-thread COM**: STA-only interfaces, COM marshaling fails. Stream uses PostMessage bridge.
5. **Async break-point jitter**: TS `hostCall('log_ui_event')` arrives async — may split C++ log runs. Cosmetic only.

---



## 完整历史

开发日志、Recent Fixes 详细描述、历史变更完整记录 → `CLAUDE.old.md`（铁律 7）。