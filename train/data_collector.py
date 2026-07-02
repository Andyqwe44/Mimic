"""
Training Data Collector

Runs MLP self-play games, captures screen frames, records (frame, action) pairs.
Used to train the visual model (distillation from MLP teacher).

Workflow:
  1. Start AI server (ai_server.py) with trained MLP
  2. Start C++ game in --server --auto mode (MLP vs MLP via server)
  3. For each AI move:
     a. Capture game window (DXGI or GDI via C++ capture_test.exe)
     b. Record board state (via TCP protocol, for ground truth)
     c. Save (frame_tensor, action_token) pair
  4. After N games, save dataset to disk

The visual model then learns: frame → action (mimicking the MLP teacher).
"""
import subprocess
import socket
import struct
import time
import os
import sys
import pickle
import numpy as np
from pathlib import Path
from typing import List, Tuple, Optional
from dataclasses import dataclass

# Add parent to path
sys.path.insert(0, str(Path(__file__).parent.parent))

from model.action_space import (
    tictactoe_click_action, TOK_NOOP, MAX_ACTION_TOKENS
)


@dataclass
class TrainingSample:
    """One training sample: frame + action tokens"""
    frame: np.ndarray          # (4, 84, 84) float32, grayscale frame stack
    action_tokens: List[int]   # action token sequence from MLP teacher
    board_state: List[int]     # 9 board values (for debugging)
    player: int                # 1=X, -1=O
    value: float               # position evaluation from MLP


class DataCollector:
    """
    Collects training data by monitoring MLP self-play games.

    Uses the C++ game's TCP protocol to read board state and actions.
    For visual frames, captures the game console window.
    """

    def __init__(self, game_exe: str = "../game/main.exe",
                 ai_server_script: str = "../ai/ai_server.py",
                 model_path: str = "../ai/model.pkl",
                 capture_exe: str = "../capture/capture_test.exe",
                 server_port: int = 9999,
                 data_dir: str = "../data/training"):
        self.game_exe = game_exe
        self.ai_server_script = ai_server_script
        self.model_path = model_path
        self.capture_exe = capture_exe
        self.server_port = server_port
        self.data_dir = Path(data_dir)
        self.data_dir.mkdir(parents=True, exist_ok=True)

    def collect(self, num_games: int = 100, delay_ms: int = 50) -> List[TrainingSample]:
        """
        Collect training data from num_games of MLP self-play.

        For each move in each game:
        1. Connect to AI server to get the MLP's chosen move
        2. Record the board state
        3. Compute the action token sequence for that move
        4. (Frame capture would be done by C++ agent in production)

        Returns list of TrainingSample.
        """
        samples = []

        print(f"Collecting {num_games} games of MLP self-play data...")

        for game_num in range(1, num_games + 1):
            # Connect to server
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.connect(("127.0.0.1", self.server_port))

            board = [0] * 9  # 0=empty, 1=X, -1=O
            current_player = 1  # X starts

            moves_in_game = 0
            while True:
                # Send board state to server
                board_str = " ".join(str(b) for b in board) + f" {current_player}\n"
                sock.sendall(board_str.encode())

                # Receive move
                resp = sock.recv(256).decode().strip()
                if resp.startswith("END"):
                    # Game over
                    _, winner = resp.split()
                    winner = int(winner)
                    break

                parts = resp.split()
                if len(parts) < 3:
                    break

                row, col, value = int(parts[0]), int(parts[1]), float(parts[2])

                # Compute action token sequence
                action_tokens = tictactoe_click_action(row, col)

                # Create dummy frame (placeholder for real visual data)
                # In production, this would be captured by DXGI/GDI
                frame = np.zeros((4, 84, 84), dtype=np.float32)

                sample = TrainingSample(
                    frame=frame,
                    action_tokens=action_tokens,
                    board_state=board.copy(),
                    player=current_player,
                    value=value,
                )
                samples.append(sample)

                # Update board
                board[row * 3 + col] = current_player
                current_player = -current_player
                moves_in_game += 1

            sock.close()

            if game_num % 10 == 0:
                print(f"  Game {game_num}/{num_games}, {len(samples)} samples so far")

        print(f"Done: {len(samples)} samples from {num_games} games")

        # Save to disk
        self._save(samples)
        return samples

    def _save(self, samples: List[TrainingSample]):
        """Save training data to disk"""
        path = self.data_dir / "tictactoe_mlp_data.pkl"
        with open(path, "wb") as f:
            pickle.dump(samples, f)
        print(f"Saved {len(samples)} samples to {path}")

    @staticmethod
    def load(path: str) -> List[TrainingSample]:
        with open(path, "rb") as f:
            return pickle.load(f)


def main():
    """Standalone data collection"""
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument("--games", type=int, default=100)
    parser.add_argument("--port", type=int, default=9999)
    parser.add_argument("--output", type=str, default="../data/training")
    args = parser.parse_args()

    collector = DataCollector(
        server_port=args.port,
        data_dir=args.output,
    )
    samples = collector.collect(num_games=args.games)

    # Quick statistics
    if samples:
        values = [s.value for s in samples]
        print(f"\nStats:")
        print(f"  Samples: {len(samples)}")
        print(f"  Value mean: {np.mean(values):.3f}")
        print(f"  Value std:  {np.std(values):.3f}")
        print(f"  Avg tokens/sample: {np.mean([len(s.action_tokens) for s in samples]):.1f}")


if __name__ == "__main__":
    main()
