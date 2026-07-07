井字棋（Tic-Tac-Toe）是这套通用视觉AI系统的"最小可行示例"——它是系统其他所有模块（屏幕捕获、输入模拟、AI模型）的测试靶场。理解游戏本体的架构，是进入整个项目最深处的第一把钥匙。本文聚焦于 `game/` 模块的核心职责：**棋盘状态管理、规则执行、胜负判定，以及两套终端交互界面（箭头键驱动的TUI 与 经典文本行输入）**。

Sources: [board.hpp](game/include/board.hpp), [main.cpp](game/src/main.cpp), [tui.hpp](game/include/tui.hpp)

---

## 架构总览：棋盘 + 界面 + 网络，三者解耦

`game/` 模块的内部不采用复杂的类继承或多态设计，而是遵循 **C 风格函数式模块化**：四个头文件各司其职，通过全局变量 `g_cfg`（配置）和全局数组 `board[3][3]`（棋盘状态）隐式耦合。

```
┌─────────────────────────────────────────────────────────────┐
│                         main.cpp                            │
│  ┌────────────────────────────────────────────────────────┐ │
│  │  play_tui_game()  ← 箭头键TUI（Human vs Human）       │ │
│  │  play_tui_vs_ai() ← 箭头键TUI（Human vs AI）          │ │
│  │  play_classic()   ← 纯文本模式（训练 / AI vs AI）     │ │
│  └────────────┬───────────────────────────┬───────────────┘ │
│               │                           │                  │
│         ┌─────▼──────┐            ┌──────▼──────┐          │
│         │  board.cpp  │            │  network.cpp│          │
│         │  (棋盘/规则) │            │  (TCP协议)   │          │
│         └─────┬──────┘            └──────┬──────┘          │
│               │                          │                  │
│         ┌─────▼──────┐            ┌──────▼──────┐          │
│         │  tui.cpp    │            │  config.cpp │          │
│         │  (终端原始) │            │  (CLI解析)  │          │
│         └────────────┘            └─────────────┘          │
└─────────────────────────────────────────────────────────────┘
```

这个四文件结构（board / tui / config / network）外加一个 `main.cpp` 作为主控，形成了一个清晰的**分层**：
1. **数据层**：`board.hpp/cpp` — 棋盘内存表示与规则函数
2. **交互层**：`tui.hpp/cpp` — 终端原始模式与ANSI渲染
3. **通信层**：`network.hpp/cpp` — TCP客户端，与Python AI服务器对话
4. **控制层**：`main.cpp` — 游戏循环主逻辑，组合其他模块

