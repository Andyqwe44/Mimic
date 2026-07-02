/**
 * TicTacToe - Config & CLI parsing
 */
#include "config.hpp"
#include <iostream>
#include <cstdlib>
#include <cctype>

Config g_cfg;

void print_usage(const char* prog) {
    std::cout << "Tic Tac Toe - C++ Terminal Game\n\n"
              << "Usage: " << prog << " [options]\n\n"
              << "Modes:\n"
              << "  (no args)                          Human vs Human\n"
              << "  --server HOST PORT                 Human vs AI (AI=X, Human=O)\n"
              << "  --server HOST PORT --ai O          Human vs AI (AI=O, Human=X)\n"
              << "  --server HOST PORT --auto          AI vs AI (self-play training)\n\n"
              << "Options:\n"
              << "  --server HOST PORT   Connect to AI server at HOST:PORT\n"
              << "  --ai X|O|B           Which side the AI plays (B = both sides)\n"
              << "  --auto               Short for --ai B\n"
              << "  --games N            Max games in auto mode (default: unlimited)\n"
              << "  --delay N            Delay between moves in seconds (default: 0)\n"
              << "  --game-delay N       Delay between games in seconds (default: 1, auto mode)\n"
              << "  --help               Show this help\n"
              << std::endl;
}

void parse_args(int argc, char* argv[]) {
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            std::exit(0);
        } else if (arg == "--server" && i + 2 < argc) {
            g_cfg.use_server = true;
            g_cfg.server_host = argv[++i];
            g_cfg.server_port = std::atoi(argv[++i]);
        } else if (arg == "--auto") {
            g_cfg.ai_player = 'B';
        } else if (arg == "--ai" && i + 1 < argc) {
            g_cfg.ai_player = static_cast<char>(std::toupper(argv[++i][0]));
        } else if (arg == "--games" && i + 1 < argc) {
            g_cfg.max_games = std::atoi(argv[++i]);
        } else if (arg == "--delay" && i + 1 < argc) {
            g_cfg.move_delay = std::atoi(argv[++i]);
        } else if (arg == "--game-delay" && i + 1 < argc) {
            g_cfg.game_delay = std::atoi(argv[++i]);
        } else {
            std::cerr << "Unknown option: " << arg << "\nUse --help for usage.\n";
            std::exit(1);
        }
    }
}
