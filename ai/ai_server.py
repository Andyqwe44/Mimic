"""
ai_server.py — 井字棋 AI TCP 预测服务
=======================================
监听本地端口，接收 C++ 游戏客户端发来的棋盘状态，返回最佳落子。

协议（纯文本，换行分隔）：
    客户端请求:  "b0 b1 b2 b3 b4 b5 b6 b7 b8 player\n"
                 9 个棋盘值（+1=X, -1=O, 0=空）+ 当前执棋方
    服务端响应:  "row col value\n"
                 最佳落子坐标 + 局面评估值 [-1, 1]
    游戏结束:    客户端发送 "END winner\n"（1=X胜, -1=O胜, 0=平局）

数据成员:
    model:         TicTacToeNet — 神经网络模型
    port:          int — 监听端口
    training_data: list — 累积的训练样本
    ready:         Event — 服务就绪信号
"""

import random
import socket
import threading
import torch
from model import TicTacToeNet


class AIServer:
    """AI 预测 TCP 服务端。

    数据成员:
        model:         TicTacToeNet — 已加载权重的网络
        port:          int — TCP 监听端口
        running:       bool — 服务运行标志
        ready:         threading.Event — 就绪信号（bind 完成后设置）
        training_data: list — 训练模式时累积的 (state, policy_target, value_target)
    """

    # ==================== 数据成员 ====================
    # model:         神经网络
    # port:          监听端口
    # running:       运行标志
    # ready:         就绪事件
    # training_data: 训练数据缓冲
    # _sock:         服务端 socket

    def __init__(self, model=None, port=9999, hidden=128, epsilon=0):
        """初始化服务端。

        Args:
            model:   TicTacToeNet or None — 模型实例，None 则创建随机权重
            port:    int — 监听端口
            hidden:  int — 隐藏层宽度（仅在 model=None 时使用）
            epsilon: float — 探索率：随机落子概率 [0, 1]，0 = 纯 argmax
        """
        self.model = model if model is not None else TicTacToeNet(hidden=hidden)
        self.port = port
        self.epsilon = epsilon
        self.running = False
        self.ready = threading.Event()
        self.training_data = []
        self.game_count = 0      # 已完成对局计数
        self._sock = None

    # ==================== 服务生命周期 ====================

    def start(self):
        """启动服务（阻塞，在后台线程中调用）。"""
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._sock.bind(('127.0.0.1', self.port))
        self._sock.listen(5)
        self._sock.settimeout(1.0)  # 每秒检查 running 标志
        self.running = True
        self.ready.set()            # 通知主线程：已就绪

        while self.running:
            try:
                conn, addr = self._sock.accept()
                self._handle_client(conn)
            except socket.timeout:
                continue
            except Exception as e:
                if self.running:
                    print(f"[Server] accept error: {e}")

    def stop(self):
        """停止服务。"""
        self.running = False
        if self._sock:
            try:
                self._sock.close()
            except Exception:
                pass

    # ==================== 客户端处理 ====================

    def _handle_client(self, conn):
        """处理一个客户端的一局完整对弈。

        流程：
            收到 9+1 个数 → 模型推理 → 返回 row col
            收到 END → 构建训练样本 → 断开
        """
        game_history = []  # 本局记录: [(board, move, player), ...]

        try:
            buf = ""
            while True:
                data = conn.recv(4096)
                if not data:
                    break
                buf += data.decode('utf-8')

                # 按行处理
                while '\n' in buf:
                    line, buf = buf.split('\n', 1)
                    line = line.strip()
                    if not line:
                        continue

                    # 游戏结束消息
                    if line.startswith('END'):
                        parts = line.split()
                        winner = int(parts[1]) if len(parts) > 1 else 0
                        # 从对弈历史构建训练样本
                        for board, move, pl in game_history:
                            state = torch.tensor(
                                [cell * pl for cell in board], dtype=torch.float32
                            )
                            policy_target = torch.zeros(9)
                            policy_target[move] = 1.0
                            value_target = winner * pl
                            self.training_data.append((
                                state, policy_target,
                                torch.tensor([value_target], dtype=torch.float32)
                            ))
                        self.game_count += 1  # 对局计数 +1
                        return

                    # 落子请求: "b0 b1 ... b8 player"
                    parts = line.split()
                    if len(parts) != 10:
                        continue

                    board = [int(x) for x in parts[:9]]
                    player = int(parts[9])

                    # 模型推理
                    state = torch.tensor(
                        [cell * player for cell in board], dtype=torch.float32
                    )
                    with torch.no_grad():
                        logits, value = self.model(state.unsqueeze(0))
                        logits = logits.squeeze(0)

                    # 屏蔽已落子位置
                    for i in range(9):
                        if board[i] != 0:
                            logits[i] = -float('inf')

                    # epsilon-greedy 选择走法
                    valid = [i for i in range(9) if board[i] == 0]
                    if random.random() < self.epsilon:
                        move = random.choice(valid)
                    else:
                        move = logits.argmax().item()
                    row, col = move // 3, move % 3

                    # 记录历史
                    game_history.append((board, move, player))

                    # 返回结果
                    resp = f"{row} {col} {value.item():.4f}\n"
                    conn.sendall(resp.encode('utf-8'))

        except Exception as e:
            print(f"[Server] client error: {e}")
        finally:
            try:
                conn.close()
            except Exception:
                pass

    # ==================== 训练数据管理 ====================

    def get_training_data(self):
        """获取并清空累积的训练数据。"""
        data = self.training_data
        self.training_data = []
        return data


# ==================== 独立运行 ====================

if __name__ == '__main__':
    import argparse
    from model import TicTacToeModel

    parser = argparse.ArgumentParser(description="井字棋 AI 预测服务")
    parser.add_argument('--model', type=str, default='model.pkl', help='模型权重文件')
    parser.add_argument('--port', type=int, default=9999, help='监听端口')
    parser.add_argument('--hidden', type=int, default=128, help='隐藏层宽度')
    args = parser.parse_args()

    model = TicTacToeModel(model_path=args.model, hidden=args.hidden)
    server = AIServer(model=model.model, port=args.port)
    print(f"AI 服务启动: 127.0.0.1:{args.port}")
    print("等待 C++ 客户端连接...")
    try:
        server.start()
    except KeyboardInterrupt:
        print("\n服务已停止")
        server.stop()
