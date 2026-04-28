"""训练井字棋 AI——自我对弈 + 保存权重为 .pkl。

用法:
    python train.py                          # 快速训练，保存到 model.pkl
    python train.py --load model.pkl         # 加载已有权重继续训练
    python train.py --iters 100 --games 200  # 更多迭代和局数
"""

import argparse
import os
from model import TicTacToeModel


def main():
    parser = argparse.ArgumentParser(description="训练井字棋 AI（自我对弈）")
    parser.add_argument('--load', type=str, default=None, help='加载已有 .pkl 权重')
    parser.add_argument('--save', type=str, default='model.pkl', help='保存路径 (.pkl)')
    parser.add_argument('--iters', type=int, default=50, help='自我对弈迭代次数')
    parser.add_argument('--games', type=int, default=100, help='每次迭代对弈局数')
    parser.add_argument('--epochs', type=int, default=5, help='每次迭代训练轮数')
    parser.add_argument('--batch-size', type=int, default=64)
    parser.add_argument('--lr', type=float, default=0.001)
    parser.add_argument('--eps-start', type=float, default=0.8)
    parser.add_argument('--eps-end', type=float, default=0.05)
    parser.add_argument('--eps-decay', type=float, default=0.995)
    args = parser.parse_args()

    ai = TicTacToeModel(model_path=args.load)
    if args.load:
        print(f"已加载权重: {args.load}")

    ai.train(
        iterations=args.iters,
        games_per_iter=args.games,
        epochs=args.epochs,
        batch_size=args.batch_size,
        lr=args.lr,
        eps_start=args.eps_start,
        eps_end=args.eps_end,
        eps_decay=args.eps_decay,
        save_path=args.save,
        verbose=True,
    )


if __name__ == '__main__':
    main()
