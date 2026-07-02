# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Structure

```
├── Makefile              # root Makefile → nests game/
├── game/                 # C++ Tic Tac Toe (game client)
│   ├── main.cpp          # terminal game + TCP client (3 modes)
│   ├── main.exe
│   └── Makefile
└── ai/                   # PyTorch AI (prediction server)
    ├── net.py            # TicTacToeNet(nn.Module) — MLP + policy/value heads
    ├── model.py          # TicTacToeModel — load, predict, save
    ├── ai_server.py      # AIServer — TCP server, handles game clients
    ├── api.py            # utility functions: board encode, moves, win/draw
    ├── train.py          # training orchestrator: start server + spawn C++ clients
    ├── play.py           # convenience: start server + one C++ client (human vs AI)
    └── requirements.txt
```

## Architecture: Client-Server IPC

```
Python Server (ai_server.py)         C++ Client (main.cpp)
        │                                  │
        │  TCP 127.0.0.1:9999             │  --server 127.0.0.1 9999
        │◄─────────────────────────────────│  "b0 b1 ... b8 player\n"
        │  predict → best move             │
        │─────────────────────────────────►│  "row col value\n"
        │                                  │
        │◄─────────────────────────────────│  "END winner\n"
```

C++ game modes:
- No args: Human vs Human
- `--server HOST PORT`: Connect to AI server (AI=X, Human=O)
- `--server HOST PORT --ai O`: AI plays O, Human plays X
- `--server HOST PORT --auto`: AI vs AI (training, no human input)
- `--delay N`: Auto-mode delay between moves (seconds)

Protocol (text, newline-delimited):
- Client → Server: `b0 b1 ... b8 player` (9 board values + player to move)
- Server → Client: `row col value` (move coordinates + evaluation)
- Game end: `END winner` (1=X, -1=O, 0=draw)

## Key invariant: C++ game is source of truth

- C++ manages board display, move validation, win/draw detection
- Python server is stateless per request: receives board, returns best move
- Training: Python spawns C++ `--auto` processes → C++ connects to server → plays full game → server collects (state, move, outcome) → trains model
- Human vs AI: human plays in C++ terminal, AI moves via server

## Build & Run

```bash
make game           # build C++ engine
make game run       # build + play human vs human
make train          # train AI → ai/model.pkl
make train-slow     # train with delay (watch C++ games connect to server)
make play           # human vs AI (AI=X)
make clean          # clean C++ binary
make clean-all      # clean C++ + model weights + Python cache
```

## AI (ai/)

### Training
```bash
cd ai
python train.py                          # quick train (50 iters × 100 games)
python train.py --load model.pkl         # continue training
python train.py --iters 200 --games 200  # longer training
python train.py --delay 0.5              # slow down (observe server logs)
```

Training flow: `train.py` → starts `AIServer` in thread → spawns `game/main.exe --auto` per game → C++ plays via server → collects training data → trains model.

### Human vs AI
```bash
cd ai
python play.py                           # AI=X, human=O
python play.py --human-first             # human=X, AI=O
python play.py --model my_weights.pkl    # use specific weights
```

`play.py` starts server + one C++ game with `--server` flag. Human plays directly in C++ terminal.

### Manual server/client
```bash
# Terminal 1
cd ai && python -m ai_server --model model.pkl --port 9999

# Terminal 2
cd game && ./main --server 127.0.0.1 9999          # human vs AI
cd game && ./main --server 127.0.0.1 9999 --auto   # AI vs AI
```

## Key files

**game/main.cpp** — ~220 lines. Platform socket (Winsock/BSD), arg parsing, 3 game modes. `g++ -Wall -Wextra -std=c++11 -lws2_32`.

**ai/net.py** — MLP: 9→hidden→hidden→hidden/2 → policy(9) + value(1, tanh). Default hidden=128.

**ai/model.py** — `TicTacToeModel`: load (auto-detect hidden from file), predict, predict_probs, save. ~100 lines.

**ai/ai_server.py** — `AIServer`: TCP server, accept clients, handle game protocol, collect training data. `training_data` list accumulates (state, policy_target, value_target) tuples per game.

**ai/train.py** — Training orchestrator: start server, spawn N C++ `--auto` games per iteration, train on collected data. Has its own `train_epoch()` for the training loop.

**ai/play.py** — Convenience: start server, spawn one C++ `--server --ai X|O` game. Human plays in C++ terminal.
