/**
 * Visual Game Agent - Main Loop
 *
 * Pipeline: Capture -> Preprocess -> Send to Server -> Recv Action -> Input Sim
 *
 * For TicTacToe PoC, uses existing text protocol with ai_server.py.
 * The agent acts as a "virtual human" — it sees the game window and
 * simulates mouse/keyboard input to play.
 */
#pragma once
#include <string>
#include <memory>
#include "../common/types.hpp"

struct AgentConfig {
    // Game window
    std::wstring window_title;          // e.g. L"Tic Tac Toe" or L"main.exe"

    // Model server
    std::string server_host = "127.0.0.1";
    int server_port = 9999;

    // Control
    int frame_interval_ms = 100;        // ms between actions (turn-based = longer)
    int game_delay_ms = 2000;           // delay between games
    int max_games = 0;                  // 0 = unlimited
    bool verbose = false;               // print latency per frame
    bool dry_run = false;               // if true, don't execute input (debug mode)
};

/** Main agent entry point */
int run_agent(const AgentConfig& cfg);
