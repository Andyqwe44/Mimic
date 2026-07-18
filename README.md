# Mimic — PC / Server / Android

Peer remote client + symmetric signaling. **Pixels in, actions out.**

> Canonical repo: [gitee.com/Andyqwe44/mimic](https://gitee.com/Andyqwe44/mimic)
> (mirror: [github.com/Andyqwe44/Mimic](https://github.com/Andyqwe44/Mimic)).

## Products

| Product | Path | Runtime |
|---------|------|---------|
| **PC (Windows)** | [`pc/`](pc/) | C++ WebView2 host + native capture/input |
| **Server** | [`server/`](server/) | Node signaling + Bootstrap mesh |
| **Android** | [`android/`](android/) | Capacitor WebView (skeleton) |
| **Shared UI** | [`shared/web/`](shared/web/) | React — used by PC **and** Android |
| **Protocol** | [`shared/protocol/`](shared/protocol/) | FRAM / H.264 / CONTROL_MSG |

```
┌─ shared/web (React) ────────────────────────────────────────────┐
│  hostCall bridge → Windows WebView2  OR  Android Capacitor      │
└───────────────┬─────────────────────────────┬───────────────────┘
                │                             │
       ┌────────▼────────┐           ┌────────▼────────┐
       │ pc/client (.exe)│           │ android (APK)   │
       │ capture / input │           │ MimicHost plugin│
       └────────┬────────┘           └────────┬────────┘
                │  login / WS presence        │
                └────────────┬────────────────┘
                             ▼
                  ┌─ server/ (Bootstrap) ─┐
                  │  http://47.107.43.5:8443 │
                  └───────────────────────┘
```

**Electron is not used on Android** (desktop-only). Mobile uses the same WebView UI pattern as PC.

## Layout

```
pc/
  client/       # C++ WebView2 → mimic_client.exe
  capture/ input/ logger/ updater/ common/
  test/ test_target/
  legacy/       # controller_server / controller_web (aux)
server/         # MimicServer (Node)
android/        # Capacitor shell + MimicHost plugin sources
shared/
  web/          # React UI (npm run build → dist/)
  protocol/     # wire constants
scripts/        # Build.ps1 / Release.ps1 / CDN
installer/      # thin MimicClient_Setup + MimicServer_Setup
docs/
```

## Bootstrap mesh (phase 1 — discovery)

1. Bake-in `BOOTSTRAP_URL = http://47.107.43.5:8443`
2. Every MimicServer install joins Bootstrap (`/api/cluster/join` + heartbeat)
3. Clients default-login to Bootstrap; cross-node presence is later (roadmap)

## Build & release (PC + Server)

```powershell
cd shared\web; npm run build
powershell -File scripts\Build.ps1                    # all native under pc/
powershell -File scripts\Build.ps1 -Module mimic_client

powershell -File scripts\Release.ps1 -DryRun
powershell -File scripts\Release.ps1 -ClientOnly      # pc/client/src/version.h
powershell -File scripts\Release.ps1 -ServerOnly      # server/package.json
```

| Version truth | File |
|---------------|------|
| PC client | `pc/client/src/version.h` → `APP_VERSION` |
| Server | `server/package.json` → `version` |
| Android | `android/package.json` + `android/version.json` |

CDN: `http://47.107.43.5/mimic/client/` · `.../server/` · `.../android/`。

Gitee Release 附件：`MimicClient_Setup_*.exe` · `MimicServer_Setup_*.exe` · `MimicAndroid_Setup_*.zip`（骨架；有 APK 时 zip 内含 APK）。

## Android skeleton (current)

See [`android/README.md`](android/README.md).

- Reuses **shared/web** UI
- Crash handlers → `crash_log`
- `peer_probe` / settings / `check_update` + APK download stub via MimicHost
- Bidirectional phone↔PC control = **next phase**

## Quick start (local)

```powershell
cd server; npm install; npm start   # :8443
# after Build.ps1:
.\pc\client\build\bin\mimic_client.exe
```

## 后续更新计划 (roadmap)

1. **跨节点 presence 联邦** — 设备目录汇聚；invite/signal 跨节点转发
2. 客户端按 RTT / 区域选服
3. `wss`/TLS
4. MimicServer 独立增量更新
5. **Android 双向远控** — 手机控 PC → 再 PC 控手机（MediaProjection / 无障碍）
6. `pc/legacy/controller_*` 去留

Agent context: `.cursor/rules/*.mdc`. Long-form: `CLAUDE.md`.

See [server/README.md](server/README.md), [android/README.md](android/README.md), [docs/auto-update.md](docs/auto-update.md).
