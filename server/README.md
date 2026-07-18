# MimicServer (signaling)

Cloud signaling only: account auth, same-account device presence, invite/accept mutex, SDP/LAN candidate relay.
**Does not forward H.264 or input traffic.**
**Is not a peer device** — PC and Android log in as devices; this process only stores accounts and routes signaling.

All installs are **symmetric**. On start the process joins the Bootstrap registry
(`http://47.107.43.5:8443`, override with `MIMIC_BOOTSTRAP`). The machine that owns
the bootstrap public IP becomes `role=bootstrap`; others register via
`POST /api/cluster/join` (Bootstrap reverse-probes `/health`).

## Quick start

```powershell
cd server
npm install
npm start
# http://0.0.0.0:8443  WS: ws://host:8443/ws?token=...
# seeded account: demo / demo  (clients must send passHash, not plaintext)
```

Accounts live in `data/users.json` (created at runtime; salted hash only).

## Password protocol (no plaintext on the wire)

1. **Client** (PC / Android): `passHash = hex(SHA-256(UTF-8(password)))`
2. **Register / login body**: `{ user, passHash, ... }` — **never** send `password`
3. **Server store**: `salt` + `hash = SHA-256(salt + ":" + passHash)`

If the body contains a non-empty `password` field, the server returns `400`.

> Client-side hashing removes plaintext passwords from HTTP bodies. It does **not** replace TLS:
> without HTTPS an attacker can still replay `passHash`. Prefer a TLS reverse proxy in production.

Seeded `demo` / `demo` uses the new protocol. Old clients that POST plaintext `password` must upgrade; old `users.json` rows from the plaintext era will not verify — re-register or delete `data/users.json` to re-seed.

## API

| Method | Path | Body | Result |
|--------|------|------|--------|
| POST | `/api/register` | `{user, passHash}` | `{ok}` |
| POST | `/api/login` | `{user, passHash, deviceId?, deviceName?, lanIps?, platform?, peerProto?}` | `{ok, token, deviceId, user, …}` |
| GET | `/health` | — | `{ok, ver, role, nodeCount, …}` |
| GET | `/api/cluster` | — | `{ok, bootstrap, nodes[]}` |
| POST | `/api/cluster/join` | `{port?, url?}` | `{ok, selfUrl, nodes[]}` |
| POST | `/api/cluster/heartbeat` | `{url}` | `{ok, nodes[]}` |

WebSocket messages: `presence`, `invite`, `invite_accept`, `invite_reject`, `hangup`, `signal`.
Server → client: `hello`, `devices`, `session_state`, `invite`, `session_start`, `session_end`, `signal`, `error`.

`devices` entries: `{ deviceId, deviceName, lanIps, platform, peerProto, online }` — peer devices only.

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
