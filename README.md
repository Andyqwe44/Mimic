# TicTacToe → 通用视觉游戏AI

构建能**自己发现子任务**的通用视觉Agent。模型接口纯粹: **像素进, 动作出**。

## 项目结构

```
tictactoe/
├── common/              # 共享 C++ 工具
│   ├── include/         # types.hpp, signals.hpp
│   └── src/             # signals.cpp
│
├── game/                # 井字棋 TUI 游戏
│   ├── src/             # main.cpp, board.cpp, tui.cpp, ...
│   ├── include/         # board.hpp, tui.hpp, ...
│   ├── build/           # *.obj
│   ├── main.exe
│   └── build.cmd
│
├── capture/             # C++ 屏幕截图 + 窗口枚举 + H.264 GPU 编码
│   ├── src/
│   │   ├── capture_single.cpp   # 单帧截图 (PrintWindow/DXGI/GDI)
│   │   ├── capture_dxgi.cpp     # DXGI Desktop Duplication 后端
│   │   ├── capture_stream.cpp   # BMP 帧差流 (老方案)
│   │   ├── capture_h264.cpp     # GPU H.264 编码流 (新方案, 60FPS)
│   │   ├── mf_encoder.cpp       # Media Foundation H.264 硬件编码器
│   │   ├── window_list.cpp      # 任务栏窗口枚举 (JSON)
│   │   └── process_list.cpp     # 所有可见窗口枚举 (JSON)
│   ├── include/
│   │   ├── capture.hpp          # ICaptureBackend 接口
│   │   ├── mf_encoder.hpp       # MfH264Encoder 接口
│   │   └── preprocess.hpp
│   ├── build/           # *.exe
│   └── build.cmd
│
├── input/               # C++ 输入模拟
│   ├── src/             # input_sendinput.cpp, input_interception.cpp
│   ├── include/         # input.hpp
│   ├── build/
│   └── build.cmd
│
├── agent/               # C++ 智能体 (像素→动作)
│   ├── src/             # agent.cpp, action_mapper.cpp
│   ├── include/         # agent.hpp, action_mapper.hpp
│   ├── build/
│   └── build.cmd
│
├── monitor_web/         # Tauri 2 + React 监控面板 (桌面应用)
│   ├── src/
│   │   ├── App.tsx               # 主 UI (600+ lines, Tooltip/IconBtn/ActionBtn/ThemeBtn)
│   │   ├── main.tsx              # React 入口
│   │   └── index.css             # Tailwind + CSS 变量 (暗/亮主题)
│   ├── src-tauri/                # Rust 后端
│   │   ├── src/
│   │   │   ├── main.rs           # 窗口枚举 + GDI 截图 + H.264 流 IPC + fMP4 封装
│   │   │   └── fmp4.rs           # 最小 fMP4 ISOBMFF 构建器 (ftyp+moov+mvex, moof+mdat)
│   │   ├── Cargo.toml            # tauri, windows, serde, chrono, miniz_oxide
│   │   └── tauri.conf.json       # 1200x780 window, devUrl:1420
│   ├── package.json              # react, tailwindcss, lucide-react, clsx, tailwind-merge
│   └── vite.config.ts
│
├── model/               # Python 模型
│   ├── generic_agent.py  # L3 (~947K), hierarchical.py (#116K)
│   └── action_space.py   # 通用动作空间
│
├── ai/                  # Python AI (MLP 文本协议)
│   ├── ai_server.py, train.py, net.py, model.py
│   └── requirements.txt
│
├── train/               # 训练数据采集器
└── README.md
```

## 构建

### C++ 工具

```bash
cd capture && build.cmd     # 全部工具: window_list, process_list, capture_single,
                            #   capture_stream, capture_h264
cd input   && build.cmd     # 输入模拟
cd agent   && build.cmd     # 智能体
```

### 监控面板

```bash
cd monitor_web
npm install
npm run tauri dev           # 开发模式 (Vite HMR, React 即时刷新)
npm run tauri build         # 生产打包 → .exe (自包含, 无网络请求)
```

### Python AI

```bash
cd ai && python train.py --iters 50 --games 100
```

## 架构

```
┌─ monitor_web (Tauri 2) ──────────────────────────────────────┐
│  React (TypeScript + Tailwind)  ←→  Rust (IPC)              │
│       UI 界面                   │  Win32 API 直调 (无子进程)  │
│                                  │  fMP4 封装 (MSE 视频)      │
└──────────────────────┬──────────┴────────────────────────────┘
                       │
           ┌───────────┼──────────────┐
           ▼           ▼              ▼
      Rust EnumWindows  Rust GDI     capture_h264.exe
      (窗口枚举, 0ms)   (单帧截图)   (H.264 GPU 流, 60FPS)
                            │              │
                       GetWindowDC    FramePool/WGC → MF H.264 HW Encoder
                       BitBlt         stdout pipe + TCP :9998
```

### Screenshot (单帧截图) 数据流

```
Rust Win32 API (直调, 无子进程)
  Desktop: GetDC(None) + BitBlt → BGRA (GDI, ~2ms @ 1920x1080)
  Window:  GetWindowDC + BitBlt → BGRA (GDI, ~2ms)
  ↓
Scale (max 640px) + BGRA→RGBA + PNG (miniz_oxide) + base64
  ↓
<img src="data:image/png;base64,...">
```

