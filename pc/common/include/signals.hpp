// Shared signal handling + quit flag — single definition for all modules
#pragma once

extern volatile int g_quit_flag;

void setup_global_signals();
