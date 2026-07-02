// Shared signal handling + quit flag implementation
#include "signals.hpp"

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
#else
  #include <csignal>
#endif

volatile int g_quit_flag = 0;

#ifdef _WIN32
static BOOL WINAPI global_console_handler(DWORD dwType) {
    if (dwType == CTRL_C_EVENT || dwType == CTRL_BREAK_EVENT) {
        g_quit_flag = 1;
        return TRUE;
    }
    return FALSE;
}
#else
static void global_sig_handler(int) { g_quit_flag = 1; }
#endif

void setup_global_signals() {
#ifdef _WIN32
    SetConsoleCtrlHandler(global_console_handler, TRUE);
#else
    std::signal(SIGINT, global_sig_handler);
    std::signal(SIGTERM, global_sig_handler);
#endif
}
