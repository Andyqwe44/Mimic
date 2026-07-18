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
  client/          # com.mimic.client — 客户端（当前为 stub，后续接 Capacitor+shared/web）
```

## 构建

```powershell
powershell -File scripts\Build-Android.ps1
# 产物: release\MimicAndroid\*.apk + version.json
```

需要本机已装 **Android Studio（SDK）**；日常可用命令行 Gradle，不必每次开 Studio。

## 手机测试

1. 把 `MimicAndroid_Setup_v0.1.0.apk` 传到手机（Gitee / CDN / 数据线）。
2. 允许「未知来源」安装 Setup。
3. 打开 **Mimic Setup** → 允许「安装未知应用」→ 自动从 CDN 拉 Client → 确认安装。
4. 打开 **Mimic** → 点 Probe Bootstrap，应能访问 `http://47.107.43.5:8443`。

## Cursor / Android Studio

- **不用「连接」Studio**；Cursor 改代码，终端跑 `Build-Android.ps1`。
- Studio 用于：SDK 管理、真机调试、签名配置。
- Cursor 可装 Kotlin 语法扩展，**不能**替代完整 Android 构建链。
