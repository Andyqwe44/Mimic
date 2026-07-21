# Android native bridge

**发版路径**：`android/setup/client` — `MainActivity` 裸 WebView + `AndroidHost.dispatch`（页面内注入 `Capacitor.Plugins.MimicHost` shim）。

旧 Capacitor 模板 `MimicHostPlugin.kt` 已删除（从未编入 setup/client）。

Kotlin 只做 Web 做不了的事：采集/编码、peer、输入、Shizuku、系统 Intent / APK 更新。  
UI 与横滑（`PagePager`）在 `shared/web`。
