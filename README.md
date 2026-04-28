# Tic Tac Toe + AI

C++ 井字棋游戏 + PyTorch 自我对弈 AI。

## 项目结构

```
├── Makefile         # 根 Makefile（嵌套 game/）
├── game/            # 井字棋引擎 (C++)
│   ├── main.cpp
│   ├── main.exe
│   └── Makefile
└── ai/              # AI 模型 (Python + PyTorch)
    ├── net.py       # 神经网络定义 (TicTacToeNet)
    ├── model.py     # 模型封装：训练、推理、保存/加载 (TicTacToeModel)
    ├── api.py       # 工具函数：棋盘编码、走法、胜负判定
    ├── train.py     # CLI 训练脚本（自我对弈）
    ├── play.py      # 人机对战（纯 Python，终端界面）
    └── requirements.txt
```

## 快速开始

### 1. 编译游戏

```bash
make game       # 或: cd game && make
make game run   # 编译并运行（人类对战人类）
```

### 2. 安装依赖

```bash
pip install -r .\ai\requirements.txt
```

### 3. 训练 AI

```bash
make train      # 快速训练 → 保存到 ai/model.pkl
```

或手动：

```bash
cd ai
python train.py                          # 默认 50 轮 × 100 局，约 30 秒
python train.py --iters 200 --games 200  # 更充分训练
python train.py --load model.pkl         # 加载权重继续训练
python train.py --save my_model.pkl      # 保存到指定路径
```

训练采用自我对弈 (self-play)，MLP 同时输出策略 (policy head) 和价值 (value head)。权重保存为 `.pkl` 文件。

### 4. 人机对战

```bash
make play       # AI 执 X 先手
```

或手动：

```bash
cd ai
python play.py                    # AI = X, 人类 = O
python play.py --human-first      # 人类 = X 先手
python play.py --model model.pkl  # 使用指定权重
```

输入格式：`row col`（如 `1 1`），输入 `q` 退出。

## API 使用

```python
from model import TicTacToeModel
from api import check_winner, is_draw, get_valid_moves

# 训练
ai = TicTacToeModel()
ai.train(iterations=100, games_per_iter=150, save_path='model.pkl')

# 或加载已有权重
ai = TicTacToeModel(model_path='model.pkl')

# 推理
board = [0, 0, 0, 0, 1, 0, 0, 0, 0]  # 当前盘面: +1=X, -1=O, 0=空
move, value = ai.predict(board, player=1)     # 最佳落子 + 局面评估
probs, value = ai.predict_probs(board, player=1)  # 所有合法落子及概率

# 保存 / 加载
ai.save('model.pkl')
ai.load('model.pkl')
```

## Make 命令一览

| 命令 | 作用 |
|------|------|
| `make game` | 编译 C++ 游戏 |
| `make train` | 训练 AI |
| `make play` | 人机对战 |
| `make clean` | 清理 C++ 编译产物 |

## 技术架构

**game/** — 单文件 C++ 终端井字棋。无外部依赖，仅 `<iostream>` + `<string>`。`g++ -Wall -Wextra -std=c++11` 编译。

**ai/net.py** — PyTorch MLP：输入 9 维（当前执棋方视角），3 层全连接 (9→128→128→64)，双头输出：policy (9 类 logits) + value (tanh → [-1,1])。

**ai/model.py** — `TicTacToeModel` 类封装训练/推理/持久化。`train()` 使用 epsilon-greedy 自我对弈 + 交叉熵策略损失 + MSE 价值损失。权重通过 `pickle` 序列化 state_dict。

**ai/api.py** — 工具函数：棋盘编码、走法生成、胜负判定。纯函数，不依赖 PyTorch 模型。

**ai/play.py** — 纯 Python 终端交互，不依赖 C++ 引擎。AI 通过 `TicTacToeModel.predict()` 推理最佳落子。
