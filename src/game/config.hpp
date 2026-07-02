/**
 * TicTacToe - Config & CLI parsing
 */
#pragma once
#include <string>

struct Config {
    bool use_server = false;
    char ai_player = 'X';       // 'X', 'O', or 'B'=both
    std::string server_host = "127.0.0.1";
    int server_port = 9999;
    int max_games = 0;          // max games in auto mode (0=unlimited)
    int move_delay = 0;         // delay between moves (seconds)
    int game_delay = -1;        // delay between games (-1=use default 1s)
};

extern Config g_cfg;

void print_usage(const char* prog);
void parse_args(int argc, char* argv[]);
