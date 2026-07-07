## 核心设计理念

本项目的核心设计哲学可以凝练为一句话：**像素输入，动作输出**（pixels in, actions out）。这不是一个普通的井字棋程序，而是一个**通用视觉游戏AI系统的原型**，井字棋只是它的第一个验证靶场。

整个系统被拆分为四个独立的层，每一层解决一个明确的问题域，层与层之间通过明确定义的接口通信，互不感知对方的存在。这种四层解耦设计的核心目的，是让系统能够**在不修改捕获、输入、Agent主循环代码的前提下，切换到任意另一个游戏**。

从代码仓库的顶层结构可以直接看到这四层的物理投影：`game/`、`capture/`、`input/`、`ai/`，加上作为胶水层的 `agent/`。"零Rust纯C++"是项目的另一个硬性约束——所有实时、性能敏感的代码全部使用C++编写，没有任何Rust参与。

Sources: [项目顶层目录](.)

---

## 四层架构总览

```
┌──────────────────────────────────────────────────────────────┐
│                     四层架构总览                               │
├──────────────────────────────────────────────────────────────┤
│                                                              │
│  ┌───────────────────┐    ┌──────────────────┐              │
│  │   Layer 1         │    │   Layer 2         │              │
│  │   游戏本体 (game/) │    │   屏幕捕获 (capture/)│            │
│  │                   │    │                   │              │
│  │   3x3棋盘规则     │    │   ICaptureBackend │              │
│  │   胜负判定       │    │   抽象接口         │              │
│  │   终端TUI交互     │◄──►│   5种后端实现      │              │
│  │   纯文本网络协议  │    │   WGC/DXGI/GDI    │              │
│  │                   │    │   帧预处理管线     │              │
│  └────────┬──────────┘    └────────┬──────────┘              │
│           │                        │                         │
│           │    TCP :9999           │   像素数据流              │
│           ▼                        ▼                         │
│  ┌───────────────────────────────────────────────────┐       │
│  │   Layer 3+4 Agent (agent/)                        │       │
│  │   捕获→预处理→TCP发送→接收令牌→解码→输入模拟     │       │
│  │   ActionMapper: 游戏无关的二进制动作协议           │       │
│  └────────┬─────────────────────────────────┬─────────┘       │
│           │                                 │                  │
│           ▼                                 ▼                  │
│  ┌───────────────────┐    ┌──────────────────┐              │
│  │   Layer 4         │    │   Layer 4         │              │
│  │   输入模拟 (input/) │    │   AI模型 (ai/)    │              │
│  │                   │    │                   │              │
│  │   IInputBackend   │    │   MLP/CNN模型     │              │
│  │   抽象接口         │    │   策略头+价值头    │              │
│  │   Interception驱动 │    │   epsilon-greedy  │              │
│  │   SendInput系统层  │    │   自弈训练系统     │              │
│  └───────────────────┘    └──────────────────┘              │
│                                                              │
│  ┌─────────────────────────────────────────────────────┐     │
│  │  支撑模块                                           │     │
│  │  logger/ (日志引擎)  protocol/ (线缆协议)           │     │
│  │  common/ (共享类型)  monitor_app/ (监控桌面)        │     │
│  └─────────────────────────────────────────────────────┘     │
└──────────────────────────────────────────────────────────────┘
```

