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
│   ├── src/             # capture_dxgi.cpp, window_list.cpp, ...
│   ├── include/         # capture.hpp, preprocess.hpp
│   ├── build/           # *.obj, window_list.exe, capture_test.exe
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
│   ├── src-tauri/       # Rust 胶水层 + Cargo.toml
│   │   └── src/main.rs  # 调用 C++ window_list.exe
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
npm run tauri dev           # 开发模式 (热更新)
npm run tauri build         # 生产打包 → .exe
```

### C++ 工具

```bash
cd capture && build.cmd     # 截屏 + 窗口列表
cd input   && build.cmd     # 输入模拟
cd agent   && build.cmd     # 智能体
```

## 架构

```
┌─ monitor_web (Tauri 2) ──────────────────────────┐
│  React (TypeScript + Tailwind)  ←→  Rust (IPC)   │
│       UI 界面                     调用 C++ 工具    │
└──────────────────────────┬───────────────────────┘
                           │ subprocess
          ┌────────────────┼────────────────┐
          ▼                ▼                ▼
   window_list.exe   capture_dxgi    input_sendinput
   (窗口枚举)         (屏幕截图)       (输入模拟)
          │                │                │
     EnumWindows       DXGI/GDI        SendInput
     + DwmGetWindow    Desktop Dup     Interception
```

## 可执行文件

| 文件 | 用途 |
|------|------|
| `game/main.exe` | 井字棋 TUI 游戏 |
| `capture/build/window_list.exe` | 窗口枚举 (JSON 输出) |
| `capture/build/capture_test.exe` | 截屏测试 |
| `input/build/input_test.exe` | 输入测试 |
| `agent/build/agent.exe` | AI Agent |
| `monitor_web/src-tauri/target/debug/game-agent-monitor.exe` | 监控面板 |

## 监控面板功能

- **Select Window** → C++ EnumWindows + DwmGetWindowAttribute, 分类 Desktop / Window / Process
- **单帧截图** → Rust GDI capture → PNG base64, 点击 Screenshot 面板 📷 按钮
- **Preview** → 实时截屏预览 20 FPS (TODO: 接入 DXGI)
- **Log** → 实时操作日志, 全局共享状态
- **Config** → 模型服务器 + 游戏窗口配置
- **Tooltip** → 自定义悬停气泡, 300ms 延迟, 持续显示, TypeScript title: string 强制必填
- **ActionBtn / IconBtn** → 统一组件, title 编译时强制检查

## 运行

```bash
# 井字棋
cd game && ./main.exe

# 监控面板 (开发, 需要 Vite HMR)
cd monitor_web && npm run tauri dev

# 监控面板 (发布版, 自包含 exe, 无需网络)
cd monitor_web && npm run tauri build
./src-tauri/target/release/game-agent-monitor.exe

# C++ 工具
cd capture && build.cmd          # window_list.exe + capture_test.exe
cd input   && build.cmd          # input_test.exe
cd agent   && build.cmd          # agent.exe

# 训练 AI
cd ai && python train.py --iters 50 --games 100
cd game && ./main.exe --server 127.0.0.1 9999 --auto --games 5000
```

## 技术栈

- **C++**: DXGI截屏, Interception输入, EnumWindows, MSVC 2022
- **Rust**: Tauri 2 IPC 胶水层
- **React 19 + TypeScript + Tailwind CSS**: UI 界面 (MaaEnd/MXU 风格)
- **Python**: PyTorch 模型训练, ONNX 推理
