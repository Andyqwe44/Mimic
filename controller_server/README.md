# controller_server

Standalone HTTP + WebSocket relay for Game Agent Monitor.

```
Browser  --WS-->  controller_server.exe  <--WS--  GAM agent (outbound)
                     | serves www/
```

## Run

```powershell
powershell -File scripts\Build.ps1 -Module controller_server
.\controller_server\build\controller_server.exe
# optional: --port 9997 --root C:\path\to\www
```

Open `http://<host>:9997` in a browser. On the agent PC, enter this host:port in Connection and click Connect, then open the stream / control gates.

Default static root: `<exe_dir>\www` (staged from `controller_web/dist` at build time).
