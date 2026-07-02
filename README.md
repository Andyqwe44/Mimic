# TicTacToe → 通用视觉游戏AI

构建能**自己发现子任务**的通用视觉Agent。模型接口纯粹: **像素进, 动作出**。

## 项目结构

```
tictactoe/
├── common/                  # 共享工具
│   ├── types.hpp            # Rect, sleep_ms
│   └── signals.hpp/cpp      # g_quit_flag, 信号处理
│
├── game/                    # C++ 井字棋 (终端游戏)
│   ├── main.cpp             # 入口
│   ├── config.cpp/h         # 配置 & 命令行解析
│   ├── board.cpp/h          # 棋盘 & 规则
│   ├── network.cpp/h        # TCP 客户端 (Winsock)
│   ├── build.cmd            # MSVC 构建
│   └── Makefile             # MinGW 构建
│
├── capture/                 # C++ 屏幕截图
│   ├── capture.hpp          # ICaptureBackend 抽象接口
│   ├── capture_dxgi.cpp     # DXGI Desktop Duplication (1-2ms)
│   ├── capture_gdi.cpp      # GDI BitBlt 回退 (5-10ms)
│   ├── preprocess.cpp/h     # resize/gray/normalize/stack → 84x84
│   ├── test_capture.cpp     # 测试程序
│   └── build.cmd
│
├── input/                   # C++ 输入模拟
│   ├── input.hpp            # IInputBackend 抽象接口
│   ├── input_sendinput.cpp  # SendInput 默认后端
│   ├── input_interception.cpp # Interception 驱动 (绕过反作弊)
│   ├── test_input.cpp       # 测试程序
│   └── build.cmd
│
├── agent/                   # C++ 智能体
│   ├── agent.hpp            # AgentConfig
│   ├── agent.cpp            # capture → send → recv → act 主循环
│   ├── action_mapper.cpp/hpp # 通用动作映射 (token → 鼠标/键盘)
│   ├── main.cpp             # CLI 入口
│   └── build.cmd
│
├── monitor_slint/           # Slint 监控面板 (总控入口)
│   ├── appwindow.slint      # 声明式 UI (暗色主题, 4标签页)
│   ├── main.cpp             # C++ 入口 (进程管理, 一键启动/停止)
│   ├── build.cmd            # 一键构建
│   └── slint_bin/           # Slint 1.17 SDK (首次需下载)
│
├── model/                   # Python 模型 (通用, 游戏无关)
│   ├── generic_agent.py     # L3 通用Agent (~947K 参数)
│   ├── hierarchical.py      # L1+L2 层次化 (~116K 参数)
│   ├── action_space.py      # 通用动作空间 (256 token 词汇)
│   └── __init__.py
│
├── ai/                      # 现有 Python AI
│   ├── net.py               # TicTacToeNet (MLP 26K 参数)
│   ├── model.py             # 模型封装
│   ├── ai_server.py         # TCP 预测服务
│   ├── api.py               # 工具函数
│   ├── train.py             # 训练编排
│   └── requirements.txt
│
├── train/                   # 训练数据工具
│   └── data_collector.py    # MLP 自对弈 → (截屏, 动作) 数据采集
│
├── Makefile                 # 根 Makefile
└── README.md
```

## 环境要求

- **Windows 10+** (DXGI 需要 Win8+)
- **Visual Studio 2022 Community** (含 MSVC)
- **Python 3.10+** + PyTorch 2.x
- **Slint 1.17 SDK** (监控面板需要, 构建脚本自动处理)

## 构建