Sources: [main.cpp](game/src/main.cpp#L1-L33), [board.cpp](game/src/board.cpp), [network.cpp](game/src/network.cpp), [tui.cpp](game/src/tui.cpp)

---

## 棋盘模型：全局字符数组与基础操作

井字棋的棋盘以**最简单的数据结构**呈现——一个全局3×3字符数组：

```cpp
// game/include/board.hpp line 12
extern char board[3][3];
```

三种字符状态的含义：
| 字符 | 含义 |
|------|------|
| `'.'` | 空格子（未被占据） |
| `'X'` | 玩家X落子 |
| `'O'` | 玩家O落子 |

对应的操作函数构成了棋盘的核心API：

| 函数 | 作用 | 复杂度 |
|------|------|--------|
| `reset_board()` | 全部格子置为 `'.'` | O(9) |
| `print_board()` | 终端打印ASCII网格 | O(9) |
| `is_valid(r, c)` | 行列是否在[0,2]范围内 | O(1) |
| `is_occupied(r, c)` | 指定格子是否已被占据 | O(1) |
| `check_win(p)` | 检查玩家p是否胜利 | O(8) |
| `is_draw()` | 是否平局（棋盘满且无人胜） | O(9) |
| `parse_input(line, row, col)` | 解析用户输入"0 1"到行列 | O(n) |

Sources: [board.hpp](game/include/board.hpp#L6-L18), [board.cpp](game/src/board.cpp#L1-L82)

### 胜负判定算法

`check_win()` 采用**穷举8条线**的直截了当方式——3行 + 3列 + 2条对角线：

```cpp
// board.cpp 第23-29行
bool check_win(char p) {
    for (int i = 0; i < 3; i++) {
        if (board[i][0] == p && board[i][1] == p && board[i][2] == p) return true;  // 行
        if (board[0][i] == p && board[1][i] == p && board[2][i] == p) return true;  // 列
    }
    if (board[0][0] == p && board[1][1] == p && board[2][2] == p) return true;      // 主对角线
    if (board[0][2] == p && board[1][1] == p && board[2][0] == p) return true;      // 副对角线
    return false;
}
```

这个算法没有使用任何技巧或优化——对于一个3×3的棋盘，8条线各检查3个格子的暴力穷举已经足够高效。但它的设计有一个值得注意的**架构隐含点**：`check_win()` 只检查"传入的特定玩家"是否胜利，而不是同时检查双方。这意味着**调用者需要在每次落子后，仅检查刚落子的那一方**——这在主循环中表现为 `check_win(cur)`，而不是 `check_win('X') || check_win('O')`。

平局判定 `is_draw()` 更简单：遍历全盘，若存在至少一个 `'.'` 则未平局，否则返回 `true`。

Sources: [board.cpp](game/src/board.cpp#L22-L36)

### 用户输入解析

`parse_input()` 是连接"人类文本输入"与"棋盘格子索引"的桥梁。它从一行文本中提取两个整数，允许各种分隔符（空格、逗号、字母均可）。设计上特别注意了**溢出保护**：最多读取两位数字，超出的部分被跳过，防止 `"999 999"` 这样的输入导致 `row = 999` 穿越边界。然而，防御的最终防线仍在调用方——`main.cpp` 中 `play_classic()` 在调用 `parse_input()` 之后还调用了 `is_valid()` 和 `is_occupied()` 进行三重验证。

Sources: [board.cpp](game/src/board.cpp#L40-L82), [main.cpp](game/src/main.cpp#L215-L222)

---

## 终端TUI：箭头键驱动的交互式棋盘界面

这是 `game/` 模块最有趣的设计——一个不使用ncurses、仅依赖**ANSI转义码 + Windows控制台API**的终端用户界面。

### 初始化与还原

`tui_init()` 和 `tui_restore()` 负责切换终端模式：

| 操作 | Windows实现 | Linux/macOS实现 |
|------|-------------|-----------------|
| 启用ANSI转义 | `SetConsoleMode(ENABLE_VIRTUAL_TERMINAL_PROCESSING)` | 默认支持 |
| 原始输入 | 关闭 `ENABLE_LINE_INPUT` + `ENABLE_ECHO_INPUT` | `cfmakeraw()` 风格（关闭ICANON+ECHO） |
| 隐藏光标 | `printf("\x1b[?25l")` | 同左 |
| 恢复终端 | 恢复保存的 `g_old_in` / `g_old_out` | `tcsetattr(TCSANOW, &g_old_termios)` |

关键设计点：**Windows 10 RS1 (1607) 之后的操作系统版本均支持 `ENABLE_VIRTUAL_TERMINAL_PROCESSING`**，这让跨平台ANSI转义码成为可能。旧的 `SetConsoleTextAttribute()` API 被完全弃用。

Sources: [tui.cpp](game/src/tui.cpp#L1-L74)

### ANSI渲染管线

棋盘渲染完全构建在ANSI转义码之上。`main.cpp` 中定义了一系列宏函数作为调色板：

```cpp
#define ESC "\033"
static void a_grn() { printf(ESC "[32m"); }   // 绿色 (X用)
static void a_blu() { printf(ESC "[34m"); }   // 蓝色 (O用)
static void a_inv() { printf(ESC "[7m"); }    // 反转 (光标所在空位)
static void a_wht_bg(){ printf(ESC "[47m"); } // 白底 (光标在已占位)
static void a_dim() { printf(ESC "[2m"); }    // 暗淡 (空格占位符)
```

渲染一个格子的逻辑包含四种视觉状态：

```
                    ┌─────────────────────────────────────┐
                    │         格子状态机 (g_row函数)        │
                    ├─────────────┬───────────────────────┤
                    │  未占据      │  .  (暗淡灰色点)       │
                    │  未占据+光标 │  当前玩家字符(反转)    │
                    │  已占据(X)   │  绿色加粗X             │
                    │  已占据(O)   │  蓝色加粗O             │
                    │  已占据+光标 │  白底加粗玩家色         │
                    └─────────────┴───────────────────────┘
```

这种设计让玩家在任何时候都能通过视觉反馈清楚自己的位置和棋盘状态。

Sources: [main.cpp](game/src/main.cpp#L34-L72)

### 光标闪烁与游戏循环

`tui_get_move()` 实现了完整的交互式输入循环，其核心设计是**一个事件驱动的主循环 + 一个定时刷新的光标闪烁**：

```
                          ┌──────────────────────┐
                          │  初始化: cr=1, cc=1  │
                          │  cv=true, 绘制画面    │
                          └──────────┬───────────┘
                                     │
                          ┌──────────▼───────────┐
         ┌────────────┐   │   主循环 while(1)    │
         │ 光标闪烁    │   │                     │
         │ 每400ms切换  │◄──┤  kbhit()?  → 处理  │
         │ 可见性并重绘 │   │  否则  → 检查闪烁   │
         └────────────┘   │                     │
                          └──────────┬───────────┘
                                     │
                     ┌───────────────┴───────────────┐
                     │     方向键    回车    Esc      │
                     ▼               ▼               ▼
                ┌─────────┐   ┌─────────┐     ┌──────────┐
                │移动光标   │   │尝试落子   │   │返回false │
                │边界钳位   │   │→ 若被占  │   │(退出)    │
                └─────────┘   │  "Cell  │   └──────────┘
                              │  taken!"│
                              │  → 闪提示│
                              │→ 返回true│
                              └─────────┘
```

三个值得一提的工程细节：

1. **光标闪烁**：使用 `std::chrono::steady_clock` 记录上次切换时间，每400ms反转一次可见性 `cv`。每次有键盘事件时重置闪烁计时，确保用户在按下按键时光标稳定可见。

2. **"格子已被占"反馈**：当用户试图在已占格子落子时，系统不会简单地忽略——它会将状态行显示为红色"Cell taken!"并持续1秒（`flash_until` 时间戳），为用户提供明确的视觉错误反馈。

3. **方向键边界钳位**：方向键处理使用三目运算符 `cr = cr > 0 ? cr-1 : 0` 确保光标永远不会移出棋盘边界，而不是模运算或其他方式——前者不会让光标"循环"到对面，更符合直觉。

Sources: [main.cpp](game/src/main.cpp#L101-L161)

---

## 三种游戏模式的控制流

`main()` 函数根据配置参数将游戏导向三种模式之一。下面这张流程图用到的函数调用展示了完整的控制流：

```
                    main()
                      │
       ┌──────────────┼──────────────┐
       │              │              │
  无 --server    --server        --server
                  (Human vs AI)   --auto
       │              │              │
   ┌──▼──┐       ┌───▼───┐      ┌──▼──┐
   │TUI  │       │TUI    │      │纯   │
   │人机 │       │人机   │      │文本 │
   │对弈 │       │对AI   │      │AI自弈│
   └──┬──┘       └───┬───┘      └──┬──┘
      │              │              │
  play_tui_      play_tui_      play_classic
  game()         vs_ai()        (sock)
      │              │              │
  ┌───┴─────┐   ┌───┴─────┐   ┌───┴─────┐
  │tui_get  │   │tui_get  │   │cin >>   │
  │_move()  │   │_move()  │   │行输入    │
  │(箭头键)  │   │(人)     │   │(文本)    │
  └─────────┘   │get_ai_  │   │get_ai_  │
                │move()   │   │move()   │
                │(AI)     │   │(AI)     │
                └─────────┘   └─────────┘
```

| 模式 | CLI标志 | 人类输入方式 | AI参与 | 用途 |
|------|---------|------------|--------|------|
| 人机对弈 | 无参数 | 箭头键TUI | 无 | 本地双人娱乐 |
| Human vs AI | `--server host port` | 箭头键TUI | 通过TCP查询AI | 测试AI对战 |
| AI自弈 | `--server host port --auto` | 无（纯文本） | 双方均为AI | 训练数据生成 |

自弈模式是训练系统的核心——它不加载TUI（`tui` 变量为false），不等待按键，纯粹在文本模式下自动运行。当 `g_quit_flag` 被Ctrl+C触发时，游戏循环安全退出。

Sources: [main.cpp](game/src/main.cpp#L241-L281), [config.cpp](game/src/config.cpp)

---

## 跨平台构建系统

`game/` 提供两套构建方式：

| 构建系统 | 入口 | 编译器 | 链接库 |
|----------|------|--------|--------|
| Makefile | `game/src/Makefile` | g++ (MinGW) | `-lws2_32` (Winsock) |
| VS Build | `game/build.cmd` | MSVC cl.exe | `ws2_32.lib` |

Makefile 通过 `ifneq ($(OS),Windows_NT)` 自动检测Windows环境并添加Winsock依赖。而 `build.cmd` 直接调用Visual Studio的 `vcvars64.bat` 设置环境，使用MSVC编译。两种方式都使用C++17标准（`-std=c++17`），依赖于 `/I include` 和 `/I ../common/include` 两个头文件搜索路径——其中 `../common/include` 提供了 `types.hpp`（`sleep_ms` 宏定义）和 `signals.hpp`（全局退出标志）。

Sources: [Makefile](game/src/Makefile), [build.cmd](game/build.cmd), [types.hpp](common/include/types.hpp)

---

## 设计决策逻辑

理解 `game/` 模块的设计哲学，有助于理解它在整个项目中的角色：

**为什么使用全局变量而不是类？** 棋盘 `board[3][3]` 是全局可见的，`g_cfg` 也是。这并非设计缺陷，而是**有意的权衡**——整个 `game/` 模块是"一次性使用"的测试靶场，不存在多实例需求，全局变量让代码更简短、调用更直接，也更容易被外部工具（如训练脚本）通过 `extern` 访问。

**为什么TUI不使用ncurses？** Ncurses 在Windows上的支持复杂且需要额外安装。而Windows 10+ 原生支持ANSI转义码后，纯ANSI方案在跨平台上的表现完全够用，且零依赖。

**为什么需要"纯文本模式"和"TUI模式"两套界面？** 纯文本模式（std::cin行输入）适用于AI vs AI的自弈训练——数据管道不需要渲染UI，只需接收和发送文本。TUI模式适用于人类与AI对弈的场景，提供更好的用户体验。两种模式共享同一套 `board.cpp` 的规则引擎。

Sources: [board.hpp](game/include/board.hpp), [main.cpp](game/src/main.cpp#L241-L248)

---

## 进一步探索

当你理解了棋盘规则和终端交互之后，下一步逻辑上是**网络对战协议**——AI服务器如何通过TCP接收棋盘状态并返回落子决策：

➡ [网络对战协议：纯文本行协议（9棋格+执棋方→返回行列），支持人人/Human-vs-AI/AI-vs-AI三种模式](7-wang-luo-dui-zhan-xie-yi-chun-wen-ben-xing-xie-yi-9qi-ge-zhi-qi-fang-fan-hui-xing-lie-zhi-chi-ren-ren-human-vs-ai-ai-vs-aisan-chong-mo-shi)

如果想要理解这个井字棋游戏在整个视觉AI系统架构中的位置，推荐阅读：

➡ [设计哲学：四层架构（游戏本体→屏幕捕获→输入模拟→AI模型）与零Rust纯C++策略](3-she-ji-zhe-xue-si-ceng-jia-gou-you-xi-ben-ti-ping-mu-bu-huo-shu-ru-mo-ni-aimo-xing-yu-ling-rustchun-c-ce-lue)