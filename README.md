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

## 状态转换表（SSOT · 铁律 6 + 12）

> 新功能若改变登录 / 通话 / 设备在线 / 页面跳转，**先出表 → 确认 → 改码 → 更新本节**。  
> 四层正交：**Auth**（认证）· **Call**（通话）· **Device**（目录项）· **Page**（导航）。

### 状态列表

| 层 | 状态 | 含义 |
|----|------|------|
| Auth | `LoggedOut` | 无 token / 已 logout |
| Auth | `LoggedIn` | 持有 token，信令在线或意图保持登录 |
| Auth | `Reconnecting` | 持有 token，本机 WS 断、正在重连（**不是**登出） |
| Call | `Idle` | 已登录，无呼叫 |
| Call | `Outgoing` | 已发出邀请 |
| Call | `Ringing` | 来电振铃（Banner + 导火索进度条） |
| Call | `InCall` | 会话中（`controller` / `controlled`） |
| Device | `online` / `away` / （移除=offline） | 对端 live WS / grace 中 / 确认离线 |
| Page | `Peers` / `Monitor` / … | 通话结束**强制**回 Peers |

### 转换表

| # | Auth | Call | 事件 | → Auth | → Call | Page | 可见反馈 |
|---|------|------|------|--------|--------|------|----------|
| 1 | LoggedOut | — | login 成功 | LoggedIn | Idle | Peers | 设备列表 |
| 2 | LoggedIn | Idle | logout（`goodbye`） | LoggedOut | — | Peers | 登录表单；**对端目录立刻移除**（无 away grace） |
| 3 | LoggedIn | Idle | invite 发出 | LoggedIn | Outgoing | Peers | 呼叫中 |
| 4 | LoggedIn | Idle | 收到 invite | LoggedIn | Ringing | Peers | Banner |
| 5 | LoggedIn | Outgoing/Ringing | session_start | LoggedIn | InCall | **Monitor** | 进入通话 |
| 6 | LoggedIn | InCall | 本机 hangup | LoggedIn | Idle | **Peers** | 会话已结束 |
| 7 | LoggedIn | InCall | 对方杀进程 / WS 死 → `session_end(peer_disconnect)` | **LoggedIn** | **Idle** | **Peers** | **对方设备已离线**；目录 away→移除 |
| 8 | LoggedIn | Outgoing | 对方 reject | LoggedIn | Idle | Peers | 邀请被拒绝 |
| 9 | LoggedIn | Ringing | **本机 Banner 超时（10s 导火索烧完）** → `peer_reject` | LoggedIn | Idle | Peers | Banner 关闭 |
| 10 | LoggedIn | * | 本机 WS 断 | Reconnecting | 保持或本地降级 | 保持/Peers | 重连中 |
| 11 | Reconnecting | * | WS 重连成功 | LoggedIn | 保持或 session_state | 保持 | 已在线 |
| 12 | Reconnecting | InCall | token grace 耗尽 | LoggedOut/需重登 | Idle | Peers | 连接丢失 |
| 13 | LoggedIn | Idle | 目录某设备 away | LoggedIn | Idle | Peers | 灰显、禁用 Invite |
| 14 | LoggedIn | Idle | 目录移除设备 | LoggedIn | Idle | Peers | 列表更新 |

**#2 要点**：主动 logout 发 WS `goodbye` → 服务端立刻删 session 并广播；**不做** 20s away。异常断线（杀进程/断网）仍走 close + grace + 服务端 ping。

**#7 要点**：拆通话**立刻**（不经 20s roster grace）；Auth 不变。Roster 仍可用 20s `away` 防 Android 切后台闪烁。

**Presence**：客户端仅在 LAN IP / 设备名变化时发 `presence`（本地约 5s 扫描）；在线探活靠服务端 WS ping（20s），不靠业务层定时 presence。

**#9 要点**：`IncomingCallBanner` 底部导火索进度条，`INCOMING_TIMEOUT_MS = 10000`。