系统已安装 MSVC 2022 (`C:\Program Files\Microsoft Visual Studio\18\`).
每个模块目录下有 `build.cmd`，双击或命令行运行即可。

### 一键构建全部

```bash
cd game      && build.cmd    # 井字棋游戏
cd capture   && build.cmd    # 截屏工具
cd input     && build.cmd    # 输入模拟工具
cd agent     && build.cmd    # 智能体
cd monitor_slint && build.cmd  # 监控面板
```

### 监控面板额外步骤 (首次)

```bash
cd monitor_slint

# 1. 下载 Slint SDK (一次性)
#    浏览器打开: https://github.com/slint-ui/slint/releases
#    下载: Slint-cpp-1.17.0-win64-MSVC-AMD64.exe
#    双击运行, 解压到: monitor_slint/slint_bin/

# 2. 构建
build.cmd
```

### Python 依赖

```bash
pip install torch>=2.0 onnx onnxruntime numpy opencv-python
```

## 可执行文件

| 文件 | 用途 | 用法 |
|------|------|------|
| `game/main.exe` | 井字棋终端游戏 | `./main.exe` (人vs人) / `--server IP PORT` (人vsAI) / `--auto` (机vs机训练) |
| `capture/capture_test.exe` | 截屏测试 | `./capture_test.exe` 全屏 / `./capture_test.exe "窗口标题"` |
| `input/input_test.exe` | 输入模拟测试 | `./input_test.exe` 查看后端 / `./input_test.exe --execute` 实际点击 |
| `agent/agent.exe` | AI Agent | `./agent.exe --window "窗口标题"` 视觉采集 / 加 `--dry-run` 不执行输入 |
| `monitor_slint/monitor.exe` | 监控面板 (总控入口) | `./monitor.exe` GUI, 一键启动/停止任务 |

## 使用方法

### 1. 玩井字棋 (人类 vs 人类)

```bash
cd game
./main.exe
```

### 2. 训练 AI

```bash
# 终端1: 启动训练服务器
cd ai
python train.py --iters 50 --games 100

# 终端2: 启动自对弈客户端
cd game
./main.exe --server 127.0.0.1 9999 --auto --games 5000
```

### 3. 人机对战

```bash
# 终端1: AI 服务
cd ai
python ai_server.py --model model.pkl

# 终端2: 游戏 (AI=X)
cd game
./main.exe --server 127.0.0.1 9999
```

### 4. 测试截屏

```bash
cd capture
./capture_test.exe              # 全屏截取
./capture_test.exe "记事本"      # 指定窗口
```

### 5. 测试输入模拟

```bash
cd input
./input_test.exe                # 测试后端激活
./input_test.exe --execute      # 实际移动鼠标点击 (3秒延迟!)
```

### 6. 启动监控面板 (总控入口)

```bash
cd monitor_slint
./monitor.exe
```

MAA-Meow 风格统一入口: 打开 → Config 配置路径 → Monitor 点 Start → 自动拉起 game.exe + ai_server.py → 实时监视 → Stop 全部终止。

4 个标签页: Monitor(任务控制+实时画面) / Log(带时间戳事件日志) / Config(可执行文件路径+服务器配置) / Settings(版本+关于)

### 7. 启动 Agent (视觉采集模式)

```bash
cd agent
./agent.exe --window "Tic Tac Toe" --verbose
# 加 --dry-run 只采集不执行输入 (安全调试)
```

## 井字棋命令行参数

```
game/main.exe [选项]

模式:
  (无参数)                        人类 vs 人类
  --server HOST PORT              人机对战 (AI=X)
  --server HOST PORT --ai O       人机对战 (AI=O)
  --server HOST PORT --auto       AI vs AI (训练)

选项:
  --delay N         步间延迟(秒)
  --game-delay N    局间延迟(秒, 默认1)
  --games N         自动模式总局数
  --help            帮助
```

## Agent 命令行参数

```
agent/agent.exe [选项]

  --window TITLE     游戏窗口标题 (必填)
  --server HOST:PORT AI 服务器地址 (默认 127.0.0.1:9999)
  --interval MS      帧间隔 ms (默认 100)
  --games N          最大局数 (默认无限)
  --verbose          显示每帧延迟
  --dry-run          只采集不执行输入
  --help             帮助
```

## 协议

```
C++ → Python:  "b0 b1 ... b8 player\n"    (棋盘状态 + 执棋方)
Python → C++:  "row col value\n"           (AI 走法 + 评估)
C++ → Python:  "END winner\n"              (终局, 1=X胜 -1=O胜 0=平)
```

## 模型

| 模型 | 参数 | 推理 | 说明 |
|------|------|------|------|
| TicTacToeNet (MLP) | 26K | <0.1ms | 棋盘状态 → 走法, 现有基线 |
| GenericAgent (L3) | 947K | <5ms | 像素 → 动作 token, 通用视觉 |
| Hierarchical (L1+L2) | 116K | <2ms | L1(25K) 感知 → z(16维) → L2(91K) 决策 |

## 实施路线图

- [x] Phase 0: C++ 基础设施 (游戏+截屏+输入+网络)
- [x] Phase 0: 通用 Agent 模型 (pixels → actions)
- [x] Phase 1: 层次化蒸馏 (L1+L2, 信息瓶颈 z)
- [x] 监控面板 (Slint UI, MAA-Meow 风格)
- [ ] 训练数据生成 + 端到端闭环验证
- [ ] Phase 2: 自动子任务发现 + CV 加速器
- [ ] Phase 3: 学习型 Router + 文字接口
- [ ] Phase 4: 跨游戏迁移
