# Mimic Android — thin Setup → CDN Client（对齐 PC）

和 PC 一样分两层：

| 角色 | 文件 | 放哪 |
|------|------|------|
| **Setup（薄安装器）** | `MimicAndroid_Setup_v*.apk` | Gitee Release / 也可放 CDN |
| **Client（完整应用）** | `MimicClient_Android_v*.apk` | **只放 CDN** `http://47.107.43.5/mimic/android/` |

```
手机下载 Setup.apk（小入口）
  → 打开 Mimic Setup
  → GET CDN /android/version.json
  → 下载 client_apk
  → 系统安装 Mimic Client
```

对应 PC：`MimicClient_Setup.exe` → CDN `payload.zip`。

## 工程

```
android/setup/     # Gradle 双模块
  setup/           # com.mimic.setup — 薄安装器
  client/          # com.mimic.client — WebView + shared/web + native host
    …/AndroidHost.kt          # hostCall 分发（gate / target / peer / capture）
    …/capability/              # normal | shizuku | root（显式，无静默回退）
    …/target/                  # list_targets / AppEnumerator / AppLauncher
    …/capture/                 # MediaProjection + MediaCodec H.264 Annex-B
    …/input/                   # AccessibilityService（普通档）
    …/peer/                    # HTTP/WS 信令 + LAN type=1/2 帧
```

**能力档：** 普通 = MediaProjection + 无障碍；Shizuku/root 依赖与特权服务尚未打包（fail-closed）。
**Peer：** 登录/邀请/LAN offer 已接线；与 PC 共用 peer proto v2（`list_targets` / `id`）。

## 构建

```powershell
powershell -File scripts\Build-Android.ps1
# 产物: release\MimicAndroid\*.apk + version.json
```

需要本机已装 **Android Studio（SDK）**；日常可用命令行 Gradle，不必每次开 Studio。

## Setup 会不会重复下载？

会检测。`<queries>` 声明 `com.mimic.client` 后，Setup 可用 `PackageManager` 读已装版本（**不需要** `QUERY_ALL_PACKAGES`）。

- 已装且 **≥ CDN `app`** → 跳过下载，直接打开 Mimic  
- 已装但更旧 → 下载并覆盖安装  
- 按钮「Re-download」可强制重下  

## 应用内更新（全量 APK，不是 PC 增量）

安卓安装单元是**整包签名 APK**，没有 PC 那种「多文件 sha256 差量覆盖目录」的 updater。

当前设计（正确且刻意）：

1. CDN `version.json`：`full_update: true` + `client_apk` + `client_sha256`  
2. 下载整包 APK → **SHA-256 校验** → 系统安装器同 `applicationId` 覆盖安装  
3. UI / 原生改动都打进 APK（`shared/web` 在 assets），所以每次更新都是整包  

若将来要「更小下载」，应做 **APK 二进制差分（bsdiff）** 或上 Play 分发，而不是移植 PC 的 `files{}` 清单。

Settings → Check Update 走 `check_update` / `download_update`（shared/web 同一套 UI，`mode: full`）。

## 手机测试

1. 装 `MimicAndroid_Setup_v*.apk`（Gitee / CDN）。  
2. 打开 Setup → 从 CDN 装 Client（或已最新则直接打开）。  
3. Client 应显示与 PC 相同的 React UI（shared/web）。  
4. 测：**日志面板**、**Settings → Check Update**、Peer Probe Bootstrap。

## Cursor / Android Studio

- **不用「连接」Studio**；Cursor 改代码，终端跑 `Build-Android.ps1`。
- Studio 用于：SDK 管理、真机调试、签名配置。
- Cursor 可装 Kotlin 语法扩展，**不能**替代完整 Android 构建链。
