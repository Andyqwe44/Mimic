"""训练井字棋 AI——持久训练服务。

作为后台服务运行，等待 C++ 游戏进程连接并进行自弈。
每收集够 games_per_iter 局数据后训练一轮，循环直到 iters 轮完成。

用法:
    python train.py --model model.pkl --port 9999 --iters 50 --games 100
"""

import argparse
import os
import sys
import threading
import time
import torch
import torch.nn.functional as F
import random
from model import TicTacToeModel
from ai_server import AIServer


def train_epoch(model, optimizer, data, batch_size):
    """单轮训练。"""
    model.model.train()
    random.shuffle(data)
    total_loss = total_pol = total_val = 0.0
    n = 0

    for i in range(0, len(data), batch_size):
        batch = data[i:i + batch_size]
        states = torch.stack([ex[0] for ex in batch])
        policy_targets = torch.stack([ex[1] for ex in batch])
        value_targets = torch.stack([ex[2] for ex in batch])

        logits, values = model.model(states)
        values = values.squeeze(-1)

        loss_policy = F.cross_entropy(logits, policy_targets, reduction='none')
        weight = (value_targets.squeeze(-1) >= -0.5).float()
        loss_policy = (loss_policy * weight).sum() / (weight.sum() + 1e-8)

        loss_value = F.mse_loss(values, value_targets.squeeze(-1))
        loss = loss_policy + loss_value

        optimizer.zero_grad()
        loss.backward()
        optimizer.step()

        total_loss += loss.item()
        total_pol += loss_policy.item()
        total_val += loss_value.item()
        n += 1

    if n == 0:
        return 0.0, 0.0, 0.0
    return total_loss / n, total_pol / n, total_val / n


def main():
    parser = argparse.ArgumentParser(description="训练井字棋 AI（持久训练服务）")
    parser.add_argument('--model', type=str, default='model.pkl', help='模型权重文件 (.pkl)')
    parser.add_argument('--save', type=str, default='model.pkl', help='保存路径 (.pkl)')
    parser.add_argument('--port', type=int, default=9999, help='AI 服务监听端口')
    parser.add_argument('--iters', type=int, default=50, help='训练迭代次数')
    parser.add_argument('--games', type=int, default=100, help='每次迭代对弈局数')
    parser.add_argument('--epochs', type=int, default=5, help='每次迭代训练轮数')
    parser.add_argument('--batch-size', type=int, default=64)
    parser.add_argument('--lr', type=float, default=0.001)
    parser.add_argument('--eps-start', type=float, default=0.8)
    parser.add_argument('--eps-end', type=float, default=0.05)
    parser.add_argument('--eps-decay', type=float, default=0.995)
    parser.add_argument('--hidden', type=int, default=128, help='隐藏层宽度')
    args = parser.parse_args()

    # 加载模型
    model = TicTacToeModel(model_path=args.model, hidden=args.hidden)

    # 启动 AI 预测服务（后台线程）
    server = AIServer(model=model.model, port=args.port, epsilon=args.eps_start)
    server_thread = threading.Thread(target=server.start, daemon=True)
    server_thread.start()
    server.ready.wait()

    print(f"训练服务已启动: 127.0.0.1:{args.port}")
    print(f"等待 C++ 游戏连接... (需 {args.games} 局/轮, 共 {args.iters} 轮)")
    print(f"{'Iter':>5s} {'Games':>7s} {'Eps':>6s} {'Loss':>8s} {'Pol':>8s} {'Val':>8s}")
    print("-" * 55)

    optimizer = torch.optim.Adam(model.model.parameters(), lr=args.lr)
    epsilon = args.eps_start

    try:
        for it in range(1, args.iters + 1):
            # 等待凑齐 games_per_iter 局
            target = it * args.games
            while server.game_count < target:
                time.sleep(0.5)

            # 获取训练数据并训练
            data = server.get_training_data()
            for _ in range(args.epochs):
                avg_loss, avg_pol, avg_val = train_epoch(model, optimizer, data, args.batch_size)

            # 衰减探索率
            epsilon = max(args.eps_end, epsilon * args.eps_decay)
            server.epsilon = epsilon

            if it % 10 == 0 or it == 1:
                print(f"{it:5d} {len(data):7d} {epsilon:.4f} {avg_loss:8.4f} {avg_pol:8.4f} {avg_val:8.4f}")

        model.save(args.save)
        print(f"\n训练完成。模型已保存到 {os.path.abspath(args.save)}")

    except KeyboardInterrupt:
        print(f"\n[中断] 正在退出...")
        model.save(args.save)
        print(f"当前模型已保存到 {os.path.abspath(args.save)}")
    finally:
        server.stop()


if __name__ == '__main__':
    main()
