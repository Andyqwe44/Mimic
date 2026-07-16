# GAM Web Controller

React + Tailwind UI styled like **Game Agent Monitor**. Connects to the agent’s embedded WebSocket (`:9997`).

## Flow

```
Browser (this UI) --WS:9997--> monitor_app
  WebCodecs H.264 decode         CONTROL JSON (needs accept_control gate)
```

## Dev

```powershell
cd controller_web
npm install
npm run dev          # http://0.0.0.0:8080 → connect ws://<host>:9997
```

Production/static is built by `scripts\Build.ps1` into `monitor_app\build(_dev)\controller\` and served by the agent at `http://<host>:9997`.

## Agent gates

1. **发送画面** — start H.264 push  
2. **接受控制** — apply mouse/keyboard from this page  

## Note on latency

Agent must use **hardware** H.264 when possible. Soft 1080p encode was ~8 fps / multi-second lag; soft path now scales to 1280 and drops frames under WS backpressure.
