本文以一个端到端的实操路径，带你从零开始编译并运行这个项目最核心、最简单的功能模块——**井字棋（TicTacToe）游戏**与**AI自弈训练系统**。你将亲眼看到：一个用C++编写的终端棋盘游戏，通过TCP网络协议与一个Python训练的神经网络模型对弈，并且AI能通过自我对弈（Self-Play）从随机下子逐步学会下赢井字棋。

本文聚焦"怎么做"——所有依赖、命令、预期输出一目了然。读完本文，你会拥有一个可以运行、可以观察、可以魔改的AI demo；关于每一项背后的架构思想，请查阅目录中对应的深入页面。

Sources: [README.md](README.md#L1-L129), [game/src/main.cpp](game/src/main.cpp#L1-L16)

---

## 一、你会得到什么

运行成功后，你将获得以下三种模式：

| 模式 | 命令行参数 | 玩法说明 |
|------|-----------|---------|
| **人人对战** | 无参数 | 两人轮流在终端用方向键+回车下棋 |
| **人机对战** | `--server 127.0.0.1 9999` | 人类用方向键下棋，AI自动响应走子 |
| **AI自弈训练** | `--server 127.0.0.1 9999 --auto --games 5000` | 两个AI左右互搏，边下边学，模型自动优化 |

核心数据流如下：

```mermaid
flowchart LR
    subgraph "C++ 游戏进程 (game/main.exe)"
        A[棋盘状态<br/>9格 + 执棋方] -->|TCP<br/>纯文本行协议| B[AI 预测服务]
    end

    subgraph "Python 进程 (ai_server.py / train.py)"
        B --> C{模型推理}
        C --> D[策略 logits + 价值评分]
        D -->|TCP<br/>"row col value"| A
    end

    A --> F[落子 → 棋盘更新]
    F --> G{胜负判定}
    G -->|对局结束| H[send_end]
    H --> I[构建训练样本<br/>(state, policy, value)]
    I --> J[梯度下降训练]
    J --> C
```

**整个流程不依赖屏幕捕获、不依赖输入模拟**——它直接用TCP协议传递棋盘状态，是理解本项目"四层架构"最轻量级的入口。

Sources: [game/src/main.cpp](game/src/main.cpp#L249-L281), [ai/ai_server.py](ai/ai_server.py#L1-L214)

---

## 二、前置准备

### 2.1 系统要求

| 组件 | 版本要求 | 备注 |
|------|---------|------|
| Windows | 10 或 11 | 本项目专为 Windows 设计 |
| Visual Studio | 2022（含 C++ 工具链） | 需要 `cl.exe` 和 MSVC 编译器 |
| Python | 3.9+ | 用于运行 AI 服务 |
| PyTorch | 2.0+ | 神经网络框架 |

### 2.2 验证环境

打开**开发者命令提示符**（Visual Studio 自带的 "Developer Command Prompt" 或 "x64 Native Tools Command Prompt"），执行：

```cmd
cl
```

如果看到类似 `Microsoft (R) C/C++ Optimizing Compiler ...` 的信息，说明 C++ 编译器可用。

再验证 Python：

```cmd
python --version
pip install torch --index-url https://download.pytorch.org/whl/cpu
```

> **提示**：如果只想跑自弈训练（AI vs AI），PyTorch 安装 CPU 版即可，不需要 CUDA。井字棋模型极小（9维输入→3层全连接→128隐藏层），CPU 推理单步 < 50ms。

> **关于 MSVC 编译器路径**：项目中的 `build.cmd` 脚本会自动调用 Visual Studio 的环境初始化脚本 `vcvars64.bat`。如果你安装的 Visual Studio 版本不是 2022（即目录中的 `18` 可能不匹配），请根据你的实际安装路径修改对应 `build.cmd` 中的 `call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"` 路径号。

Sources: [game/build.cmd](game/build.cmd#L1-L10), [ai/requirements.txt](ai/requirements.txt#L1-L2)

---

## 三、第一步：编译井字棋游戏

整个项目在根目录有一个统一 `Makefile`。编译游戏最简单的方式是在项目根目录下执行：

```cmd
make game
```

这条命令等价于手动执行 `game\build.cmd`，它会做以下几件事：

1. 调用 `vcvars64.bat` 设置 MSVC x64 编译环境
2. 创建 `game/build/` 目录
3. 用 `cl.exe` 编译以下源文件：
   - `src/main.cpp` — 游戏主循环，含三种模式调度
   - `src/config.cpp` — 命令行参数解析
   - `src/board.cpp` — 棋盘数据结构与胜负判定
   - `src/network.cpp` — TCP 客户端，向 AI 服务发送/接收数据
   - `src/tui.cpp` — 终端原始模式（raw mode）初始化
   - `../common/src/signals.cpp` — Ctrl+C 信号处理
4. 链接 `ws2_32.lib`（Windows Socket API）
5. 输出 `game/build/main.exe`

编译成功后你会看到：

```
Build OK: game\main.exe
```

你也可以直接运行这个 exe 验证编译是否成功：

```cmd
game\build\main.exe
```

这会直接进入**人人对战**模式——用方向键移动光标、回车落子。按 `Esc` 退出。

项目的 `Makefile` 也支持一步编译所有模块（game + capture + input + agent + monitor），但当前我们只需要 `game`：

```cmd
make all        # 编译全部
make game       # 仅编译游戏（推荐）
```

Sources: [Makefile](Makefile#L1-L35), [game/build.cmd](game/build.cmd#L1-L10), [game/src/main.cpp](game/src/main.cpp#L249-L281)

---

## 四、第二步：运行 AI 预测服务 — 人机对弈

编译成功后，让人类与 AI 下棋只需两步。

### 4.1 启动 AI 服务（新终端窗口）

打开**第二个**终端窗口（普通 cmd 即可，不需要开发者提示符），进入 `ai/` 目录，启动 AI 预测服务：

```cmd
cd ai
python ai_server.py --model model.pkl --port 9999
```

由于项目尚未训练出 `model.pkl` 文件，AI 服务会**自动使用随机初始化的权重**运行。这意味着 AI 现在还"不会"下棋——它会随机落子。

```
AI 服务器启动在 127.0.0.1:9999
[WARN] 模型文件不存在: model.pkl。使用随机初始化权重。
等待 C++ 游戏连接...
```

### 4.2 启动游戏（原终端窗口）

回到第一个终端窗口（或再开一个），运行：

```cmd
game\build\main.exe --server 127.0.0.1 9999
```

各参数含义：

| 参数 | 值 | 说明 |
|------|-----|------|
| `--server` | `127.0.0.1 9999` | 连接到本地 9999 端口的 AI 服务 |
| `--ai` | `X`（默认） | AI 执 X 方；若不指定，默认 AI 执 X |
| `--ai O` | `O` | 让 AI 执 O 方，人类执 X |

> **注意**：如果你希望人类执 X（先手）、AI 执 O（后手），请加 `--ai O`：`game\build\main.exe --server 127.0.0.1 9999 --ai O`

游戏启动后，你会看到带方向键控制的彩色 TUI 界面：

- **方向键**移动光标
- **回车**落子
- **Esc** 退出
- 光标在空格上时**反色闪烁**，在已有棋子格子上时**白色高亮**提示该格已被占

**通信协议过程**（理解数据流的关键）：

1. 人类落子后，轮到 AI 时，游戏通过 TCP 发送：`"<9个棋盘值> <执棋方>\n"`
   - 棋盘值：`+1`=X, `-1`=O, `0`=空格
   - 例如：`"1 -1 0 0 0 0 0 0 0 1\n"` 表示 X 在左上角、O 在正上、当前 X 走
2. AI 服务推理后返回：`"row col value\n"`
   - 例如：`"1 1 0.1234\n"` 表示推荐落子(1,1)，局面评分 0.12
3. 收到 AI 步后，游戏在终端打印：`"AI plays 1 1 (0.12)"` 并更新棋盘
4. 对局结束，游戏发送：`"END winner\n"`（`1`=X胜, `-1`=O胜, `0`=平局）

> **提示**：因为此时模型是随机权重，AI 的每一步本质上是随机落子，你的获胜难度约等于与随机点击空格的人对弈——这对人类来说太简单了。但恭喜你，你已经跑通了一套完整的"游戏↔AI"网络通信架构。

Sources: [ai/ai_server.py](ai/ai_server.py#L1-L214), [game/src/network.cpp](game/src/network.cpp#L62-L127), [game/src/config.cpp](game/src/config.cpp#L1-L56), [game/src/main.cpp](game/src/main.cpp#L200-L248)

---

## 五、第三步：运行 AI 自弈训练

现在是见证奇迹的时刻——让 AI 通过自我对弈从零学会下棋。

### 5.1 启动训练服务

打开一个终端，进入 `ai/` 目录：

```cmd
cd ai
python train.py --model model.pkl --port 9999 --iters 50 --games 100
```

各参数含义：

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `--model` | `model.pkl` | 模型权重文件路径（不存在则随机初始化） |
| `--save` | `model.pkl` | 训练完成后保存路径 |
| `--port` | `9999` | AI 服务监听端口 |
| `--iters` | `50` | 迭代轮数（每轮收集数据→训练→衰减探索率） |
| `--games` | `100` | 每轮收集的对弈局数 |
| `--epochs` | `5` | 每轮对数据训练的 epoch 数 |
| `--batch-size` | `64` | 训练 batch 大小 |
| `--lr` | `0.001` | 学习率 |
| `--eps-start` | `0.8` | 初始探索率（80% 概率随机走） |
| `--eps-end` | `0.05` | 最终探索率 |
| `--eps-decay` | `0.995` | 每轮探索率衰减系数 |
| `--hidden` | `128` | 神经网络隐藏层宽度 |

启动后会看到：

```
训练服务已启动: 127.0.0.1:9999
等待 C++ 游戏连接... (需 100 局/轮, 共 50 轮)
Iter  Games    Eps     Loss      Pol       Val
-------------------------------------------------------
```

### 5.2 启动自弈游戏

保持训练服务运行，打开**另一个**终端窗口，运行：

```cmd
game\build\main.exe --server 127.0.0.1 9999 --auto --games 5000
```

参数说明：

- `--auto`：等价于 `--ai B`，表示 AI 同时控制双方（Black = Both）
- `--games 5000`：最多进行 5000 局对弈
- `--delay 0`：每步间隔（秒），默认 0
- `--game-delay 1`：每局之间间隔（秒），默认 1

自弈模式是**纯文本模式**（无 TUI 界面），每局输出类似：

```
Player X > AI (X): 0 1 (0.0234)
AI (O): 1 1 (-0.0156)
...
X wins!
Player X > AI (X): 2 2 (0.0341)
...
```

每一行 `AI (X): r c (v)` 表示 AI（控制 X 方）落子位置和该局面的价值评分。

### 5.3 训练过程解析

训练循环的运作方式：

1. **数据收集阶段**：`game/main.exe` 进行自弈，每次落子请求都发到 `ai_server.py`，服务器记录每一步的 (棋盘状态, 落子位置, 执棋方)。对局结束后发送 END 消息，服务端根据胜负结果为每一步计算价值目标（`+1`=胜方步, `-1`=负方步, `0`=平局步）
2. **训练阶段**：每凑齐 100 局（`--games 100`），服务端取出所有数据，用策略梯度目标训练 5 个 epoch
3. **探索率衰减**：训练结束后，`epsilon` 乘以 `0.995`，下一轮 AI 更倾向于选择模型推荐的走法而非随机走法

训练过程中的终端输出示例（共 50 轮）：

```
Iter  Games    Eps     Loss      Pol       Val
-------------------------------------------------------
    1    3700  0.8000   1.8200   1.5100   0.3100
   10   37000  0.7638   1.4500   1.1200   0.3300
   20   74000  0.7268   1.2100   0.8900   0.3200
   30  111000  0.6919   1.0500   0.7400   0.3100
   40  148000  0.6586   0.9500   0.6500   0.3000
   50  185000  0.6270   0.8800   0.5800   0.3000
```

观察指标：
- **Loss** 下降 → 模型在收敛
- **Pol（策略损失）** 下降 → 模型越来擅长选择正确位置
- **Val（价值损失）** 保持在 0.3 左右 → 井字棋平局率高，价值预测天然存在不确定性

训练完成后，`model.pkl` 会被保存到 `ai/` 目录下。

> **预期结果**：5000 局（50轮×100局）后，模型应该学会了基本的井字棋策略——占据中心、堵对手连胜、制造双线威胁。人类已经不容易赢它了。如果需要更强的 AI，可以调大 `--iters 200 --games 500`。

Sources: [ai/train.py](ai/train.py#L1-L124), [ai/ai_server.py](ai/ai_server.py#L1-L214), [game/src/main.cpp](game/src/main.cpp#L249-L281)

---

## 六、第四步：用训练好的模型进行人机对战

训练完成后，用 `ai\run_play.bat` 一键启动人机对战：

```cmd
cd ai
run_play.bat
```

这个批处理脚本会做三件事：

1. 在新窗口中启动 `python ai_server.py --model model.pkl --port 9999`
   - 此时 `model.pkl` 已经训练完毕，AI 不再是随机水平
2. 等待 2 秒确保服务就绪
3. 启动 `game\main.exe --server 127.0.0.1 9999 --ai X`
   - AI 执 X（先手），人类执 O（后手）

如果你想手动精细控制，也可以分步执行：

```cmd
:: 终端1：启动 AI 服务（加载训练好的权重）
cd ai
python ai_server.py --model model.pkl --port 9999

:: 终端2：启动游戏（人类执X，AI执O）
game\build\main.exe --server 127.0.0.1 9999 --ai O
```

现在和你对弈的是一个经过数千局自弈训练的 AI，虽然不如 AlphaGo 那样强大，但对付井字棋已经足够——它不会再随意落子，而是会抢占中心、制造双线威胁、堵你的连珠。

Sources: [ai/run_play.bat](ai/run_play.bat#L1-L19), [ai/ai_server.py](ai/ai_server.py#L100-L129)

---

## 七、AI 模型结构速览（理解你的对手）

训练的核心神经网络 `TicTacToeNet` 定义在 `ai/net.py` 中，是标准的**MLP（多层感知机）+ 双头输出**架构：

```
输入层 (9)          ← 棋盘9格，从当前执棋方视角编码：+1=我方, -1=对方, 0=空
  ↓
全连接 + ReLU (128)  ← 第1隐藏层
  ↓
全连接 + ReLU (128)  ← 第2隐藏层
  ↓
全连接 + ReLU (64)   ← 第3隐藏层（压缩）
  ↓                  ↘
策略头 (9)           价值头 (1)
  ↓                  ↓
logits (未softmax)   tanh 压缩 [-1, 1]
  ↓                  ↓
落子概率分布         局面评分
```

- **策略头**：输出 9 个 logits，经 softmax 后得到每个格子的落子概率
- **价值头**：输出一个标量，`+1`=当前方必胜, `-1`=必败, `0`=平局

训练时，损失函数 = 策略交叉熵 + 价值均方误差（MSE）。从每一局对弈历史中，模型学到"什么位置该下"和"当前局面好不好"两个任务。

Sources: [ai/net.py](ai/net.py#L1-L68), [ai/train.py](ai/train.py#L1-L67)

---

## 八、常见问题与故障排除

### 编译失败

| 问题 | 检查点 |
|------|--------|
| `'cl' 不是内部或外部命令` | 未在开发者命令提示符中运行，或 VS 2022 未安装 C++ 工具链 |
| `vcvars64.bat` 找不到 | VS 安装路径非默认，修改 `build.cmd` 中的路径 |
| `fatal error C1083` 找不到头文件 | 确认 `include/` 目录和 `../common/include/` 存在 |
| `unresolved external symbol` | 确认链接了 `ws2_32.lib` |

### AI 服务连接失败

| 问题 | 检查点 |
|------|--------|
| `Cannot connect to AI server` | 确认 `ai_server.py` 或 `train.py` 已在运行 |
| `[WinError 10061] 连接被拒绝` | 端口号不匹配（游戏默认 `9999`，服务端也是 `9999`） |
| AI 服务启动后立即退出 | 检查 `ai/` 目录下是否安装了 PyTorch |

### 训练异常

| 现象 | 原因与解决 |
|------|-----------|
| Loss 不下降 | 学习率过低，尝试 `--lr 0.01`；或探索率衰减太快 |
| AI 总是走同一个位置 | epsilon 降到了 0 且模型尚未学会探索，检查 `--eps-end` 值 |
| 训练速度极慢 | 使用 CPU 版本 PyTorch，单步约 50ms，5000 局约需 5-10 分钟 |
| `model.pkl` 未生成 | 确认训练循环正常结束，或使用 `Ctrl+C` 中断时会自动保存 |

### 自弈模式闪退

如果加 `--auto` 后命令行窗口一闪而过，请在普通 cmd 中手动执行 `game\build\main.exe --server 127.0.0.1 9999 --auto --games 5000` 查看具体错误信息。

Sources: [game/build.cmd](game/build.cmd#L1-L10), [game/src/network.cpp](game/src/network.cpp#L62-L127), [ai/train.py](ai/train.py#L1-L124)

---

## 九、下一步阅读

你已经完成了整个流水线中最核心的闭环：**编译游戏 → 启动 AI 服务 → 人机对战 → 自弈训练 → 模型收敛**。这是理解整个项目"像素输入，动作输出"思想的最轻量级入口。

如果你对这个流程背后的设计原则感兴趣，推荐按以下顺序深入：

- **[设计哲学：四层架构](3-she-ji-zhe-xue-si-ceng-jia-gou-you-xi-ben-ti-ping-mu-bu-huo-shu-ru-mo-ni-aimo-xing-yu-ling-rustchun-c-ce-lue)** — 为什么 game、capture、input、AI 要严格分离？为什么零 Rust 纯 C++？
- **[井字棋游戏逻辑](6-you-xi-luo-ji-3x3qi-pan-gui-ze-sheng-fu-pan-ding-yu-kong-zhi-tai-zhong-duan-tuijiao-hu)** — 棋盘规则、胜负判定、TUI 渲染的详细实现
- **[网络对战协议](7-wang-luo-dui-zhan-xie-yi-chun-wen-ben-xing-xie-yi-9qi-ge-zhi-qi-fang-fan-hui-xing-lie-zhi-chi-ren-ren-human-vs-ai-ai-vs-aisan-chong-mo-shi)** — 你刚用过的纯文本行协议的完整规范
- **[MLP 模型详解](15-jing-zi-qi-mlpmo-xing-9wei-shu-ru-3ceng-quan-lian-jie-ce-lue-tou-9-logits-jie-zhi-tou-tanh-1-1-ke-50msnei-cputui-li)** — 刚训练的神经网络架构深入解析
- **[自弈训练系统](16-zi-yi-xun-lian-xi-tong-ai_server-py-game-main-exe-lian-diao-epsilon-greedytan-suo-ce-lue-ti-du-die-dai-500lun-shou-lian)** — epsilon-greedy 探索、策略梯度、迭代收敛的完整原理

想体验完整"视觉AI"闭环（屏幕捕获+输入模拟）？请继续阅读 **[Agent 主循环管线](13-agentzhu-xun-huan-guan-xian-bu-huo-yu-chu-li-tcpfa-song-jie-shou-dong-zuo-ling-pai-jie-ma-zhi-xing-shu-ru)**，了解 agent 如何从捕获屏幕像素到模拟鼠标操作的完整流程。