Sources: [game/include/board.hpp](game/include/board.hpp#L1-L19), [capture/include/capture.hpp](capture/include/capture.hpp#L1-L67), [agent/include/agent.hpp](agent/include/agent.hpp#L1-L33), [input/include/input.hpp](input/include/input.hpp#L1-L98)

---

## Layer 1: 游戏本体 — 井字棋规则与TUI

游戏本体位于 `game/` 目录，是整个系统中最简单的一层。它的职责非常纯粹：维护3x3棋盘状态、执行走棋规则、判定胜负平局，并提供两种交互方式——人类通过终端TUI操作，或AI通过TCP协议接入。

### 核心模块

| 文件 | 职责 |
|------|------|
| `board.hpp` / `board.cpp` | 棋盘数据结构 `char board[3][3]`（'.'=空，'X'=玩家1，'O'=玩家2），落子合法性检查、胜负平局判定 |
| `config.hpp` / `config.cpp` | CLI参数解析，支持 --server(连接AI)、--auto(全自动)、--ai(指定执棋方) |
| `network.hpp` / `network.cpp` | TCP客户端（纯文本行协议），连接ai_server.py发送"b0...b8 player\n"、接收"row col value\n" |
| `tui.hpp` / `tui.cpp` | 带方向键/Enter的终端交互，ANSI颜色标记X绿O蓝，光标闪烁提示 |
| `main.cpp` | 入口分发：无参数→人人对战；--server→人机对战；--server --auto→AI自弈 |

### 架构特点

游戏本体对上层捕获层**完全无感知**。它不知道自己正在被视觉系统观察，也不关心输入是通过键盘还是Interception驱动模拟的。它只是忠诚地维护棋盘、显示TUI，或者通过TCP接受落子指令。

关键设计体现在 `network.cpp` 的纯文本协议上。当游戏运行在"服务端模式"（如AI自弈：`main.exe --server 127.0.0.1 9999 --auto`），它会在每次轮到自己时，以纯文本格式发送棋盘状态到ai_server.py，并接收"行 列 价值"的回复。这个协议虽然简单，但与 `agent/` 层使用的二进制协议**完全不同**——前者是针对井字棋的专用文本协议，后者是游戏无关的通用二进制动作协议。这种"同一种功能两套协议并存"的设计，是为了让井字棋本身就能独立运作和训练，而不必依赖视觉捕获流水线。

Sources: [game/include/board.hpp](game/include/board.hpp#L1-L19), [game/include/network.hpp](game/include/network.hpp#L1-L48), [game/include/config.hpp](game/include/config.hpp#L1-L21), [game/src/main.cpp](game/src/main.cpp#L1-L200)

---

## Layer 2: 屏幕捕获 — 从窗口像素到浮点张量

捕获层位于 `capture/` 目录，是系统中最复杂、C++特性最密集的一层。它的职责是将屏幕上指定窗口的像素内容，经过一系列预处理，最终输出为AI模型可以直接消费的浮点张量。

### 抽象接口与多后端设计

捕获层的核心是抽象接口 `ICaptureBackend`，它定义了三个基本操作：`init()`、`capture()`、`get_window_rect()`。

```cpp
class ICaptureBackend {
public:
    virtual bool init() = 0;
    virtual bool capture(FrameBuffer& out, const Rect* region = nullptr) = 0;
    virtual bool get_window_rect(const wchar_t* title, Rect& out) = 0;
    virtual const char* name() const = 0;
    virtual void shutdown() = 0;
};
```

Sources: [capture/include/capture.hpp](capture/include/capture.hpp#L20-L45)

这个抽象接口背后，系统提供了五种可插拔的捕获后端：

| 后端 | 源文件 | 原理 | 典型耗时 | GPU参与 |
|------|--------|------|----------|---------|
| **WGC** (Windows Graphics Capture) | `capture_wgc.cpp` | WinRT FramePool + D3D11 GPU拷贝 | 1-3ms | ✅ 纯GPU |
| **DXGI Desktop Duplication** | `capture_dxgi.cpp` | DXGI直接桌面复制 | 1-2ms | ✅ 纯GPU |
| **GetWindowDC** | `capture_gdi.cpp` | GDI `GetWindowDC` → BitBlt | 5-10ms | ❌ CPU |
| **PrintWindow** | `capture_pw.cpp` | `PrintWindow` API + 洋红色哨兵检测 | 5-20ms | ❌ CPU |
| **ScreenBitBlt** | `capture_screen.cpp` | 虚拟屏幕DC → BitBlt | 10-30ms | ❌ CPU |
| **DesktopBlt** | `capture_desktop.cpp` | 桌面DC → BitBlt (多显示器安全) | 10-30ms | ❌ CPU |

工厂函数 `create_capture_backend()` 自动按优先级选择最佳可用后端，上层调用者完全无需关心具体使用的是哪个后端。

一个值得注意的细节是 `capture_internal.h` 中实现的 **DPI感知** 机制。在混合DPI的多显示器环境下（例如4K高分辨率主屏 + 1080p副屏），GDI操作会因DPI上下文不匹配而产生坐标偏移。代码通过动态加载 `SetThreadDpiAwarenessContext`（运行时可选的User32 API），使用RAII守卫 `DpiGuard` 在每个捕获操作前后切换DPI上下文，确保捕获区域的坐标精确匹配目标窗口的实际像素位置。

Sources: [capture/include/capture_internal.h](capture/include/capture_internal.h#L1-L131), [capture/src/capture_auto.cpp](capture/src/capture_auto.cpp), [capture/include/capture_methods.h](capture/include/capture_methods.h#L1-L43)

### WGC捕获的深度技术选型

WGC（Windows Graphics Capture）是系统的首选捕获后端，它的技术深度值得单独剖析。选择WGC而非更常见的DXGI Desktop Duplication，是基于以下工程考量：

1. **窗口感知而非桌面感知**：DXGI捕获的是**整个桌面**（可能包含多个显示器），需要额外做窗口裁剪。WGC直接绑定到特定 `HWND`，天然只捕获目标窗口内容，即使窗口被部分遮挡也只会捕获可见部分。
2. **GPU端到端管线**：WGC使用 `Direct3D11CaptureFramePool` 管理三重缓冲的 `ID3D11Texture2D`，所有帧拷贝操作都在GPU端完成，无需CPU内存拷贝直到最后一步 `Map()` 读出像素。
3. **WinRT MTA多线程模型**：WGC要求使用WinRT的 `DispatcherQueue` 和MTA（多线程单元）初始化。`capture_wgc.cpp` 在一个独立的守护线程上初始化MTA、创建 `DispatcherQueue`，并使用条件变量（`std::condition_variable`）实现高效的帧等待——帧到达时 `SetEvent` 通知，而非轮询等待。

Sources: [capture/src/capture_wgc.cpp](capture/src/capture_wgc.cpp), [capture/include/capture_wgc.hpp](capture/include/capture_wgc.hpp)

### 帧预处理管线

捕获的BGRA像素帧必须经过标准化的预处理管线才能送入AI模型。这个管线定义在 `preprocess.hpp` 和 `preprocess.cpp` 中：

```
原始BGRA帧 (任意分辨率)
  → 裁剪到游戏窗口区域
  → 双线性缩放到 84x84
  → 灰度化 (BGRA → 单通道)
  → 4帧堆叠 (ring buffer, 形成(4,84,84)张量)
  → 归一化 float32 [0,1]
  → 输出 float tensor[4*84*84]
```

84x84的分辨率是视觉AI领域的经典选择（DeepMind DQN在Atari游戏中使用的输入尺寸），4帧堆叠则让模型能够感知运动趋势——单帧无法区分"静止的X"和"刚刚落下的X"。

Sources: [capture/include/preprocess.hpp](capture/include/preprocess.hpp#L1-L58)

---

## Layer 3: 输入模拟 — 游戏动作的物理执行

输入层位于 `input/` 目录，是整个系统中最"微妙"的一层——它在操作系统层面模拟真实的鼠标键盘输入，让游戏窗口以为是一个真实的人类在操作。

### 抽象接口 IInputBackend

与捕获层类似，输入层也基于抽象接口设计：

```cpp
class IInputBackend {
public:
    virtual bool init() = 0;
    virtual bool send_action(const GameAction& a) = 0;
    virtual bool move_mouse(int x, int y) = 0;
    virtual bool click(MouseButton btn = MouseButton::Left) = 0;
    virtual bool key_press(uint16_t vk) = 0;
    virtual bool key_release(uint16_t vk) = 0;
    virtual bool key_tap(uint16_t vk, int dur_ms = 50) = 0;
    virtual const char* name() const = 0;
    virtual void shutdown() = 0;
};
```

Sources: [input/include/input.hpp](input/include/input.hpp#L30-L63)

### GameAction：统一语义层

`GameAction` 是一个联合体风格的POD结构，定义了10种输入动作的通用语义：

| 动作类型 | 参数字段 | 典型用途 |
|----------|----------|----------|
| `MouseMove` | x, y (绝对像素坐标) | 移动鼠标到棋盘格 |
| `MouseMoveRelative` | dx, dy (像素增量) | 微调鼠标位置 |
| `MouseClick` | x, y, btn | 点击棋盘格落子 |
| `MouseDown` / `MouseUp` | btn | 拖拽操作 |
| `KeyDown` / `KeyUp` | vk_code | 按下/释放功能键 |
| `KeyTap` | vk_code, wait_ms | 快捷键输入 |
| `Wait` | wait_ms | 帧间延迟 |

这个语义层的核心价值在于**将"做什么"与"怎么做"分离**。上层（agent）只需要告诉输入层"在(500,300)处点击左键"，而不必关心这个点击是通过SendInput API还是Interception驱动实现的。

Sources: [input/include/input.hpp](input/include/input.hpp#L10-L28)

### 双后端策略：一个可检测，一个不可检测

系统提供两个输入后端实现：

1. **SendInput**（`input_sendinput.cpp`）：使用Windows API `SendInput()` 模拟输入。这个API的输入事件会被操作系统标记为"合成输入"，可以通过 `GetMessageExtraInfo()` 的低7位检测。这是"文明"的后端——可被检测，适合非对抗场景。

2. **Interception驱动**（`input_interception.cpp`）：使用OBS项目的 `interception` 内核驱动，在驱动层注入输入事件。这个后端的输入对于被监控的程序完全透明——游戏收到的输入事件与真实物理输入在操作系统层面没有任何区别，无法被任何用户态代码检测或区分。这是"不文明"的后端——不可检测，适合需要绕过反作弊的场景。

后端的切换由 `create_input_backend()` 工厂函数自动选择：优先尝试Interception驱动（需要管理员权限加载驱动），如果不可用则回退到SendInput。

Sources: [input/src/input_interception.cpp](input/src/input_interception.cpp), [input/src/input_sendinput.cpp](input/src/input_sendinput.cpp)

---

## Layer 4: AI模型 — 从棋子表示到通用视觉策略

AI层是系统中最"异构"的一层，由 `ai/`（Python训练推理）和 `model/`（Python模型定义）组成。它不是C++层，而是通过TCP协议与C++层通信的独立进程。这种跨语言异构架构的设计考量很明确：C++负责所有实时、性能敏感的工作（捕获、输入、WebView2 GUI），Python则负责AI模型定义、训练和推理——因为这些工作受益于Python丰富的深度学习生态（PyTorch、NumPy），且推理延迟被网络通信开销掩盖，性能不再是瓶颈。

### 井字棋MLP模型（当前实现）

当前系统运行的是一个针对井字棋的简洁多层感知器（MLP）：

```
输入层: 9维 (3x3棋盘状态, 归一化到[-1,0,1])
  ↓
FC1: 9 → 128 (ReLU)
  ↓
FC2: 128 → 128 (ReLU)
  ↓
FC3: 128 → 128 (ReLU)
  ↓  ↓
策略头 (128→9, softmax)  价值头 (128→1, tanh)
```

- **策略头**输出9个logits，经softmax转为9个格子的落子概率
- **价值头**输出 `tanh` 压缩到[-1,1]的局面评估值（1=执棋方必胜，-1=必败）
- 单次CPU推理耗时约 **50ms以内**（在普通桌面CPU上）

模型在井字棋这个场景上表现得极为出色——在128维隐藏层、epsilon=0.1、500轮自弈的条件下稳定收敛，学会从"随机乱下"到"永不输棋"的全部必要策略。

Sources: [ai/model.py](ai/model.py#L1-L102), [ai/net.py](ai/net.py)

### 通用视觉Agent模型（长远目标）

系统真正的长远目标是通用视觉Agent，其架构在 `model/generic_agent.py` 中定义：

```
输入: 4x84x84 灰度帧堆叠
  ↓
CNN视觉编码器: 3层卷积 (stride=2) → 256维特征向量
  ↓
Transformer自回归解码器: 4层因果注意力
  ↓
输出: 动作令牌序列 (每个令牌1字节类型 + 变长参数)
```

这个架构的设计要点包括：

1. **游戏无关性**：模型不接收任何游戏特定的状态表示（如井字棋的9维向量），只接收像素帧。这意味着同一个模型架构可以用于任何通过像素呈现的游戏。
2. **自回归动作生成**：使用Transformer的自回归解码器生成动作令牌序列，与语言模型的文本生成在数学上完全同构——只是词汇表变成了动作令牌。
3. **信息瓶颈层次化**：更进一步的 `model/hierarchical.py` 实现了一个端到端信息瓶颈架构，将模型拆分为L1感知专家（像素→16维压缩隐变量z）和L2策略推理器（z+历史→动作），使得感知和策略可以分别微调甚至替换。

Sources: [model/generic_agent.py](model/generic_agent.py), [model/hierarchical.py](model/hierarchical.py)

---

## Agent主循环：四层的胶水

`agent/` 目录是整个系统的调度中枢，负责将Capture、Preprocess、Network、Input四层串联起来，形成一个完整的闭环：

```
                   Agent 主循环
                 ┌─────────────────────────────────────┐
                 │     while(!g_quit_flag) {           │
                 │         t0 = now()                   │
Capture ◄────────│─── 1. capture->capture(frame)       │
                 │         t1 = now()                   │
Preprocess ◄─────│─── 2. preproc.process(frame, tensor)│
                 │         t2 = now()                   │
TCP Send ◄───────│─── 3. server.send_tensor(tensor)    │
                 │                                     │
AI Server ◄──────│─── TCP :9999 ──► PyTorch推理        │
                 │                                     │
TCP Recv ◄───────│─── 4. server.recv_action_tokens()   │
                 │         t3 = now()                   │
ActionDecoder ◄──│─── 5. decoder.decode(raw_tokens)    │
                 │                                     │
ActionMapper ◄───│─── 6. mapper.execute(decoded)       │
                 │         t4 = now()                   │
Input Sim ◄──────│─── 7. input->send_action(gameAction)│
                 │         log(t0..t4 latencies)        │
                 │         sleep(frame_interval_ms)     │
                 │     }                                │
                 └─────────────────────────────────────┘
```

### 两个关键解耦点

Agent层之所以能够做到"游戏无关"，得益于两个关键设计：

**1. 动作令牌解码器 `ActionDecoder`**：将AI服务器返回的二进制令牌流（如 `[MOUSE_MOVE_ABS, 0.3f, 0.5f, MOUSE_CLICK, 0.3f, 0.5f, 0]`）转换为带有绝对像素坐标的物理动作。这里的坐标是屏幕归一化值（0.0~1.0），乘以实际窗口宽高即可得到像素坐标——无论游戏是什么分辨率，模型只需要学习归一化坐标。

**2. 通用动作映射器 `GenericActionMapper`**：将解码后的物理动作直接提交给输入后端执行。它不包含任何游戏特定逻辑——不关心点击的位置是井字棋的格子还是扫雷的方块，只是忠实地将"在(x,y)处点击"翻译为 `IInputBackend::click_at(x,y)`。

Sources: [agent/src/agent.cpp](agent/src/agent.cpp#L1-L200), [agent/include/action_mapper.hpp](agent/include/action_mapper.hpp#L1-L101), [agent/src/main.cpp](agent/src/main.cpp#L1-L64)

---

## 零Rust纯C++策略

"零Rust纯C++"是本项目的硬性架构约束——所有实时、性能敏感的代码全部使用C++编写，任何Rust代码都不参与核心流水线。

### 为什么不用Rust？

这不是一个随意的技术偏好，而是经过项目演进后的理性选择。项目的上一个版本确实使用了Rust（通过Tauri作为WebView2宿主），但在v0.3.0版本中，团队将Tauri完全移除了，并用纯C++的WebView2宿主替代。

移除Rust/Tauri后，项目的技术栈简化为：

| 组件 | 当前实现 | 替代前 |
|------|----------|--------|
| WebView2宿主 | C++ Win32 窗口 + COM API | Rust (Tauri) |
| IPC桥接 | `chrome.webview.postMessage` → C++命令分发 | Tauri `invoke` |
| 屏幕捕获 | C++ ICaptureBackend + 五种后端 | C++（未变） |
| 输入模拟 | C++ IInputBackend + 双后端 | C++（未变） |
| AI模型 | Python (PyTorch) | Python（未变） |
| 前端UI | React + TypeScript + Tailwind | React（未变，零改动） |

### 零Rust策略的关键技术细节

**1. WebView2直接COM操作**：C++宿主直接通过COM接口创建WebView2环境、控制器、调用 `PostSharedBufferToScript`，不再需要Tauri的Rust抽象层。关键接口包括 `ICoreWebView2Environment12`（创建共享缓冲区）、`ICoreWebView2_17`（`PostSharedBufferToScript` 方法）。

**2. WebMessage桥接取代Tauri invoke**：JS端使用 `chrome.webview.postMessage('{"cmd":"list_windows","id":1,"args":{}}')` 发送命令，C++端 `WebMessageReceived` 事件处理函数解析JSON、分发给 `commands.cpp` 的命令处理器，再通过 `PostWebMessageAsJson` 返回结果。前端代码的改动仅限于将 `invoke()` 调用替换为一个同名的 `hostCall()` shim——`App.tsx` 本身100%不变。

**3. SharedBuffer零拷贝GPU→Canvas**：WGC捕获的GPU纹理通过 `CreateSharedBuffer` → `memcpy`（GPU→CPU这一次拷贝不可避免）→ `PostSharedBufferToScript` → JS端直接解码到Canvas。整个过程无需经过任何FFI层，避免了Rust的transmute开销。

Sources: [monitor_app/src/main.cpp](monitor_app/src/main.cpp#L1-L200), [CLAUDE.md](CLAUDE.md#L1-L200), [monitor_app/build.cmd](monitor_app/build.cmd#L1-L30)

### 断言：所有实时代码全是C++

我们可以通过项目的构建系统来验证"零Rust"的断言。每个模块的 `build.cmd` 文件都直接调用MSVC的 `cl.exe` ：

- `game/build.cmd`：`cl.exe /EHsc /W4 /std:c++17 ...` → `game.exe`
- `capture/build.cmd`：`cl.exe ...` → `capture_test.exe` + 多个工具
- `capture/build_capture_lib.cmd`：`cl.exe ...; lib.exe ...` → 5个静态库（common.lib, wgc.lib, gdi.lib, pw.lib, screen.lib, desktop.lib）
- `input/build.cmd`：`cl.exe ...` → `input_test.exe`
- `agent/build.cmd`：`cl.exe ...` → `agent.exe`
- `monitor_app/build.cmd`：`cl.exe ...` → `monitor_app.exe`

整个构建链中没有任何 `cargo build` 或 `rustc` 调用（注意： `capture/build.cmd` 末尾确实有一行 `rustc wgc_bench_recv.rs`，但那是一个独立的基准测试接收端工具，不属于核心流水线）。

Sources: [game/build.cmd](game/build.cmd#L1-L10), [capture/build.cmd](capture/build.cmd#L1-L35), [capture/build_capture_lib.cmd](capture/build_capture_lib.cmd#L1-L32), [input/build.cmd](input/build.cmd#L1-L8), [agent/build.cmd](agent/build.cmd#L1-L8), [monitor_app/build.cmd](monitor_app/build.cmd#L1-L30)

### 为什么不需要Rust？

井字棋这个级别的项目确实不需要Rust，原因如下：

1. **资源竞争不激烈**：C++的原始指针和手动内存管理在这个规模下完全可控，智能指针（`std::unique_ptr`、`ComPtr`）已经覆盖了主要的资源安全场景。
2. **无跨线程数据竞争**：捕获帧通过 `std::condition_variable` 同步，日志引擎使用临界区互斥，WebView2回调在UI线程上处理——线程模型足够简单，无需编译器级别的数据竞争检查。
3. **COM天然是C++的领域**：WinRT/COM接口（WebView2、WGC的Direct3D、Windows Imaging Component）的官方头文件和接口定义全部是C++格式，用Rust调用需要额外的bindings层（winrt-rs、windows-rs等），增加而非减少了复杂度。
4. **构建链统一**：MSVC + `cl.exe` 一条完整的Windows原生工具链，无需交叉编译、无需处理Rust的target triple、无需管理Cargo依赖和C++/Rust互操作的ABI对齐问题。

---

## 模块间通信协议

四层之间通过两种截然不同的协议通信，这两种协议分别服务于"游戏原生AI"和"视觉AI"两个不同的使用场景：

### 场景A：游戏原生AI（井字棋专有）

通过TCP 9999端口，使用**纯文本行协议**：

```
客户端 → 服务器: "1 0 -1 0 1 0 -1 0 0 1\n"  (9个棋盘值 + 执棋方)
服务器 → 客户端: "1 2 0.8234\n"                 (行 列 价值)
游戏结束:       "END 1\n"                        (END 胜方)
```

这个协议用于 `game/main.exe` 与 `ai/ai_server.py` 之间的直接通信，是井字棋训练的专用通道。

Sources: [game/include/network.hpp](game/include/network.hpp#L7-L15), [ai/ai_server.py](ai/ai_server.py#L1-L200)

### 场景B：视觉AI通用（游戏无关）

通过TCP 9999端口，使用**二进制帧协议**，定义在 `protocol/protocol.h` 中：

```
[魔数:4 "FRAM"][载荷大小:4 LE][类型标签:4 LE][载荷体: 载荷大小 bytes]

类型标签 1 (BGRA帧): [宽度:4][高度:4][通道数:4][保留:4][像素数据...]
```

这个协议用于 `agent/agent.exe` 与通用视觉AI服务器之间的通信。Agent负责将捕获的像素张量打包发送，服务器返回一个或多个动作令牌（如 `[MOUSE_MOVE_ABS][x_norm][y_norm]`）。

Sources: [protocol/protocol.h](protocol/protocol.h#L1-L80)

---

## 总结：架构设计的核心权衡

四层架构的设计不是随意的，它代表了以下关键工程权衡：

1. **通用性 vs 简单性**：为了从井字棋平滑过渡到通用视觉AI，为井字棋实现了一套简单的文本协议，同时为视觉Agent准备了一套完整的二进制帧协议和通用动作解码器。当前阶段"两条腿走路"增加了初期代码量，但为未来的泛化提供了清晰的迁移路径。

2. **解耦度 vs 性能**：每一层都通过抽象接口（`ICaptureBackend`、`IInputBackend`、纯文本/二进制TCP协议）与相邻层解耦，这引入了虚函数调用开销和网络通信延迟。但考虑到井字棋AI的推理延迟在毫秒量级，这种解耦带来的灵活性收益远超其性能代价。

3. **零Rust纯C++ vs 技术流行度**：在Rust作为系统编程语言日益流行的当下，坚持纯C++意味着放弃了Rust的内存安全保证和现代工具链。但项目的实际约束（Windows Only、COM密集使用、不需要跨平台）使得C++成为更务实的选型。

Sources: [CLAUDE.md](CLAUDE.md#L1-L241), [protocol/protocol.h](protocol/protocol.h#L1-L80), [agent/src/agent.cpp](agent/src/agent.cpp#L1-L200)

---

> **下一步阅读建议**：
> - 想深入了解每个模块的完整职责？→ [项目全貌：logger/capture/monitor_app/agent/ai/model 六大模块职责分工](4-xiang-mu-quan-mao-logger-capture-monitor_app-agent-ai-model-liu-da-mo-kuai-zhi-ze-fen-gong)
> - 想了解系统的长期发展方向？→ [长期愿景：自组织层次化视觉AI — 从井字棋到通用游戏的泛化路径](5-chang-qi-yuan-jing-zi-zu-zhi-ceng-ci-hua-shi-jue-ai-cong-jing-zi-qi-dao-tong-yong-you-xi-de-fan-hua-lu-jing)
> - 想深入游戏本体的TUI交互和网络协议？→ [游戏逻辑：3x3棋盘规则、胜负判定与控制台终端TUI交互](6-you-xi-luo-ji-3x3qi-pan-gui-ze-sheng-fu-pan-ding-yu-kong-zhi-tai-zhong-duan-tuijiao-hu)