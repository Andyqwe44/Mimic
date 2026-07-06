# TicTacToe → 通用视觉游戏AI

构建能**自己发现子任务**的通用视觉Agent。模型接口: **像素进, 动作出**。

## 架构

```
┌─ monitor_web (Tauri 2) ──────────────────────────────────┐
│  React (TypeScript + Tailwind)  ←→  Rust (IPC)          │
│       MXU-style UI               │  Win32 API 直调       │
│       Dashboard / Screenshot/Log  │  TCP server :9999     │
└──────────────────┬────────────────┴──────────────────────┘
                   │
     ┌─────────────┼──────────────┐
     ▼             ▼              ▼
  Rust            Rust           TCP :9999
  EnumWindows     GDI Capt.      (agent.exe / Python)
  (0ms)           + WGC GPU       binary frames
```

## 项目结构

```
tictactoe/
├── protocol/                  # 线格式 — C++/Rust/Python 共享
│   ├── protocol.h / .rs / .py
├── common/                    # C++ 共享模块
│   ├── include/
│   │   └── capture_helpers.hpp # 公共辅助 (ScaleBgra, IsSolidColor等)
│   ├── payload/bgra.hpp       # BGRA 像素打包/解析
│   └── transport/             # 传输层 (pipe, tcp)
├── capture/                   # C++ 屏幕捕获
│   ├── include/
│   │   ├── capture_wgc.hpp    # WGC FramePool (GPU)
│   │   └── capture.hpp        # DXGI + GDI 后端
│   └── src/
│       ├── capture_wgc.cpp    # WGC 库实现
│       ├── capture_wgc_main.cpp # WGC CLI (单帧/流)
│       └── capture_dxgi.cpp   # DXGI 后端
├── monitor_web/               # Tauri 2 + React 监控面板
│   ├── src/App.tsx            # 前端 (MXU-style UI)
│   └── src-tauri/src/
│       ├── main.rs            # Rust 后端
│       ├── protocol.rs        # include! shared protocol
│       └── payload/bgra.rs    # Rust 应用层
├── model/                     # Python
│   ├── action_space.py        # 动作词表 + 序列化
│   ├── generic_agent.py       # VisionEncoder + Transformer ActionDecoder
│   ├── hierarchical.py        # L1 感知 + L2 策略推理
│   └── payload/bgra.py        # BGRA 像素打包/解析
├── examples/                  # 端到端协议示例 + Benchmark
│   ├── wgc_bench_send.cpp     # WGC → TCP 基准测试 (C++)
│   ├── wgc_bench_recv.rs      # TCP → 文件 基准测试 (Rust)
│   └── run_bench.bat          # 一键benchmark
└── log/                       # 统一日志目录
    ├── agent_*.log             # Rust (Tauri主进程)
    └── wgc_*.log               # C++ (WGC子进程)
```

## 线协议 (protocol/)

```
Frame: [magic:4 "FRAM"][body_size:4 LE][type_tag:4 LE][body...]

type_tag 1 (BGRA): [w:4][h:4][ch:4][reserved:4][pixels: w*h*ch]

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
cd capture     && build.cmd              # C++ 工具 (含 WGC)
cd monitor_web
npm install && npm run tauri dev         # 开发 (Vite HMR + Cargo watch)
npm run tauri build                      # 生产 .exe
```

## GUI 功能

### 页面
- **Dashboard** — 系统信息、采集管线、更新检查、资源配置
- **Monitor** — Agent 控制（Start/Stop）
- **Log** — 实时日志流（最近100条）
- **Settings** — 连接、主题、模型、日志可折叠卡片

### 右侧控制栏 (MXU-style)
- **Connection** — 窗口选择 + IP/Port，固定宽度布局
- **Log** — 可折叠实时日志，带清空按钮
- **Screenshot** — 实时预览（Canvas RGBA 直显），单帧截图，动态屏幕比例

## 截图技术

### WGC (Windows.Graphics.Capture)
- GPU 加速 FramePool，7ms/帧（140+ FPS 能力）
- 支持后台/遮挡窗口捕获
- 事件驱动，窗口静止时 0% CPU
- 三重缓冲 staging texture，GPU/CPU 流水线重叠

### 单帧截图 (Camera)
3 方法回退: `GetWindowDC → PrintWindow(品红检测) → ScreenBitBlt`

### 实时预览 (Preview)
- 窗口模式: WGC 子进程 → 管道 → Rust → Canvas 渲染
- 桌面模式: DXGI Desktop Duplication → GDI 回退
- BGRA → RGBA 直接转换，无 BMP/base64 格式开销
- TCP :9999 广播帧（多客户端）
- 时间戳: `cap=3500us copy=861us readback=6833us`

### 黄色边框
- 选中窗口后黄色框选叠加
- `SetWinEventHook` 事件驱动跟踪（非轮询）
- 鼠标释放时更新位置，窗口最小化时隐藏
- Z-order 跟随目标窗口

## 性能

| 操作 | 老 (exe spawn) | 新 (Rust 直调 + WGC) |
|------|---------------|---------------------|
| 窗口列表 | 5000ms | 0ms |
| 单帧截图 | 5000ms | 5-54ms |
| WGC 采集 | N/A | 7ms (140+ FPS) |
| BGRA 打包 | ~12μs | ~12μs |
| BMP 编码 | 338ms | 0ms (Canvas RGBA) |

## H.264 GPU (未来)
`capture_h264.exe`: FramePool → MF H.264 硬件编码 → pipe/TCP。
AMD 驱动不暴露 MF 编码器 (CLSID ADC9BC80 返回空类型)。

## AI 模型

### GenericAgent (L3 单体)
CNN Encoder (Nature-style) → Transformer Decoder (自回归动作生成)。
~0.8M–3M 参数。训练用因果 mask + teacher forcing。
输入: 4×84×84 灰度帧栈 → 输出: 动作 token 序列。

### HierarchicalAgent (L1+L2)
L1: 感知专家 — 小 CNN 将像素压缩到 16-dim z（VAE 信息瓶颈）。
L2: 策略推理 — z + 动作历史 → Transformer → 动作 tokens。
端到端训练，Loss = L_task + γ*L_KL。

## 已知限制

- 覆盖窗口崩溃时可能残留（黄色边框 STATIC 窗口无父窗口清理）。
