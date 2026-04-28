"""
TicTacToeNet — 井字棋神经网络模型
===================================
MLP (多层感知机) 结构，包含策略头和价值头两个输出。
输入是 9 维棋盘状态，输出是 9 个位置的落子概率和局面评分。
"""

import torch
import torch.nn as nn
import torch.nn.functional as F


class TicTacToeNet(nn.Module):
    """井字棋神经网络：MLP + 策略头 + 价值头

    网络结构：
        输入层:  9 个神经元（棋盘 9 格，从当前执棋方视角编码）
                 +1 = 我方棋子, -1 = 对方棋子, 0 = 空位
        隐藏层:  9 → hidden → hidden → hidden//2（3 层全连接 + ReLU）
        输出层:  分叉为两个头
                 - 策略头 (policy head):  hidden//2 → 9（每格落子的 logits）
                 - 价值头 (value head):   hidden//2 → 1（tanh 压缩到 [-1, 1]）

    数据成员:
        fc1, fc2, fc3  — 三层全连接隐藏层
        policy         — 策略头全连接层
        value          — 价值头全连接层
    """

    # ==================== 数据成员（在 __init__ 中初始化） ====================
    # fc1:     nn.Linear   — 第1隐藏层   (9, hidden)
    # fc2:     nn.Linear   — 第2隐藏层   (hidden, hidden)
    # fc3:     nn.Linear   — 第3隐藏层   (hidden, hidden//2)
    # policy:  nn.Linear   — 策略输出层  (hidden//2, 9)  输出每个格子的落子倾向
    # value:   nn.Linear   — 价值输出层  (hidden//2, 1)  输出当前局面评分

    def __init__(self, hidden=128):
        """初始化网络结构。

        Args:
            hidden: 隐藏层宽度，默认 128。越大模型容量越大，但训练更慢。
        """
        super().__init__()
        # --- 隐藏层：逐层压缩特征 ---
        self.fc1 = nn.Linear(9, hidden)           # 9 → hidden
        self.fc2 = nn.Linear(hidden, hidden)       # hidden → hidden
        self.fc3 = nn.Linear(hidden, hidden // 2)  # hidden → hidden//2
        # --- 输出头：策略 + 价值 ---
        self.policy = nn.Linear(hidden // 2, 9)    # 策略头：9 个位置各一个 logit
        self.value = nn.Linear(hidden // 2, 1)     # 价值头：单个标量评分

    def forward(self, x):
        """前向传播。

        Args:
            x: shape (batch_size, 9) 的棋盘张量

        Returns:
            p: shape (batch_size, 9) 的策略 logits（未归一化）
            v: shape (batch_size, 1) 的局面价值，范围 [-1, 1]
        """
        x = F.relu(self.fc1(x))   # 第1隐藏层 + ReLU 激活
        x = F.relu(self.fc2(x))   # 第2隐藏层 + ReLU 激活
        x = F.relu(self.fc3(x))   # 第3隐藏层 + ReLU 激活
        p = self.policy(x)        # 策略头：线性输出（logits，未做 softmax）
        v = torch.tanh(self.value(x))  # 价值头：tanh 压缩到 [-1, 1]
        return p, v
