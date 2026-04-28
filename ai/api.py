"""
api.py — 井字棋工具函数
=======================
提供棋盘编码、走法生成、胜负判断等纯函数，不依赖 PyTorch 模型。
"""

import torch


# ==================== 棋盘编码 ====================

def board_to_tensor(board, player):
    """将棋盘转为张量，从指定执棋方视角编码。

    Args:
        board:  list[int]，长度为 9，+1 = X, -1 = O, 0 = 空
        player: int，当前执棋方（+1 或 -1）

    Returns:
        torch.Tensor: shape (9,)，我方棋子=+1，对方棋子=-1，空=0
    """
    return torch.tensor([cell * player for cell in board], dtype=torch.float32)


def encode_board(board, player):
    """board_to_tensor 的别名。"""
    return board_to_tensor(board, player)


# ==================== 走法工具 ====================

def get_valid_moves(board):
    """获取所有合法落子位置。

    Args:
        board: list[int]，长度为 9

    Returns:
        list[int]: 空格索引列表
    """
    return [i for i in range(9) if board[i] == 0]


def move_to_row_col(move):
    """平面索引转行列坐标。

    Args:
        move: int，0-8 的格子索引

    Returns:
        (int, int): (row, col)，范围 [0, 2]
    """
    return move // 3, move % 3


def row_col_to_move(row, col):
    """行列坐标转平面索引。

    Args:
        row: int，行号 [0, 2]
        col: int，列号 [0, 2]

    Returns:
        int: 0-8 的格子索引
    """
    return row * 3 + col


# ==================== 胜负判定 ====================

def check_winner(board):
    """检查是否有玩家获胜。

    Args:
        board: list[int]，长度为 9

    Returns:
        int: +1 = X 胜, -1 = O 胜, 0 = 无人获胜
    """
    lines = [
        [0, 1, 2], [3, 4, 5], [6, 7, 8],  # 行
        [0, 3, 6], [1, 4, 7], [2, 5, 8],  # 列
        [0, 4, 8], [2, 4, 6],              # 对角线
    ]
    for a, b, c in lines:
        if board[a] != 0 and board[a] == board[b] == board[c]:
            return board[a]
    return 0


def is_draw(board):
    """检查是否平局（棋盘已满且无胜者）。

    Args:
        board: list[int]，长度为 9

    Returns:
        bool: True = 平局
    """
    return all(cell != 0 for cell in board) and check_winner(board) == 0
