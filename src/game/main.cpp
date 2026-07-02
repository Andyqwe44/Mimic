/**
 * TicTacToe - C++ Terminal Game
 *
 * Modes:
 *   (no args)                          Human vs Human (TUI, arrow keys)
 *   --server HOST PORT                 Human vs AI (TUI, AI auto)
 *   --server HOST PORT --auto          AI vs AI (training, text mode)
 */
#include "config.hpp"
#include "board.hpp"
#include "network.hpp"
#include "tui.hpp"
#include "../common/types.hpp"
#include "../common/signals.hpp"
#include <iostream>
#include <string>
#include <chrono>
#include <conio.h>

// ── ANSI helpers ──
#define ESC "\033"
static void a_home()   { printf(ESC "[H"); }
static void a_rst()    { printf(ESC "[0m"); }
static void a_bold()   { printf(ESC "[1m"); }
static void a_dim()    { printf(ESC "[2m"); }
static void a_inv()    { printf(ESC "[7m"); }
static void a_grn()    { printf(ESC "[32m"); }
static void a_blu()    { printf(ESC "[34m"); }
static void a_yel()    { printf(ESC "[33m"); }
static void a_wht_bg() { printf(ESC "[47m"); }
static void a_gry()    { printf(ESC "[90m"); }
static void a_clr()    { printf(ESC "[2J" ESC "[H"); }
static void a_cur_hide(){ printf(ESC "[?25l"); }
static void a_cur_show(){ printf(ESC "[?25h"); }

// ── ASCII grid (works everywhere, looks clean) ──
static void g_top() { a_gry(); printf("      +---+---+---+"); a_rst(); printf("\n"); }
static void g_mid() { a_gry(); printf("      +---+---+---+"); a_rst(); printf("\n"); }
static void g_bot() { a_gry(); printf("      +---+---+---+"); a_rst(); printf("\n"); }

static void g_row(int r, int cur_r, int cur_c, bool cur_vis, char player) {
    printf("      ");
    a_gry(); printf("|"); a_rst();
    for (int c = 0; c < 3; c++) {
        bool cur_here = (r == cur_r && c == cur_c) && cur_vis;
        bool occupied = board[r][c] != '.';
        printf(" ");

        if (cur_here && occupied) {
            // Cursor on occupied cell — white background highlight
            a_wht_bg(); a_bold();
            if (board[r][c] == 'X') { a_grn(); printf("X"); }
            else                    { a_blu(); printf("O"); }
            a_rst();
        } else if (cur_here && !occupied) {
            // Cursor on empty cell — invert whole cell
            a_inv(); printf("%c", player); a_rst();
        } else if (occupied) {
            // Occupied, no cursor
            a_bold();
            if (board[r][c] == 'X') { a_grn(); printf("X"); }
            else                    { a_blu(); printf("O"); }
            a_rst();
        } else {
            // Empty, no cursor — dim dot placeholder
            a_dim(); printf("."); a_rst();
        }

        printf(" ");
        a_gry(); printf("|"); a_rst();
    }
    printf("\n");
}

static void g_status(const char* msg) {
    // Pad to 16 chars so shorter strings don't leave residue
    printf("  |  "); a_yel(); printf("%-16s", msg ? msg : ""); a_rst();
}

static void wait_any_key() {
    printf("  Press any key to continue...\n"); fflush(stdout);
    while (!_kbhit()) Sleep(50); _getch();
}

static bool ask_replay() {
    a_clr();
    printf("  Play again?\n\n");
    printf("    [Enter]  Yes\n");
    printf("    [Esc]    No\n");
    fflush(stdout);
    while (true) {
        if (_kbhit()) {
            int c = _getch();
            if (c == '\r') return true;
            if (c == 0x1B || c == 'q' || c == 'Q') return false;
        }
        Sleep(50);
    }
}

// ==================== TUI: get human move ====================

static bool tui_get_move(int& row, int& col, char player) {
    int cr = 1, cc = 1;
    bool cv = true;
    auto last_blink = std::chrono::steady_clock::now();
    auto flash_until = std::chrono::steady_clock::now();  // flash expiry
    bool flash_on = false;

    auto cur_status = [&]() -> const char* {
        if (flash_on && std::chrono::steady_clock::now() < flash_until) return "Cell taken!";
        flash_on = false;
        return "Your turn";
    };

    auto draw = [&]() {
        a_home();
        printf("  Tic Tac Toe  |  Player: ");
        a_bold();
        if (player == 'X') { a_grn(); printf("X"); }
        else               { a_blu(); printf("O"); }
        a_rst();
        printf("  |  arrows:move  enter:place  esc:quit"); g_status(cur_status());
        printf("\n");

        g_top();
        for (int r = 0; r < 3; r++) {
            g_row(r, cr, cc, cv, player);
            if (r < 2) g_mid();
        }
        g_bot();
        printf("\n");
        fflush(stdout);
    };

    draw();

    while (true) {
        int key = 0;
        if (_kbhit()) {
            int c = _getch();
            if (c == 0xE0 || c == 0) {
                switch (_getch()) {
                case 0x48: key = -1; break; case 0x50: key = -2; break;
                case 0x4B: key = -3; break; case 0x4D: key = -4; break;
                }
            } else if (c == '\r') key = -5;
            else if (c == 0x1B)  key = -6;
            else key = c;
        }
        if (key == 0) {
            auto now = std::chrono::steady_clock::now();
            if (now - last_blink >= std::chrono::milliseconds(400)) {
                cv = !cv; last_blink = now; draw();
            }
            Sleep(16); continue;
        }
        cv = true; last_blink = std::chrono::steady_clock::now();

        switch (key) {
        case -1: cr = cr > 0 ? cr-1 : 0; break;
        case -2: cr = cr < 2 ? cr+1 : 2; break;
        case -3: cc = cc > 0 ? cc-1 : 0; break;
        case -4: cc = cc < 2 ? cc+1 : 2; break;
        case -5:
            if (!is_occupied(cr, cc)) { row = cr; col = cc; return true; }
            // Failed — flash "Cell taken!" for 1 second
            flash_on = true;
            flash_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
            break;
        case -6: case 'q': case 'Q': return false;
        }
        draw();
    }
}

