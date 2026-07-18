# Mimic Android (skeleton)

Same React UI as Windows (`shared/web`), hosted in a **Capacitor WebView**.

Electron is desktop-only and is **not** used here.

## Goals (this phase)

| Flow | Status |
|------|--------|
| Shared UI (`shared/web`) | wired via `webDir` |
| Crash / unhandledrejection → `crash_log` | JS handlers + native stub |
| Connect MimicServer (probe / login / WS) | via `hostCall` → MimicHost |
| In-app update from CDN APK | stub → `http://47.107.43.5/mimic/android/` |
| Bidirectional remote control | **later** (phone↔PC) |

## Layout

```
android/
  capacitor.config.ts   # webDir → ../shared/web/dist
  package.json
  version.json          # CDN manifest (APK channel)
  plugins/MimicHost/    # Capacitor plugin sources (copy into generated project)
  README.md
```

Native Android Studio project is generated with Capacitor (`npx cap add android`) and is gitignored until the first stable APK is published.

## One-time setup (dev machine)

```powershell
cd shared\web; npm install; npm run build
cd ..\..\android
npm install
npx cap add android
# Copy plugins/MimicHost into android/app (see plugins/README.md)
npx cap sync android
npx cap open android
```

## CDN update (same idea as PC)

1. Build signed APK → upload to `http://47.107.43.5/mimic/android/`
2. Publish `version.json` + `MimicClient_vX.Y.Z.apk`
3. App calls `check_update` / `download_update` → PackageInstaller

## Version

Bump `android/package.json` + `android/version.json` together (independent of PC `APP_VERSION`).
