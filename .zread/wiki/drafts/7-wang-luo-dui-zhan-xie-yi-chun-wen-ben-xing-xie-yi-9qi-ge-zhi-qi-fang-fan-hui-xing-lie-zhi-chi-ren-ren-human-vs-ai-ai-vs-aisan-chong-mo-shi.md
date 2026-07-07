井字棋游戏本体通过一个精炼的纯文本行协议与 AI 推理服务（`ai_server.py`）通信。该协议的设计遵循了最小惊讶原则——每一行都是自描述的 ASCII 字符串，无需二进制解析、无需序列化框架，一条 `send()`/`recv()` 即可完成一次对弈交互。协议同时承载对局数据记录功能，AI 服务端可从请求-响应的历史中自动构建训练样本。

## 协议格式：三合一的消息契约

整个协议仅包含三种消息类型，均由换行符 `\n` 终止，C++ 端使用 `std::ostringstream` 构造、`std::istringstream` 解析，Python 端使用 `str.split()` 处理。并无魔数、版本号或校验和——复杂度被刻意压低到"一行文本"的级别。

**请求消息**格式为 `"b0 b1 b2 b3 b4 b5 b6 b7 b8 player\n"`，共 10 个由空格分隔的整数。前 9 个整数编码棋盘状态：`+1` 代表 X，`-1` 代表 O，`0` 代表空格，按行优先顺序（索引 0-8 对应 (0,0) 到 (2,2)）。第 10 个整数 `player` 指示当前执棋方视角：`+1` 表示轮到 X 走，`-1` 表示轮到 O 走。这个编码设计的关键细节在于——棋盘值始终是绝对编码（+1/-1/0），但 AI 模型内部会将棋盘值乘以 `player` 做视角归一化，使模型永远从"我方"视角看棋盘。Sources: [network.cpp](game/src/network.cpp#L88-L100), [ai_server.py](ai/ai_server.py#L144-L147)

**响应消息**格式为 `"row col value\n"`，三个由空格分隔的值。`row` 和 `col` 是 AI 选择的落子坐标，范围 [0, 2]。`value` 是当前局面的评估值，范围 [-1, 1]，正值表示模型认为对"我方"有利，负值则不利，这也直接来源于 MLP 模型价值头的 tanh 输出。Sources: [network.cpp](game/src/network.cpp#L102-L112), [ai_server.py](ai/ai_server.py#L167-L169)

**结束消息**格式为 `"END winner\n"`，由 C++ 客户端在分出胜负或平局后发送。`winner` 取值：`1` 表示 X 胜，`-1` 表示 O 胜，`0` 表示平局。Sources: [network.cpp](game/src/network.cpp#L81-L84), [ai_server.py](ai/ai_server.py#L131-L137)

```
┌──────────────────────────────────────────────────────────────────┐
│                  纯文本行协议交互流程                              │
│                                                                  │
│  game/main.exe                   ai_server.py                    │
│       │                               │                          │
│       │──── 请求: "0 0 0 0 0 0 0 0 0 1\n" ────►                │
│       │                               │  棋盘全空, X执棋          │
│       │◄─── 响应: "1 1 0.0234\n" ──────────│                   │
│       │     AI落在(1,1), 局面评估0.0234     │                    │
│       │                               │                          │
│       │──── 请求: "0 0 0 0 1 0 0 0 0 -1\n" ──►                 │
│       │                               │  棋盘中心为X, O执棋       │
│       │◄─── 响应: "0 0 -0.1567\n" ─────────│                   │
│       │     AI落在(0,0), 局面评估-0.1567   │                    │
│       │                               │                          │
│       │             ... 继续对弈 ...         │                    │
│       │                               │                          │
│       │──── "END 1\n" ─────────────────────►│                   │
│       │     X获胜, 触发训练样本构建         │                    │
│       │                               │                          │
└──────────────────────────────────────────────────────────────────┘
```

## 三种对战模式：同一协议，不同编排

协议本身是固定的，但 C++ 端通过命令行参数将同样的消息流编排为三种截然不同的游戏体验。模式切换逻辑集中在 `main()` 函数中，由 `g_cfg.ai_player` 标志位驱动。Sources: [main.cpp](game/src/main.cpp#L253-L281)

| 模式 | CLI 参数 | 玩家组成 | 交互方式 | 典型用途 |
|------|----------|----------|----------|----------|
| 人人对战 | 无参数 | Human (X) vs Human (O) | TUI 方向键 + Enter | 本地双人娱乐 |
| Human vs AI | `--server HOST PORT` | Human vs AI (默认 AI=X) | TUI + AI 自动落子 | 人机对战体验 |
| AI vs AI | `--server HOST PORT --auto` 或 `--ai B` | AI (X) vs AI (O) | 纯文本，无 TUI | 自弈训练/数据收集 |

每个人机对弈模式下，C++ 端维护一个状态机：TUI 模式下，人类回合调用 `tui_get_move()` 等待键盘输入；AI 回合调用 `get_ai_move()` 经 TCP 发送请求、获取响应、自动落子。TUI 会实时显示 AI 的选择坐标和评估值：`AI plays 1 1 (0.02)`。Sources: [main.cpp](game/src/main.cpp#L207-L225)

AI-vs-AI 自弈模式（`--auto`）则完全取消了 TUI 渲染，通过 `play_classic()` 函数运行，双方玩家均为 AI，使用同一个 `SOCKET` 连接交替发送请求。该模式支持 `--games N` 限制对局数，和 `--delay N` / `--game-delay N` 控制节奏以便观察。Sources: [main.cpp](game/src/main.cpp#L230-L250), [config.cpp](game/src/config.cpp#L13-L22)

## 服务器端：对弈历史驱动的训练数据管道

`ai_server.py` 的 `_handle_client()` 方法同时是一局对弈的裁判和数据收集器。它在处理每个请求时，将棋盘快照、落子位置、执棋方三元组追加到 `game_history` 列表中。当接收到 `END` 消息并解析出最终的胜者后，遍历历史记录，对每步棋构建一个训练样本 `(state, policy_target, value_target)`：Sources: [ai_server.py](ai/ai_server.py#L89-L137)

- **state**: 长度为 9 的张量，每个格子值乘以 `player` 做视角归一化
- **policy_target**: 长度为 9 的 one-hot 向量，仅在实际落子位置为 `1`
- **value_target**: 标量，等于 `winner * player`，即胜方每步棋的目标价值为 +1，负方为 -1

这个设计的巧妙之处在于——`policy_target` 不是"正确答案"而是"实际走法"，这意味着训练信号来源于自我对弈的实际选择（包括 epsilon-greedy 的随机探索），而非预定义的专家走法。这是策略梯度方法在井字棋尺度上的自然体现。

## 连接生命周期与错误处理

每次对局开始前，C++ 端通过 `connect_to_server()` 建立新的 TCP 连接到 `127.0.0.1:9999`（默认）。`WinsockGuard` RAII 类确保 Windows 上的 WSAStartup/WSACleanup 配对调用，跨平台兼容性通过宏实现。对局结束后立即 `closesocket()` 关闭连接——这意味着服务器端不需要维护会话状态，每个连接是轻量的一次性会话。Sources: [network.cpp](game/src/network.cpp#L14-L64)

错误处理方面，`send_all()` 进行 partial send 的循环重试，`recv()` 失败或收到空数据则直接返回 `false`，C++ 端打印 `"AI error"` 并终止当前对局。TUI 模式下的连接失败会显示引导性错误信息：`Cannot connect to AI server ... -- start 'python ai_server.py' first`，帮助用户快速定位问题。Sources: [network.cpp](game/src/network.cpp#L64-L67)

## 数据收集器的协议复用

`train/data_collector.py` 提供了一个有趣的协议复用案例：它直接使用 TCP socket 向 `ai_server.py` 发送同样的请求格式，但扮演的角色是"旁观者"而非对弈者。数据收集器自行维护棋盘状态数组，逐轮发送请求并记录响应，将 MLP 模型的每一步落子转换为 `action_tokens`（通过 `tictactoe_click_action(row, col)` 编码为通用动作序列），为后续视觉模型蒸馏训练提供标注数据。Sources: [data_collector.py](train/data_collector.py#L71-L100)

## 下一步阅读

协议是井字棋游戏本体与 AI 推理服务之间的桥梁。理解了这条数据流后，可按以下路径深入：

- 若对棋盘逻辑和胜负判定规则感兴趣，请阅读 [游戏逻辑：3x3棋盘规则、胜负判定与控制台终端TUI交互](6-you-xi-luo-ji-3x3qi-pan-gui-ze-sheng-fu-pan-ding-yu-kong-zhi-tai-zhong-duan-tuijiao-hu)
- 若想了解 AI 服务端如何执行模型推理，请阅读 [井字棋MLP模型：9维输入→3层全连接→策略头(9 logits)+价值头(tanh[-1,1])，可50ms内CPU推理](15-jing-zi-qi-mlpmo-xing-9wei-shu-ru-3ceng-quan-lian-jie-ce-lue-tou-9-logits-jie-zhi-tou-tanh-1-1-ke-50msnei-cputui-li)
- 若要探索自弈训练的完整闭环，请阅读 [自弈训练系统：ai_server.py + game/main.exe 联调，epsilon-greedy探索→策略梯度→迭代500轮收敛](16-zi-yi-xun-lian-xi-tong-ai_server-py-game-main-exe-lian-diao-epsilon-greedytan-suo-ce-lue-ti-du-die-dai-500lun-shou-lian)