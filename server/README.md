# MimicServer (signaling)

Cloud signaling only: login, device presence, invite/accept mutex, SDP/LAN candidate relay.
**Does not forward H.264 or input traffic.**

All installs are **symmetric**. On start the process joins the Bootstrap registry
(`http://47.107.43.5:8443`, override with `MIMIC_BOOTSTRAP`). The machine that owns
the bootstrap public IP becomes `role=bootstrap`; others register via
`POST /api/cluster/join` (Bootstrap reverse-probes `/health`).

## Quick start

```powershell
cd mimic_server
npm install
npm start
# http://0.0.0.0:8443  WS: ws://host:8443/ws?token=...
# seeded account: demo / demo
```

## API

| Method | Path | Body | Result |
|--------|------|------|--------|
| POST | `/api/register` | `{user,password}` | `{ok}` |
| POST | `/api/login` | `{user,password,deviceId?,deviceName?,lanIps?}` | `{ok,token,deviceId}` |
| GET | `/health` | — | `{ok, ver, role, nodeCount, …}` |
| GET | `/api/cluster` | — | `{ok, bootstrap, nodes[]}` |
| POST | `/api/cluster/join` | `{port?, url?}` | `{ok, selfUrl, nodes[]}` |
| POST | `/api/cluster/heartbeat` | `{url}` | `{ok, nodes[]}` |

WebSocket messages: `presence`, `invite`, `invite_accept`, `invite_reject`, `hangup`, `signal`.

## Cloud install (foolproof)

1. Run `MimicServer_Setup` (or copy this folder / CDN `server/payload.zip`) onto any VPS.
2. `npm install --omit=dev && node server.js --host 0.0.0.0 --port 8443`
3. Open firewall for `:8443` (or put HTTPS reverse proxy in front).
4. No region config required — the node auto-joins Bootstrap and learns the mesh.

Optional env:

| Env | Meaning |
|-----|---------|
| `MIMIC_BOOTSTRAP` | Override bootstrap URL |
| `MIMIC_PUBLIC_URL` | Force advertised URL (skip echo) |
| `MIMIC_IS_BOOTSTRAP` | Force registry role (`1`) |

Default client URL: `http://47.107.43.5:8443`.