// ==================== Game modes ====================

static int play_tui_game() {
    reset_board();
    char cur = 'X'; int r, c;
    a_clr();
    while (!g_quit_flag) {
        if (!tui_get_move(r, c, cur)) return 0;
        board[r][c] = cur;
        a_clr(); print_board();
        if (check_win(cur)) { a_yel(); a_bold(); printf("  *** %c wins! ***\n", cur); a_rst(); wait_any_key(); return 1; }
        if (is_draw())      { a_yel(); printf("  *** Draw! ***\n"); a_rst(); wait_any_key(); return 1; }
        cur = (cur == 'X') ? 'O' : 'X';
    }
    return 0;
}

static int play_tui_vs_ai(SOCKET sock) {
    reset_board();
    char human = (g_cfg.ai_player == 'X') ? 'O' : 'X';
    char cur = 'X'; int r, c;
    a_clr();
    while (!g_quit_flag) {
        if (cur == human) {
            if (!tui_get_move(r, c, cur)) return 0;
            board[r][c] = cur;
        } else {
            float v;
            if (!get_ai_move(sock, cur, r, c, v)) { printf("\nAI error\n"); wait_any_key(); return 0; }
            board[r][c] = cur;
            a_home(); printf("  AI plays %d %d (%.2f)\n", r, c, v);
        }
        a_clr(); print_board();
        if (check_win(cur)) {
            if (cur == human) { a_grn(); printf("  *** You win! ***\n"); }
            else              { a_yel(); printf("  *** AI wins! ***\n"); }
            a_rst(); if (g_cfg.use_server) send_end(sock, cur == 'X' ? 1 : -1);
            wait_any_key(); return 1;
        }
        if (is_draw()) { a_yel(); printf("  *** Draw! ***\n"); a_rst(); if (g_cfg.use_server) send_end(sock, 0); wait_any_key(); return 1; }
        cur = (cur == 'X') ? 'O' : 'X';
    }
    return 0;
}

static int play_classic(SOCKET sock) {
    char cur = 'X'; std::string line;
    while (!g_quit_flag) {
        bool ai = g_cfg.use_server && (g_cfg.ai_player == 'B' || cur == g_cfg.ai_player);
        if (ai) {
            int r, c; float v;
            if (!get_ai_move(sock, cur, r, c, v)) { std::cerr << "AI error\n"; return 0; }
            board[r][c] = cur;
            std::cout << "AI (" << cur << "): " << r << " " << c << " (" << v << ")" << std::endl;
        } else {
            std::cout << "Player " << cur << " > " << std::flush;
            if (!std::getline(std::cin, line)) return 0;
            if (line == "q" || line == "quit" || line == "exit") return 0;
            int r, c;
            if (!parse_input(line, r, c)) { std::cout << "Invalid.\n"; continue; }
            if (!is_valid(r, c)) { std::cout << "Range 0-2.\n"; continue; }
            if (is_occupied(r, c)) { std::cout << "Taken.\n"; continue; }
            board[r][c] = cur;
        }
        print_board();
        if (check_win(cur)) { std::cout << cur << " wins!\n"; if (g_cfg.use_server) send_end(sock, cur=='X'?1:-1); return ai?2:1; }
        if (is_draw())      { std::cout << "Draw!\n";        if (g_cfg.use_server) send_end(sock, 0); return ai?2:1; }
        cur = (cur == 'X') ? 'O' : 'X';
        if (ai && g_cfg.move_delay > 0) sleep_ms(g_cfg.move_delay * 1000);
    }
    return 0;
}

// ==================== Main ====================

int main(int argc, char* argv[]) {
    parse_args(argc, argv);
    WinsockGuard wsa;
    setup_global_signals();
    bool tui = !g_cfg.use_server || g_cfg.ai_player != 'B';
    if (tui) tui_init();

    int gd = g_cfg.game_delay >= 0 ? g_cfg.game_delay : 1;
    for (int gn = 1; !g_quit_flag; gn++) {
        SOCKET s = INVALID_SOCKET;
        if (g_cfg.use_server) { s = connect_to_server(g_cfg.server_host.c_str(), g_cfg.server_port); if (s == INVALID_SOCKET) return 1; }
        reset_board();
        int rs;
        if (g_cfg.ai_player == 'B')      rs = play_classic(s);
        else if (g_cfg.use_server)       rs = play_tui_vs_ai(s);
        else                             rs = play_tui_game();
        if (s != INVALID_SOCKET) closesocket(s);
        if (rs == 0 || g_quit_flag) break;
        if (g_cfg.ai_player == 'B') {
            if (g_cfg.max_games > 0 && gn >= g_cfg.max_games) break;
            for (int i = 0; i < gd * 10 && !g_quit_flag; i++) sleep_ms(100);
        } else {
            if (!ask_replay()) break;
        }
    }
    if (tui) tui_restore();
    else std::cout << "\nGoodbye!" << std::endl;
    return 0;
}
