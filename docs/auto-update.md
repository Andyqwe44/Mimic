# 自动更新系统 (Auto-Update)

> 一句话:用户点「检查更新」→ 从 Gitee 拉最新版 `version.json` → 逐文件从 **git raw URL** 下载到 staging →
> sha256 校验 → **提权的独立 updater** 覆盖安装目录 → 重启新版。
>
> 首次端到端跑通:**0.3.12 → 0.3.13**(2026-07-12)。

---

## 1. 全景流程

```
┌─ 用户点 Check Update ─────────────────────────────────────────────┐
│                                                                    │
│  ① check_update (commands.cpp / cmd_check_update)                 │
│     GET gitee.com/api/v5/.../releases/latest  → tag "v0.3.13"      │
│     版本串比:latest != current(APP_VERSION)  → hasUpdate           │
│     GET raw/<tag>/release/GameAgentMonitor/version.json (manifest) │
│     逐文件比 sha256 → diff 列表(21 files)                          │
│                                                                    │
│  ② download_update (后台 std::thread / download_thread_func)       │
│     每个 diff 文件:download_base + path  → git raw URL            │
│        → 302 → raw.giteeusercontent.com → 下载                    │
│     每文件 sha256 校验 → 写 %LOCALAPPDATA%\...\staging\            │
│     全过 → g_up.succeeded = true                                   │
│     (节流 WM_UPDATE_PROGRESS → 前端进度条)                         │
│                                                                    │
│  ③ 完成 (main.cpp WndProc / WM_UPDATE_PROGRESS, up.succeeded)      │
│     update_launch_updater()                                        │
│        ShellExecuteEx "runas" → UAC → updater.exe(管理员)         │
│        参数: "<staging_dir>" <monitor_app_pid>                     │
│     Sleep(200) → PostQuitMessage(0)  (旧 monitor_app 准备退出)     │
│                                                                    │
│  ④ updater.exe (updater/updater.cpp, requireAdministrator)        │
│     等 <pid> 退出 (≤30s, 超时 Terminate) + Sleep 500ms            │
│     解析 install 目录: exe-relative 优先 (<install>\bin\updater)  │
│        → install = 父的父;注册表 InstallPath 兜底                 │
│     copy_staging: 递归拷 staging\* → install\ (CopyFile 覆盖)      │
│        自替换 updater 用改名技巧 (MoveFileEx self→.old 再拷)       │
│     ShellExecute "open" install\bin\monitor_app.exe  (启新版)      │
│     remove_tree(staging)  (清理)                                   │
│                                                                    │
│  ⑤ 新 monitor_app.exe 启动 → 版本 0.3.13 ✓                        │
└────────────────────────────────────────────────────────────────────┘
```

---

## 2. 关键设计决策(为什么这么做)

