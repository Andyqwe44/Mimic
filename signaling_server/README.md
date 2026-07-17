# MimicServer (signaling)

Cloud signaling only: login, device presence, invite/accept mutex, SDP/LAN candidate relay.
**Does not forward H.264 or input traffic.**

## Quick start (dev)

```powershell
cd signaling_server
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
| GET | `/health` | — | `{ok}` |

WebSocket messages: `presence`, `invite`, `invite_accept`, `invite_reject`, `hangup`, `signal`.

## Cloud install

1. Copy this folder (or Release `MimicServer_vX.zip`) to the VPS.
2. `npm install --omit=dev && node server.js --host 0.0.0.0 --port 8443`
3. Put HTTPS reverse proxy (nginx/caddy) in front; clients use `wss://your.domain/ws`.
4. Open firewall for the proxied port only.

See repo root README for dual-package Release layout.
