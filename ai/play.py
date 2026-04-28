"""人机对战——终端版井字棋。

用法:
    python play.py                    # AI 执 X，人类执 O
    python play.py --model model.pkl  # 使用指定训练权重
    python play.py --human-first      # 人类执 X 先手
"""

import argparse
from model import TicTacToeModel
from api import check_winner, is_draw

# 棋子显示符号
SYMBOL = {1: 'X', -1: 'O', 0: '.'}


def print_board(board):
    """打印棋盘。"""
    print()
    print("  0 1 2")
    for i in range(3):
        row = [SYMBOL[board[i * 3 + j]] for j in range(3)]
        print(f"{i} {' '.join(row)}")
    print()


def get_human_move(board):
    """获取人类玩家输入。

    Returns:
        int or None: 格子索引 0-8，输入 q 则返回 None
    """
    while True:
        try:
            inp = input("你的落子 (行 列): ").strip()
            if inp.lower() in ('q', 'quit', 'exit'):
                return None
            parts = inp.split()
            if len(parts) < 2:
                # 尝试解析 "12" 连续输入格式
                if len(inp) >= 2 and inp.isdigit():
                    row, col = int(inp[0]), int(inp[1])
                else:
                    print("请输入两个数字，如: 1 1")
                    continue
            else:
                row, col = int(parts[0]), int(parts[1])

            if not (0 <= row <= 2 and 0 <= col <= 2):
                print("超出范围。请输入 0, 1, 2 中的数字。")
                continue

            idx = row * 3 + col
            if board[idx] != 0:
                print("该位置已有棋子，请选择其他位置。")
                continue

            return idx
        except (ValueError, IndexError):
            print("输入无效。请输入两个数字，如: 1 1")


def main():
    parser = argparse.ArgumentParser(description="人机对战井字棋")
    parser.add_argument('--model', type=str, default='model.pkl', help='模型权重文件 (.pkl)')
    parser.add_argument('--human-first', action='store_true', help='人类执 X 先手')
    args = parser.parse_args()

    ai = TicTacToeModel(model_path=args.model)

    human_player = 1 if args.human_first else -1
    ai_player = -human_player

    board = [0] * 9
    current = 1  # X 总是先手

    print("=== 井字棋 —— 人机对战 ===")
    print(f"你: {SYMBOL[human_player]}   AI: {SYMBOL[ai_player]}")
    print("输入行列号 (0-2)，如: 1 1。输入 'q' 退出。")
    print_board(board)

    while True:
        if current == human_player:
            move = get_human_move(board)
            if move is None:
                print("再见！")
                break
        else:
            move, value = ai.predict(board, current)
            row, col = move // 3, move % 3
            print(f"AI 落子: {row} {col}  (局面评估: {value:+.3f})")

        board[move] = current
        print_board(board)

        winner = check_winner(board)
        if winner != 0:
            print(f"{SYMBOL[winner]} 获胜！")
            break
        if is_draw(board):
            print("平局！")
            break

        current = -current  # 切换执棋方


if __name__ == '__main__':
    main()