| 决策 | 为什么 |
|------|--------|
| **增量货从 git raw URL 下,不是 Release 附件** | Release 附件只放一个 setup.exe(给新用户全新装)。逐文件下载走 `raw/<tag>/release/GameAgentMonitor/<path>`,读的是 git 仓库里 commit 的文件。**所以 `release/GameAgentMonitor/` 必须 commit 进 git —— 它本身就是更新货。** |
| **独立 updater 进程干覆盖,主程序不提权** | 覆盖 `Program Files` 需管理员;但主程序日常不该提权(UAC 骚扰 + 安全)。所以主程序 asInvoker,只在更新时 `runas` 拉起提权的 updater 去覆盖。updater 是唯一 `requireAdministrator` 的。 |
| **manifest (`version.json`) 在 git 里 = 服务端可控** | schema v2 加了 `download_base`(换下载源不用重编客户端)、`min_version`(强制全装跨破坏性变更)、`mandatory`/`message`(服务端指挥 UI)。改这些字段 = 改更新行为,不用发新客户端。 |
| **updater 用 exe-relative 定位 install** | 早期发现注册表 `InstallPath` 可能缺;exe-relative(updater 在 `<install>\bin\`)自包含更稳,注册表兜底。 |
| **失败大声报错,不当「无更新」(铁律 5)** | 拉取 manifest 失败必须返回 `{ok:false,error}`,绝不返回空 diff 让前端显示「已是最新」—— 那是 0.3.5 卡死的根源。 |

---

## 3. manifest 结构 (`version.json`, schema v2)

由 `scripts/New-VersionJson.ps1` 在发布时生成,commit 进 git,raw URL 供给。

```jsonc
{
  "schema": 2,                 // 客户端懂的版本;更高 → 提示下完整包 (needs_full_installer)
  "app": "0.3.13",
  "channel": "stable",         // 预留 beta/stable
  "min_version": "0.3.13",     // 低于此的客户端 → 强制 full(下全部文件)
  "mandatory": false,          // true → 前端不许「稍后」
  "message": "",               // 服务端给用户的话
  "full_update": false,
  "download_base": "https://gitee.com/Andyqwe44/tictactoe/raw/v0.3.13/release/GameAgentMonitor/",
  "updater": { "path": "bin/updater.exe" },
  "sig": "",                   // 预留 Ed25519 签名(未实装)
  "files": {                   // 每文件 sha256 = 增量比对依据
    "bin/monitor_app.exe": { "v": "0.3.13", "sha256": "…", "size": 444416 },
    ...
  }
}
```

---

## 4. 涉及文件 / 函数

| 文件 | 角色 |
|------|------|
| `monitor_app/src/commands.cpp` | `cmd_check_update`(检测+diff)、`download_thread_func`(下载+校验)、`update_launch_updater`(提权拉 updater)、`winhttp_get`(HTTP,非2xx 报错) |
| `monitor_app/src/main.cpp` | `WndProc` / `WM_UPDATE_PROGRESS`(下载完 → 启 updater + 退出) |
| `updater/updater.cpp` | 独立提权 updater:等 pid、定位 install、`copy_staging` 覆盖、启新 exe、清 staging;`--self-install`(破自替换死循环) |
| `monitor_web/src/App.tsx` | `checkForUpdate` / `startDownload`(前端调度,`{ diff }` 不双重编码) |
| `monitor_web/src/components/UpdateModal.tsx` | 更新弹窗(版本/message/进度条/mandatory) |
| `scripts/New-VersionJson.ps1` | 生成 `version.json`(schema v2, `Get-FileHash` 每文件 sha256) |
| `scripts/Release.ps1` | 一键发布:npm build → 编译 → 组装 → version.json → installer → 隔离验证 → git push → Gitee → 验 raw |

---

## 5. 踩坑史(从头到尾 6 个坑)

更新链是**流水线**:检测→下载→校验→提权→覆盖→重启。前一环不通就到不了后一环,所以 bug **一个个才暴露**——修好检测才跑到下载、修好下载才跑到 updater……必须一版版往前推。

| # | 暴露于 | 坑 | 症状 | 根因 | 修法 |
|---|--------|-----|------|------|------|
| 1 | 0.3.5→0.3.6 | **静默失败** | 弹「有更新」,点了「无需升级」 | 远端 manifest 拉取失败,`winhttp_get` 把非 2xx 错误页当数据返回 → 无 `"files"` → diff 空,但 hasUpdate(版本串)仍 true | `winhttp_get` 非 2xx 返空;`check_update` 拉取失败 3 次重试后返 `{ok:false,error}`,不当「无更新」 |
| 2 | 同上 | **compare 脆 + 不可控** | (连带) | 版本串比 vs sha 比两套逻辑各行其是 | schema v2:manifest 带 sha/min_version/download_base,统一 + 服务端可控 |
| 3 | 0.3.8→0.3.9 | **diff 双重转义** | 检测到 21 文件,download 报「no files to download」 | 前端 `hostCall('download_update',{diff:JSON.stringify(diff)})` 双重 JSON 编码 → 后端拿到 `[{\"path\":…}]`,`find("\"path\"")` 找真引号找不到 → 0 文件 | 前端改 `{ diff }` 不双重 stringify;后端 dispatch 反转义 `\"`→`"` 兜底 |
| 4 | 0.3.10→0.3.11 | **updater 起不来** | 下载+校验全过,`CreateProcess err=740` | `updater.exe` 是 requireAdministrator;`CreateProcess` 从普通进程起不了提权 exe(ERROR_ELEVATION_REQUIRED) | 改 `ShellExecuteEx "runas"` 弹 UAC 提权 |
| 5 | 同上(顺带) | **updater 定位 install** | (潜在)注册表缺则「Cannot find install path」失败 | updater 只读注册表 `HKLM\...\InstallPath` | 改 exe-relative 优先(updater 在 `<install>\bin\`),注册表兜底 |
| 6 | 0.3.12→0.3.13 | **✅ 全通** | 检测→下载→校验→UAC→覆盖→重启 0.3.13 | — | — |

> 附:更新逻辑的可调试性,靠**富日志**(每步 LOG:`check_update: …diff_files=N`、`HTTP <code>`、`download_update: …`、
> `update_launch_updater: …`)+ **`scripts/Read-InstalledLogs.ps1`**(抓装机版真日志)。整个排查就是「点更新→读日志→定位→修」。

---

## 6. 怎么发新版 + 怎么测更新链

**发版**(铁律 8,版本号单一真相源):
1. 改 `monitor_app/src/version.h`(`APP_VERSION` + `APP_VERSION_RC`)
2. `powershell -File scripts\Release.ps1`(从 PowerShell 窗口跑最稳)
   —— 自动:npm build → 编译全模块 → 组装 `release\` → `version.json` → installer → 隔离验证 → git commit+tag+push → Gitee Release → 验 raw URL

**测更新链**:装一个**旧版**(如 0.3.13)→ 发一个**新版**(如 0.3.14,纯 bump 即可)→ 在旧版点 **Settings → Check Update** → 应:检测新版 → 下载 → **UAC 提权** → 覆盖 → 重启成新版。

---

## 7. 已知隐患(非阻断,记录待办)

| 隐患 | 说明 | 影响 |
|------|------|------|
| **`version.json` 不随更新更新** | staging 不含 `version.json`(不在 `files{}` 里),updater 不拷它 → install 的 `version.json` 停在旧版 | 下次 `check_update` 读到旧 sha → 全文件都算「变了」→ over-download。但**当前 `min_version` 默认 = 发版自己 → 每次都强制 full**,掩盖了这问题(能更新,只是不增量) |
| **每次都是 full(不增量)** | `New-VersionJson.ps1` 的 `-MinVersion` 默认 = `$Version` → `current < min_version` 恒真 → 强制 full | 要真增量:发版时传 `-MinVersion <上一个稳定版>`,并先解决上面的 version.json 同步 |
| **`sig` 未实装** | manifest 预留了签名字段,但未做 Ed25519 验签 | 供应链/MITM 防护缺失(Gitee raw 是 HTTPS,风险低)。要做时:客户端内置公钥,验 manifest 签名 |

---

## 8. 权限提权的两处用法(别混淆)

updater 提权(更新覆盖 Program Files)和「运行权限切换」(Settings 让主程序自己变管理员)**用同一套 `ShellExecuteEx runas` 机制,但目标不同**:

- **更新**:`runas` 启**别的 exe**(updater.exe),主程序退出让它覆盖。
- **权限切换**:`runas`(升)/ token 降级(降)启**同一个 exe**(monitor_app 自己),旧的退出。切换前先 `app_release_singleton()` 释放单实例锁,否则新进程撞单实例守卫自杀(0.3.13 修)。

详见各自代码 + `CLAUDE.md` changelog(0.3.12 权限功能 / 0.3.13 单实例锁修复)。
