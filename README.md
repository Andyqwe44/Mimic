# TicTacToe → 通用视觉游戏AI

构建能**自己发现子任务**的通用视觉Agent。模型接口: **像素进, 动作出**。

## 架构

```
┌─ monitor_web (Tauri 2) ────────────────────────────────┐
│  React (TypeScript + Tailwind)  ←→  Rust (IPC)        │
│       UI 界面                   │  Win32 API 直调      │
│                                  │  TCP server :9999    │
└──────────────────┬───────────────┴─────────────────────┘
                   │           TCP :9999 → agent.exe / Python
     ┌─────────────┼──────────────┐
     ▼             ▼
  Rust            Rust
  EnumWindows     GDI Capture
  (0ms)           (多方法回退链)
```

## 项目结构

```
tictactoe/
├── protocol/                  # 线格式 — C++/Rust/Python 共享
│   ├── protocol.h / .rs / .py
├── common/
│   ├── payload/bgra.hpp       # BGRA 像素打包/解析
│   └── transport/             # 传输层 (pipe, tcp)
├── capture/                   # C++ 屏幕捕获
├── monitor_web/               # Tauri 2 + React 监控面板
│   └── src-tauri/src/
│       ├── main.rs            # Rust 后端
│       ├── protocol.rs        # include! shared protocol
│       ├── payload/bgra.rs    # Rust 应用层
│       └── transport/pipe.rs  # Rust 传输层
├── model/                     # Python
│   └── payload/bgra.py        # Python 应用层 + StreamClient
└── examples/                  # 端到端协议示例
    ├── hello_cpp_send.cpp     # C++ pipe → Rust ✓
    └── hello_python_recv.py   # C++ TCP → Python
```

## 线协议 (protocol/)

```
Frame: [magic:4 "FRAM"][size:4 LE][type_tag:4 LE][payload...]

payload (BGRA, type=1): [w:4][h:4][ch:4][reserved:4][pixels: w*h*ch]

DEFAULT_TCP_PORT = 9999  |  MAGIC = 0x4D415246
```

## 三层解耦

```
应用层 (payload/)   ← pack/unpack BGRA/H264, 不碰传输
协议层 (protocol/)  ← 只有常量和 type tags
传输层 (transport/) ← send(type, bytes) / recv(), 不碰内容
```

## 构建

```bash
cd capture     && build.cmd          # C++ 工具
cd monitor_web
npm install && npm run tauri dev     # 开发 (Vite HMR)
npm run tauri build                  # 生产 .exe
pip install torch numpy opencv-python
```

## 截图技术

### 单帧截图 (Camera)

3 方法回退: `GetWindowDC → PrintWindow(品红检测) → ScreenBitBlt`。
纯色/品红自动回退。返回 PNG + 窗口屏坐标 JSON。按比例定位在 16:9 容器。

### 实时预览 (Preview)

Rust 线程, 无子进程:
- 首帧: 检测最佳方法 → 后续帧: 直接调用 (跳过回退链)
- 帧差跳过, 窗口关闭自动停止
- TCP :9999 广播 BGRA 帧 (多客户端)
- 时间戳: `cap=3500us pack=12us bmp=450us` (每30帧)

### H.264 GPU (未来)

`capture_h264.exe`: FramePool → MF H.264 硬件编码 → pipe/TCP。
AMD 驱动不暴露 MF 编码器 (CLSID ADC9BC80 返回空类型)。

## 性能

| 操作 | 老 (exe spawn) | 新 (Rust 直调) |
|------|---------------|---------------|
| 窗口列表 | 5000ms | 0ms |
| 单帧截图 | 5000ms | 8-37ms |
| 流式捕获 | ~30ms/帧 | 2-30ms/帧 |
| BGRA 打包 | N/A | ~12μs |

## API 示例

**Python 接收帧**:
```python
import sys; sys.path.insert(0, 'model/payload')
from bgra import BgraFrame
# connect TCP :9999, read protocol headers, unpack BGRA payloads
```

**C++ 发送**:
```cpp
#include "common/payload/bgra.hpp"
#include "common/transport/tcp.hpp"
transport::TcpSender tcp; tcp.listen();
auto payload = payload::bgra_pack(pixels, w, h, 4);
tcp.broadcast(PAYLOAD_TYPE_BGRA_FRAME, payload.data(), payload.size());
```
