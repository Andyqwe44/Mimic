/**
 * TicTacToe - Board state & rules
 */
#include "board.hpp"
#include <iostream>

char board[3][3];

void reset_board() {
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            board[i][j] = '.';
}

void print_board() {
    std::cout << "\n";
    std::cout << "      +---+---+---+\n";
    for (int i = 0; i < 3; i++) {
        std::cout << "      |";
        for (int j = 0; j < 3; j++) {
            char c = board[i][j];
            std::cout << " " << (c == '.' ? ' ' : c) << " |";
        }
        std::cout << "\n";
        if (i < 2) std::cout << "      +---+---+---+\n";
    }
    std::cout << "      +---+---+---+\n\n";
}

bool is_valid(int r, int c) { return r >= 0 && r < 3 && c >= 0 && c < 3; }
bool is_occupied(int r, int c) { return board[r][c] != '.'; }

bool check_win(char p) {
    for (int i = 0; i < 3; i++) {
        if (board[i][0] == p && board[i][1] == p && board[i][2] == p) return true;
        if (board[0][i] == p && board[1][i] == p && board[2][i] == p) return true;
    }
    if (board[0][0] == p && board[1][1] == p && board[2][2] == p) return true;
    if (board[0][2] == p && board[1][1] == p && board[2][0] == p) return true;
    return false;
}

bool is_draw() {
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            if (board[i][j] == '.') return false;
    return true;
}

bool parse_input(const std::string& line, int& row, int& col) {
    // Parse two integers, separated by any non-digit
    // Limit digit count to prevent overflow
    const char* s = line.c_str();

    // Skip leading non-digits
    while (*s && (*s < '0' || *s > '9')) s++;
    if (!*s) return false;

    row = 0;
    int digits = 0;
    while (*s >= '0' && *s <= '9' && digits < 2) {
        row = row * 10 + (*s - '0');
        s++; digits++;
    }
    // Skip remaining digits (overflow protection)
    while (*s >= '0' && *s <= '9') s++;

    // Skip separator
    while (*s && (*s < '0' || *s > '9')) s++;
    if (!*s) return false;

    col = 0;
    digits = 0;
    while (*s >= '0' && *s <= '9' && digits < 2) {
        col = col * 10 + (*s - '0');
        s++; digits++;
    }
    while (*s >= '0' && *s <= '9') s++;

    return true;
}
