/**
 * TicTacToe - Board state & rules
 */
#pragma once
#include <string>

// 3x3 board, '.'=empty, 'X', 'O'
extern char board[3][3];

void reset_board();
void print_board();
bool is_valid(int r, int c);
bool is_occupied(int r, int c);
bool check_win(char player);
bool is_draw();

/** Parse user input "row col", returns false on bad format */
bool parse_input(const std::string& line, int& row, int& col);
