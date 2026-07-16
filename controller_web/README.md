# GAM Web Controller

Cross-platform remote control UI (phone / another PC). Talks to the Windows agent via **TCP FRAM** (H.264 out, JSON actions in). Browsers cannot open raw TCP, so a small Python bridge exposes WebSocket.

## Flow

```
Browser (controller_web) --WS:9997--> bridge.py --TCP:9999--> monitor_app
         WebCodecs H.264 decode              CONTROL_MSG JSON
```

## Run

1. Start `monitor_app`, select a target, start **Preview** (stream must be running).
2. Bridge:

```powershell
pip install websockets
python controller_web\bridge.py
```

3. Serve UI (another terminal):

```powershell
cd controller_web
python -m http.server 8080
```

4. Open `http://<pc-ip>:8080` on phone or another PC → Connect `ws://<pc-ip>:9997`.

## Protocol (TCP :9999)

| Direction | type_tag | Body |
|-----------|----------|------|
| Agent → controller | `2` H264 | `[w:4][h:4][flags:4][res:4][Annex-B NAL…]` flags bit0=keyframe |
| Controller → agent | `3` CONTROL | UTF-8 JSON action (`mousedown` / `mouseup` / `move` / `keydown` / …) |

Agent **forces** `hwnd` + method from the active stream target:

- desktop → foreground `sendinput`
- window → background `sendmessage`, coords must be in `[0,1]`
