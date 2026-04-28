"""
TicTacToeModel — 井字棋 AI 模型封装
====================================
提供训练、推理、保存/加载等高级 API。
内部使用 TicTacToeNet 神经网络 + 自我对弈训练。
"""

import torch
import torch.nn.functional as F
import pickle
import os
import random
from net import TicTacToeNet
from api import get_valid_moves, check_winner, is_draw


class TicTacToeModel:
    """井字棋 AI 模型：自我对弈训练 + 推理 + 持久化。

    数据成员:
        model: TicTacToeNet — 底层神经网络
    """

    # ==================== 数据成员（在 __init__ 中初始化） ====================
    # model: TicTacToeNet — 神经网络模型实例，包含所有可训练参数

    def __init__(self, model_path=None, hidden=128):
        """初始化模型。

        Args:
            model_path: str or None — 已保存的 .pkl 权重文件路径，None 则随机初始化
            hidden:      int — 隐藏层宽度，默认 128
        """
        self.model = TicTacToeNet(hidden=hidden)
        if model_path:
            if os.path.exists(model_path):
                self.load(model_path)
            else:
                print(f"[WARN] 模型文件不存在: {model_path}。使用随机初始化权重。")

    # ==================== 推理 ====================

    def predict(self, board, player):
        """给定棋盘，返回最佳落子和局面评估。

        Args:
            board:  list[int]，长度为 9，当前棋盘状态
            player: int，当前执棋方（+1 或 -1）

        Returns:
            (int, float): (最佳落子索引, 局面价值 [-1, 1])
        """
        state = torch.tensor([cell * player for cell in board], dtype=torch.float32)
        with torch.no_grad():
            logits, value = self.model(state.unsqueeze(0))
            logits = logits.squeeze(0)
        # 将已落子位置的 logit 设为 -inf，禁止选择
        for i in range(9):
            if board[i] != 0:
                logits[i] = -float('inf')
        return logits.argmax().item(), value.item()

    def predict_probs(self, board, player):
        """返回所有合法落子及其概率（按概率降序排列）。

        Args:
            board:  list[int]，长度为 9
            player: int，当前执棋方（+1 或 -1）

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
        """保存模型权重到 .pkl 文件。

        Args:
            path: str — 保存路径，建议以 .pkl 结尾
        """
        with open(path, 'wb') as f:
            pickle.dump(self.model.state_dict(), f)

    def load(self, path):
        """从 .pkl 文件加载模型权重。

        Args:
            path: str — 权重文件路径
        """
        with open(path, 'rb') as f:
            state_dict = pickle.load(f)
        self.model.load_state_dict(state_dict)
        self.model.eval()  # 切换到评估模式（关闭 dropout 等）

    # ==================== 自我对弈 ====================

    def _play_game(self, epsilon):
        """自我对弈一局，使用 epsilon-greedy 探索策略。

        Args:
            epsilon: float — 随机落子概率 [0, 1]，用于探索

        Returns:
            list[(state, policy_target, value_target)]: 训练样本
        """
        board = [0] * 9
        player = 1          # 当前执棋方：+1 = X, -1 = O
        history = []        # 记录每步的 (state, move, player)

        while True:
            state = torch.tensor([cell * player for cell in board], dtype=torch.float32)

            with torch.no_grad():
                logits, _ = self.model(state.unsqueeze(0))
                logits = logits.squeeze(0)

            for i in range(9):
                if board[i] != 0:
                    logits[i] = -float('inf')

            valid = get_valid_moves(board)
            # epsilon-greedy: 以 epsilon 概率随机探索，否则选择模型认为最优的走法
            if random.random() < epsilon:
                move = random.choice(valid)
            else:
                move = logits.argmax().item()

            history.append((state, move, player))
            board[move] = player

            winner = check_winner(board)
            if winner != 0:
                outcome = winner   # +1 或 -1
                break
            if is_draw(board):
                outcome = 0        # 平局
                break
            player = -player       # 切换执棋方

        # 从对弈历史构建训练样本
        examples = []
        for state, move, pl in history:
            policy_target = torch.zeros(9)
            policy_target[move] = 1.0  # 策略标签：one-hot 编码实际落子
            value_target = outcome * pl  # 价值标签：从最终结果回推
            examples.append((state, policy_target, torch.tensor([value_target], dtype=torch.float32)))
        return examples

    # ==================== 训练循环 ====================

    def _train_epoch(self, optimizer, data, batch_size):
        """单轮训练：遍历所有训练数据一次。

        Args:
            optimizer:   torch.optim 优化器
            data:        list[(state, policy_target, value_target)]
            batch_size:  int，批大小

        Returns:
            (float, float, float): (总损失, 策略损失, 价值损失)
        """
        self.model.train()  # 切换到训练模式
        random.shuffle(data)
        total_loss = total_pol = total_val = 0.0
        n = 0

        for i in range(0, len(data), batch_size):
            batch = data[i:i + batch_size]
            states = torch.stack([ex[0] for ex in batch])
            policy_targets = torch.stack([ex[1] for ex in batch])
            value_targets = torch.stack([ex[2] for ex in batch])

            logits, values = self.model(states)
            values = values.squeeze(-1)  # (batch,) 去掉最后一维

            # 策略损失：交叉熵（仅对胜/平局做加权，负局无意义学习）
            loss_policy = F.cross_entropy(logits, policy_targets, reduction='none')
            weight = (value_targets.squeeze(-1) >= -0.5).float()
            loss_policy = (loss_policy * weight).sum() / (weight.sum() + 1e-8)

            # 价值损失：均方误差
            loss_value = F.mse_loss(values, value_targets.squeeze(-1))
            loss = loss_policy + loss_value

            optimizer.zero_grad()
            loss.backward()
            optimizer.step()

            total_loss += loss.item()
            total_pol += loss_policy.item()
            total_val += loss_value.item()
            n += 1

        return total_loss / n, total_pol / n, total_val / n

    def train(self, iterations=50, games_per_iter=100, epochs=5, batch_size=64,
              lr=0.001, eps_start=0.8, eps_end=0.05, eps_decay=0.995,
              save_path=None, verbose=True):
        """自我对弈训练主循环。

        每轮迭代流程：
            1. 自我对弈生成训练数据（epsilon-greedy 探索）
            2. 用这些数据训练模型多个 epoch
            3. 衰减 epsilon（逐渐减少探索，增加利用）

        Args:
            iterations:     int — 自我对弈迭代次数，默认 50
            games_per_iter: int — 每次迭代的对弈局数，默认 100
            epochs:         int — 每次迭代的训练轮数，默认 5
            batch_size:     int — 批大小，默认 64
            lr:             float — 学习率，默认 0.001
            eps_start:      float — 初始探索率，默认 0.8
            eps_end:        float — 最低探索率，默认 0.05
            eps_decay:      float — 每次迭代探索率衰减系数，默认 0.995
            save_path:      str or None — 保存路径，None 则不保存
            verbose:        bool — 是否打印训练进度

        Returns:
            list[(int, float, float)]: 训练历史 [(迭代, 平均损失, epsilon), ...]
        """
        optimizer = torch.optim.Adam(self.model.parameters(), lr=lr)
        epsilon = eps_start
        history = []

        if verbose:
            print(f"{'Iter':>5s} {'Games':>6s} {'Eps':>6s} {'Loss':>8s} {'Pol':>8s} {'Val':>8s}")
            print("-" * 53)

        for it in range(1, iterations + 1):
            # 1. 自我对弈生成数据
            data = []
            for _ in range(games_per_iter):
                data.extend(self._play_game(epsilon))

            # 2. 训练
            for _ in range(epochs):
                avg_loss, avg_pol, avg_val = self._train_epoch(optimizer, data, batch_size)

            # 3. 衰减 epsilon
            epsilon = max(eps_end, epsilon * eps_decay)
            history.append((it, avg_loss, epsilon))

            if verbose and it % 10 == 0:
                print(f"{it:5d} {len(data):6d} {epsilon:.4f} {avg_loss:8.4f} {avg_pol:8.4f} {avg_val:8.4f}")

        if save_path:
            self.save(save_path)
            if verbose:
                print(f"\n模型已保存到 {os.path.abspath(save_path)}")

        return history
