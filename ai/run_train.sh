#!/bin/bash
# TicTacToe AI Training (bash/Mac/Linux)
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
echo "========================================="
echo "  TicTacToe AI Training"
echo "========================================="
echo
echo "[1/2] Starting AI training server..."
cd "$SCRIPT_DIR" && python train.py --iters 50 --games 100 &
TRAIN_PID=$!
sleep 3
echo "[2/2] Starting self-play games..."
echo
"$SCRIPT_DIR/../game/main" --server 127.0.0.1 9999 --auto --games 5000
echo
echo "========================================="
echo "  Training session complete."
echo "========================================="
wait $TRAIN_PID 2>/dev/null
