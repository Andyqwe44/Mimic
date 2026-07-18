// Shared types for cross-module use
#pragma once

struct Rect { int x, y, w, h; };

#ifdef _WIN32
  #define sleep_ms(ms) Sleep((DWORD)(ms))
#else
  #include <unistd.h>
  #define sleep_ms(ms) usleep((ms) * 1000)
#endif
