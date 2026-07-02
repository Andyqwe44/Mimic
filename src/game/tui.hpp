/**
 * TUI — Arrow-key driven terminal UI for TicTacToe
 *
 * Features:
 *   - Arrow keys to move cursor on 3x3 grid
 *   - Blinking cursor at current cell
 *   - Enter to place piece, Esc to quit
 *   - ANSI escape codes for rendering (Windows 10+ / all Unix)
 */
#pragma once

/** Initialize terminal for raw input + ANSI support. Returns true on success. */
bool tui_init();

/** Restore terminal to normal mode. */
void tui_restore();

/** Run one game in TUI mode. Returns: 0=quit, 1=play again */
int  tui_play_game();
