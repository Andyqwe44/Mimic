/**
 * TUI init/restore — raw input + ANSI support for Windows console
 */
#include "tui.hpp"

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
#else
  #include <termios.h>
  #include <unistd.h>
#endif

#include <iostream>

static bool g_tui_active = false;
#ifdef _WIN32
static DWORD g_old_in = 0, g_old_out = 0;
static HANDLE g_hin = nullptr, g_hout = nullptr;
#else
static struct termios g_old_termios;
#endif

bool tui_init() {
#ifdef _WIN32
    g_hin  = GetStdHandle(STD_INPUT_HANDLE);
    g_hout = GetStdHandle(STD_OUTPUT_HANDLE);

    // Enable ANSI escape code processing
    GetConsoleMode(g_hout, &g_old_out);
    DWORD mode = g_old_out | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    SetConsoleMode(g_hout, mode);

    // Raw input (no line buffering, no echo)
    GetConsoleMode(g_hin, &g_old_in);
    DWORD in_mode = g_old_in;
    in_mode &= ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT);
    in_mode |= ENABLE_WINDOW_INPUT;
    SetConsoleMode(g_hin, in_mode);

    // Hide blinking cursor
    std::cout << "\x1b[?25l" << std::flush;
    g_tui_active = true;
    return true;
#else
    tcgetattr(STDIN_FILENO, &g_old_termios);
    struct termios raw = g_old_termios;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    std::cout << "\x1b[?25l" << std::flush;
    g_tui_active = true;
    return true;
#endif
}

void tui_restore() {
    if (!g_tui_active) return;
    std::cout << "\x1b[?25h\x1b[0m\x1b[2J\x1b[H" << std::flush;
#ifdef _WIN32
    if (g_hin)  SetConsoleMode(g_hin, g_old_in);
    if (g_hout) SetConsoleMode(g_hout, g_old_out);
#else
    tcsetattr(STDIN_FILENO, TCSANOW, &g_old_termios);
#endif
    g_tui_active = false;
}