### Preview (实时预览) 数据流 — GPU H.264 视频

```
capture_h264.exe (持久进程, 只 spawn 一次)
  Desktop: DXGI Desktop Duplication (跳过虚拟显示器) → GDI fallback
  Window:  FramePool (Windows.Graphics.Capture, GPU)
  ↓
MF H.264 硬件编码器 (NVIDIA NVENC / AMD VCE / Intel QSV 通用)
  ↓ [size:4][H.264 NAL units] binary stream
  ├── stdout pipe → Rust → fMP4 (moof+mdat) → base64 → Tauri event
  │   ↓
  │   JS MSE SourceBuffer → <video> (GPU DXVA 解码)
  │
  └── TCP :9998 → agent.exe / 未来消费者 (预留)
```

## 可执行文件

| 文件 | 用途 | 何时调用 |
|------|------|---------|
| `capture/build/capture_h264.exe` | GPU H.264 流式捕获 | Preview 按钮 (持久进程) |
| `capture/build/capture_stream.exe` | BMP 帧差流 (老方案) | 未使用 (保留兼容) |
| `capture/build/capture_single.exe` | 单帧截图 (C++ 独立) | 未使用 (Rust 直调取代) |
| `capture/build/window_list.exe` | 任务栏窗口枚举 | 未使用 (Rust 直调取代) |
| `capture/build/process_list.exe` | 所有可见窗口枚举 | Process 筛选 Tab |
| `monitor_web/src-tauri/target/release/game-agent-monitor.exe` | 监控面板 | 主程序 |

## 截图技术栈

| 操作 | 技术 | 延迟 | 说明 |
|------|------|------|------|
| 窗口枚举 | Rust EnumWindows + DwmGetWindowAttribute | <1ms | 直调 Win32 API, 无子进程 |
| 单帧截图 (桌面) | Rust GDI GetDC(None) + BitBlt | ~30ms | 无 DXGI, 避免虚拟显示器黑屏 |
| 单帧截图 (窗口) | Rust GDI GetWindowDC + BitBlt | ~10ms | 直接读窗口 DC |
| 实时预览 (桌面) | DXGI Desktop Duplication → MF H.264 HW Encoder | 60FPS (目标) | GPU 全链路, 跳过虚拟显示器 |
| 实时预览 (窗口) | FramePool/WGC → MF H.264 HW Encoder | 60FPS (目标) | GPU 纹理 → GPU 编码, 零 CPU 拷贝 |

### 单帧截图回退链 (Rust)

```
Desktop: GetDC(None) + BitBlt
Window:  GetWindowDC + BitBlt
  → 如果纯色 → PrintWindow (PW_RENDERFULLCONTENT, 处理遮挡/最小化)
  → 如果仍纯色 → DXGI 裁剪窗口区域
```

## 性能 (2026-07-05)

| 指标 | 老方案 (exe spawn) | 新方案 (Rust 直调) |
|------|-------------------|-------------------|
| 窗口列表加载 | 5200ms (spawn exe) | 0ms (Win32 直调) |
| 单帧截图 | 5200ms (spawn exe) | 8-37ms (GDI 直调) |
| 实时预览 | BMP 15-25fps | H.264 GPU 60fps (开发中) |
| 编码器 | 无 (原始像素) | MF 硬件 MFT (NVIDIA/AMD/Intel) |

## 监控面板功能

- **Select Window** → 双列网格, 分类筛选 Desktop/Window/Process, 搜索+刷新
- **单帧截图** → Camera 按钮, Rust GDI 直调, 日志显示耗时
- **Preview** → MSE `<video>` + GPU H.264 流, 实时 FPS + 捕获方法覆盖层
- **IP/Port** → `::` 分隔符自动拆分
- **Log** → 最新在上, 固定高度+预留滚动条, 不触发重新排版
- **滚动条** → `scrollbar-gutter: stable` 全局预留空间
- **Settings** → 连接/主题/模型/日志/Links/Credits

## 已知问题

1. **MF H.264 编码器兼容性**: MFTEnumEx 返回的硬件编码器可能不支持 NV12 输入,
   或返回空类型列表 (CLSID ADC9BC80)。需逐个尝试编码器, 不能用第一个。
2. **窗口截图质量**: GetWindowDC 对遮挡/最小化窗口可能截不到内容,
   需实现 PrintWindow 回退链。
3. **process_list.exe 仍慢**: 5s spawn 延迟, 待 Rust 重写。
4. **音频**: TCP 端口预留, 接口未实现。
5. **NV12 转换**: 当前 CPU 路径 (~0.3ms @ 640px), GPU Compute Shader TODO。

## 技术栈

- **C++**: DXGI/FramePool/WGC 屏幕捕获, MF H.264 硬件编码, MSVC 2022
- **Rust**: Tauri 2 IPC, Win32 API 直调 (windows crate), fMP4 ISOBMFF 封装, PNG/base64 (miniz_oxide)
- **React 19 + TypeScript + Tailwind CSS**: MSE `<video>` 预览, Tooltip/IconBtn/ActionBtn 组件
- **Python**: PyTorch 训练, ONNX 推理
