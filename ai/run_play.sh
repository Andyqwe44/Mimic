#!/bin/bash
# TicTacToe Human vs AI (bash/Mac/Linux)
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
echo "========================================="
echo "  TicTacToe - Human vs AI"
echo "========================================="
echo
echo "[1/2] Starting AI play server..."
cd "$SCRIPT_DIR" && python ai_server.py --model model.pkl --port 9999 &
sleep 2
echo "[2/2] Starting game..."
echo
"$SCRIPT_DIR/../game/main" --server 127.0.0.1 9999 --ai X
echo
echo "========================================="
echo "  Game over."
echo "========================================="
