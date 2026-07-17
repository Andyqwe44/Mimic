# GAM Web Controller

React + Tailwind UI styled like **Game Agent Monitor**. Served by standalone **`controller_server.exe`** (not embedded in the agent).

## Topology

```
Browser (this UI) --WS--> controller_server --WS--> GAM agent (outbound connect)
  WebCodecs H.264 decode     relay only           CONTROL JSON + config
```

Agent fills **server IP:port** and connects outbound. Browser only talks to the server (LAN or public IP).

## Dev

```powershell
# Terminal 1 — relay + static (after build)
powershell -File scripts\Build.ps1 -Module controller_server
.\controller_server\build\controller_server.exe

# Terminal 2 — optional Vite HMR for UI only
cd controller_web
npm run dev   # then point browser at vite; WS still to :9997
```

## Settings (server UI)

- **Capture**: WGC (DXGI stream not implemented yet)
- **Codec**: H.264
- **Input**: Background PostMessage / Foreground Seize (SendInput)

Changes are pushed to the connected agent as `{"type":"config",...}`.

## Agent gates

On the agent PC:

1. **Connect** to this server  
2. **发送画面** — push H.264  
3. **接受控制** — apply mouse/keyboard from this page  
