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
│   ├── main.exe         # 可执行文件
│   └── build.cmd        # MSVC 构建
│
├── capture/             # C++ 屏幕截图 + 窗口枚举
│   ├── src/             # capture_single.cpp, capture_dxgi.cpp, window_list.cpp, process_list.cpp
│   ├── include/         # capture.hpp, preprocess.hpp
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
│   ├── src/             # App.tsx, index.css
│   ├── src-tauri/       # Rust IPC 胶水层
│   │   └── src/main.rs  # 调用 C++ 子进程, PNG/base64 编码
│   └── package.json
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
├── Makefile
└── README.md
```

## 构建

### 井字棋游戏

```bash
cd game && build.cmd
./main.exe                  # 人vs人 TUI (方向键操作)
```

### 监控面板 (桌面应用)

```bash
cd monitor_web
npm install
npm run tauri dev           # 开发模式 (Vite HMR 热更新, React 改动即时生效)
npm run tauri build         # 生产打包 → .exe
```

### C++ 工具

```bash
cd capture && build.cmd     # capture_single, window_list, process_list
cd input   && build.cmd     # 输入模拟
cd agent   && build.cmd     # 智能体
```

## 架构

```
┌─ monitor_web (Tauri 2) ────────────────────────────┐
│  React (TypeScript + Tailwind)  ←→  Rust (IPC)    │
│       UI 界面                    调用 C++ 子进程    │
└───────────────────────────┬────────────────────────┘
                            │ stdout/stderr
           ┌────────────────┼────────────────────┐
           ▼                ▼                     ▼
    window_list.exe   capture_single.exe   process_list.exe
    (任务栏窗口)       (截图: raw BGRA)      (所有可见窗口)
           │                │
      EnumWindows     DXGI / PrintWindow
      + DwmGetAttr    + PW_RENDERFULLCONTENT
                      GDI fallback
```

**截图数据流**:
```
C++ capture_single.exe  →  raw BGRA pixels (stdout)
Rust (main.rs)          →  scale + BGRA→RGBA + PNG + base64
React (App.tsx)         →  <img src="data:image/png;base64,...">
```

## 可执行文件

| 文件 | 用途 |
|------|------|
| `game/main.exe` | 井字棋 TUI 游戏 |
| `capture/build/window_list.exe` | 任务栏窗口枚举 (JSON) |
| `capture/build/process_list.exe` | 所有可见窗口枚举 (JSON, 按需加载) |
| `capture/build/capture_single.exe` | 单帧截图 (BGRA raw, 支持后台/遮挡) |
| `capture/build/capture_test.exe` | 截屏测试 |
| `input/build/input_test.exe` | 输入测试 |
| `agent/build/agent.exe` | AI Agent |
| `monitor_web/src-tauri/target/debug/game-agent-monitor.exe` | 监控面板 |

## 监控面板功能

- **Select Window** → 双列网格, 分类筛选 Desktop / Window / Process, 搜索 + 刷新
- **后台截图** → C++ PrintWindow + PW_RENDERFULLCONTENT, 支持遮挡/最小化窗口
- **桌面截图** → DXGI (GPU) → GDI 自动回退 (WebView2 兼容)
- **单帧截图** → 点击 📷 按钮, C++ → raw BGRA → Rust PNG/base64 → 前端预览
- **Preview** → 连续截图 @20fps, 实时 FPS 计数
- **IP/Port 分离** → `::` 分隔符自动拆分, 懒人友好
- **ActionBtn** → label ≤10 字符用 `w-20`, >10 用 `min-w-[120px]`
- **Tooltip** → Portal → body, z-index 9999, 智能翻转 + 水平限位
- **Settings** → 连接配置, 主题/配色, 模型上下文, 日志, Links, Credits
- **Log** → 实时操作日志, 全局共享状态

## 运行

```bash
# 井字棋
cd game && ./main.exe

# 监控面板 (开发, Vite HMR 热更新)
cd monitor_web && npm run tauri dev

# 监控面板 (发布版, 自包含 exe)
cd monitor_web && npm run tauri build
./src-tauri/target/release/game-agent-monitor.exe

# C++ 工具
cd capture && build.cmd          # 全部工具

# 训练 AI
cd ai && python train.py --iters 50 --games 100
cd game && ./main.exe --server 127.0.0.1 9999 --auto --games 5000
```

## 截图技术

| 场景 | 首选 | 回退 |
|------|------|------|
| 桌面 (hwnd=0) | DXGI Desktop Duplication | GDI BitBlt (WebView2 GPU 冲突自动触发) |
| 窗口 (hwnd≠0) | PrintWindow + PW_RENDERFULLCONTENT | DXGI 裁剪 → GDI GetWindowDC |

PrintWindow 通过 DWM 直接渲染窗口内容, 即使窗口被遮挡也能截到完整画面。

## 技术栈

- **C++**: DXGI/PrintWindow/GDI 截图, Interception 输入, MSVC 2022
- **Rust**: Tauri 2 IPC 胶水层, PNG/base64 编码 (miniz_oxide)
- **React 19 + TypeScript + Tailwind CSS**: UI (MaaEnd/MXU 风格)
- **Python**: PyTorch 模型训练, ONNX 推理