### Peer 媒体传输（LAN UDP 抗花屏）

| # | 事件 | 行为 |
|---|------|------|
| M1 | session_start | 双方 TCP listen + `lan_offer{port,udpPort,udpCands}`；同网 host UDP punch |
| M2 | transport=`lan` | **控制**走 LAN TCP；**H.264** 走 UDP MPC2（FEC 4+1 + NACK，重组≤80ms） |
| M3 | transport=`p2p` | WAN STUN + UDP punch；媒体同 MPC2 |
| M4 | 分片丢失 | FEC 补洞 → NACK 重传（≤2）→ 超时 `need_key` / force IDR |
| M5 | 发送 drop-old / 编码丢帧 | `media_broken` → 只发 IDR，抑制 delta |
| M6 | 解码失败 | 冻住上一好帧 + `need_key`，不 clear canvas |

### Page 导航（底部栏 / 横滑 · `PagePager`）

**皇室战争 / ViewPager 单 owner**（复刻 **v0.1.64**）：一个小数 `offset`（`translate3d`，走 compositor）。  
跟手 = 手指直接写 offset；松手 / 底栏点选 = 唯一 `settleTo(T)`（rAF + cubic-bezier）。无 `overflow-x` fling、无 snap 第二条时间线。

| # | 当前 | 事件 | 行为 |
|---|------|------|------|
| C1 | Idle @ n | 横滑过 slop + H | Dragging，跟手写 offset；若正在 settle 则冻在当前小数再跟手 |
| C2 | Dragging | finger↑ → 判定 T | `settleTo(T)` |
| C3 | Settling→A | 底栏点 C | 冻当前 x → `settleTo(C)` |
| C4 | Settling→A | 再横滑 | 冻当前 x → Dragging |
| C5 | Idle / Settling | 底栏点 C | `settleTo(C)` |
| C6 | pending 短触 | 未过 slop | tap-ignore |

### 实现落点

| 层 | 代码 |
|----|------|
| Auth / Call native | `pc/client/src/peer_session.cpp` · `android/.../PeerSession.kt` |
| Roster | `server/server.js` `devicesForUser`（`online` + `state`） |
| Banner #9 | `shared/web/src/components/IncomingCallBanner.tsx` |
| Page 导航 | `App.tsx` `session_end` → `Peers`；`onPeerSessionStart` → `Monitor`；`PagePager` C1–C6（v0.1.64） |
| UI 投影 | `PeerPanel.tsx` |

## Build & release (PC + Server)

```powershell
cd shared\web; npm run build
powershell -File scripts\Build.ps1                    # all native under pc/
powershell -File scripts\Build.ps1 -Module mimic_client

powershell -File scripts\Release.ps1 -DryRun
powershell -File scripts\Release.ps1 -ClientOnly      # PC only (pc/client/src/version.h)
powershell -File scripts\Release.ps1 -AndroidOnly     # Android only (android/version.json)
powershell -File scripts\Release.ps1 -ServerOnly      # server/package.json
powershell -File scripts\Release.ps1 -PublishGitee    # optional: attach thin Setups on Gitee
```

默认只发 **CDN**（旧 Setup 安装后应用内更新即可）。`-PublishGitee` 才会上传 thin Setup 附件。

| Version truth | File |
|---------------|------|
| PC client | `pc/client/src/version.h` → `APP_VERSION` |
| Server | `server/package.json` → `version` |
| Android | `android/version.json`（+ gradle `versionName`） |

CDN: `http://47.107.43.5/mimic/client/` · `.../server/` · `.../android/`。

Gitee Release 附件（可选）：`MimicClient_Setup_*.exe` · `MimicServer_Setup_*.exe` · **`MimicAndroid_Setup_*.apk`**。

Android 分发（与 PC 同构）：

```
手机装 MimicAndroid_Setup.apk  →  读 CDN version.json  →  下载 MimicClient_Android.apk  →  系统安装
```

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
