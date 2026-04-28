# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Structure

```
├── Makefile         # root Makefile → nests game/
├── game/            # C++ Tic Tac Toe engine
│   ├── main.cpp
│   ├── main.exe
│   └── Makefile
└── ai/              # PyTorch AI model
    ├── model.py     # neural network + game logic
    ├── api.py       # clean API: train, predict, save/load .pkl
    ├── train.py         # CLI for self-play training → model.pkl
    ├── play.py          # human vs AI, terminal-style (pure Python)
    └── requirements.txt
```

## Build & Run

```bash
make game           # build C++ engine (nested → game/)
make train          # train AI → ai/model.pkl
make play           # human vs AI
make clean          # clean C++ binary
```

## AI (ai/)

```bash
cd ai
pip install -r requirements.txt          # install PyTorch
python train.py                          # quick train (50 iters × 100 games)
python train.py --load model.pkl         # continue training
python train.py --iters 200 --games 200  # longer training
python play.py                           # human vs AI (AI = X)
python play.py --human-first             # human = X first
python play.py --model model.pkl         # use specific weights
```

## Architecture

**game/** — Single-file C++ console Tic Tac Toe (`main.cpp`). No external dependencies beyond libstdc++. Uses `using namespace std`. Input via `getline`, compiled with `g++ -Wall -Wextra -std=c++11`.

**ai/** — PyTorch MLP with policy head (9 moves) + value head (game outcome). Trained via self-play. `api.py` provides `TicTacToeAI` class: `train()`, `predict()`, `predict_probs()`, `save()`, `load()`. Weights stored as `.pkl` (pickled state_dict). `play.py` is pure Python — no subprocess dependency on the C++ engine.
