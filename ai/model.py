"""
TicTacToeModel — 井字棋 AI 模型
================================
提供模型加载、推理预测、权重持久化。
训练编排由 train.py 负责（启动 AI 服务 + C++ 客户端自弈）。
"""

import torch
import pickle
import os
from net import TicTacToeNet
from api import get_valid_moves


class TicTacToeModel:
    """井字棋 AI 模型：推理 + 持久化。

    数据成员:
        model: TicTacToeNet — 底层神经网络
    """

    # ==================== 数据成员（在 __init__ 中初始化） ====================
    # model: TicTacToeNet — 神经网络模型实例

    def __init__(self, model_path=None, hidden=128):
        """初始化模型。

        Args:
            model_path: str or None — 已保存的 .pkl 权重文件路径
            hidden:     int — 隐藏层宽度（仅 model_path 不存在或加载失败时使用）
        """
        if model_path and os.path.exists(model_path):
            # 从文件推断 hidden size，加载权重
            with open(model_path, 'rb') as f:
                state_dict = pickle.load(f)
            file_hidden = state_dict['fc1.weight'].shape[0]
            self.model = TicTacToeNet(hidden=file_hidden)
            self.model.load_state_dict(state_dict)
            self.model.eval()
        else:
            if model_path:
                print(f"[WARN] 模型文件不存在: {model_path}。使用随机初始化权重。")
            self.model = TicTacToeNet(hidden=hidden)

    # ==================== 推理 ====================

    def predict(self, board, player):
        """给定棋盘，返回最佳落子和局面评估。

        Args:
            board:  list[int]，长度为 9（+1=X, -1=O, 0=空）
            player: int，当前执棋方（+1 或 -1）

        Returns:
            (int, float): (最佳落子索引, 局面价值 [-1, 1])
        """
        state = torch.tensor([cell * player for cell in board], dtype=torch.float32)
        with torch.no_grad():
            logits, value = self.model(state.unsqueeze(0))
            logits = logits.squeeze(0)
        for i in range(9):
            if board[i] != 0:
                logits[i] = -float('inf')
        return logits.argmax().item(), value.item()

    def predict_probs(self, board, player):
        """返回所有合法落子及其概率。

        Args:
            board:  list[int]，长度为 9
            player: int，当前执棋方

        Returns:
            (list[(int, float)], float): (落子概率列表, 局面价值)
        """
        state = torch.tensor([cell * player for cell in board], dtype=torch.float32)
        with torch.no_grad():
            logits, value = self.model(state.unsqueeze(0))
            logits = logits.squeeze(0)
        for i in range(9):
            if board[i] != 0:
                logits[i] = -float('inf')
        probs = torch.softmax(logits, dim=0)
        valid = get_valid_moves(board)
        moves = [(i, probs[i].item()) for i in valid]
        moves.sort(key=lambda x: x[1], reverse=True)
        return moves, value.item()

    # ==================== 持久化 ====================

    def save(self, path):
        """保存模型权重到 .pkl 文件。"""
        with open(path, 'wb') as f:
            pickle.dump(self.model.state_dict(), f)

    def load(self, path):
        """从 .pkl 文件加载模型权重。"""
        with open(path, 'rb') as f:
            state_dict = pickle.load(f)
        self.model.load_state_dict(state_dict)
        self.model.eval()
