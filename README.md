# Mimic — peer client + signaling server

Desktop thin client for visual game AI / remote control — **pixels in, actions out**.

> Canonical repo: [gitee.com/Andyqwe44/mimic](https://gitee.com/Andyqwe44/mimic)
> (mirror: [github.com/Andyqwe44/Mimic](https://github.com/Andyqwe44/Mimic)).

## Architecture

```
┌─ MimicClient (Windows) ─────────────────────────────────────────┐
│  mimic_client.exe + mimic_web (WebView2)                         │
│  WGC capture → HW H.264 · peer invite · LAN media                │
│  Default signaling: Bootstrap http://47.107.43.5:8443            │
└──────────────────────────────┬──────────────────────────────────┘
                               │ login / WS presence (not media)
                               ▼
┌─ MimicServer (Node, symmetric) ─────────────────────────────────┐
│  Every install is the same package. On start → join Bootstrap.   │
│  Bootstrap registry: /api/cluster/join + reverse /health probe.  │
│  Media never relays through this process.                        │
└─────────────────────────────────────────────────────────────────┘
```

| Package | Source | CDN |
|---------|--------|-----|
| **MimicClient** | `mimic_client/` + `mimic_web/` | `http://47.107.43.5/mimic/client/` |
| **MimicServer** | `mimic_server/` | `http://47.107.43.5/mimic/server/` |

Gitee Release ships thin Setups that download `payload.zip` at install time.

## Bootstrap mesh (phase 1 — discovery only)

1. Bake-in `BOOTSTRAP_URL = http://47.107.43.5:8443` (override with `MIMIC_BOOTSTRAP`).
2. On start, if this host owns the bootstrap IP → **role=bootstrap** and self-register.
3. Otherwise `POST /api/cluster/join` → Bootstrap echoes public URL, reverse-probes `/health`, returns node list.
4. Heartbeat every 15s. `GET /api/cluster` for clients / peers.
5. Clients still **login to Bootstrap** by default. Cross-node presence is **not** in this phase (see roadmap).

```powershell
# Same Setup everywhere — no region pre-config
node server.js --host 0.0.0.0 --port 8443
# demo / demo after first run
```

## Build & release

```powershell
# Native + frontend (prod only — no Vite HMR / -Dev track)
cd mimic_web; npm run build
powershell -File scripts\Build.ps1                    # all modules
powershell -File scripts\Build.ps1 -Module mimic_client

# Full release (client + server)
powershell -File scripts\Release.ps1 -DryRun
powershell -File scripts\Release.ps1

# Independent channels
powershell -File scripts\Release.ps1 -ClientOnly      # APP_VERSION in mimic_client/src/version.h
powershell -File scripts\Release.ps1 -ServerOnly      # version in mimic_server/package.json
```

| Version truth | File |
|---------------|------|
| Client | `mimic_client/src/version.h` → `APP_VERSION` |
| Server | `mimic_server/package.json` → `version` |

## Quick start (local)

```powershell
# Signaling
cd mimic_server; npm install; npm start   # :8443

# Client (after Build.ps1)
.\mimic_client\build\bin\mimic_client.exe
```

Peer panel default URL: `http://47.107.43.5:8443`.

## 后续更新计划 (roadmap)

1. **跨节点 presence 联邦** — 账号库以 Bootstrap 为 SSOT 或同步；设备目录汇聚；`invite` / `signal` / `hangup` 跨节点 HTTP 转发（同账号连不同区域也能互见）。
2. 客户端按 RTT / 区域从 `/api/cluster` 选服（须在联邦完成后再开，否则设备互不可见）。
3. 信令 URL 持久化（settings），与旧 `serverHost:9997` 彻底拆清。
4. `wss`/TLS 与客户端 WinHTTP/WS TLS 对齐。
5. MimicServer 独立增量更新（现仅 CDN 整包）。
6. `controller_server` / `controller_web` 去留（仍构建、不进 MimicClient/Server 包）。

## Layout

```
mimic_client/   C++ WebView2 host (exe)
mimic_web/      React UI (Vite build → frontend/)
mimic_server/   Node signaling + Bootstrap mesh
logger/ capture/ input/ updater/   native libs
scripts/        Build.ps1 / Release.ps1 / CDN / Verify
installer/      thin MimicClient_Setup + MimicServer_Setup
```

Agent context: `.cursor/rules/*.mdc`. Long-form: `CLAUDE.md`.

See [mimic_server/README.md](mimic_server/README.md) and [docs/auto-update.md](docs/auto-update.md).
