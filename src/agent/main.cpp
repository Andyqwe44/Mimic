/**
 * Agent CLI Entry Point
 */
#include "agent.hpp"
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

int main(int argc, char* argv[]) {
    AgentConfig cfg;

    // Parse args
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--server") == 0 && i + 1 < argc) {
            // Parse host:port
            char* colon = strchr(argv[++i], ':');
            if (colon) {
                *colon = '\0';
                cfg.server_host = argv[i];
                cfg.server_port = atoi(colon + 1);
            }
        } else if (strcmp(argv[i], "--window") == 0 && i + 1 < argc) {
            // Convert to wide string
            wchar_t buf[256] = {};
            MultiByteToWideChar(CP_UTF8, 0, argv[++i], -1, buf, 256);
            cfg.window_title = buf;
        } else if (strcmp(argv[i], "--interval") == 0 && i + 1 < argc) {
            cfg.frame_interval_ms = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--games") == 0 && i + 1 < argc) {
            cfg.max_games = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--verbose") == 0) {
            cfg.verbose = true;
        } else if (strcmp(argv[i], "--dry-run") == 0) {
            cfg.dry_run = true;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Visual Game Agent\n\n");
            printf("Usage: agent.exe [options]\n\n");
            printf("Options:\n");
            printf("  --window TITLE     Game window title (required)\n");
            printf("  --server HOST:PORT AI server address (default: 127.0.0.1:9999)\n");
            printf("  --interval MS      Frame interval in ms (default: 100)\n");
            printf("  --games N          Max games (default: unlimited)\n");
            printf("  --verbose          Show per-frame latency\n");
            printf("  --dry-run          Don't simulate input (debug mode)\n");
            printf("  --help             Show this help\n");
            return 0;
        } else if (cfg.window_title.empty()) {
            // First positional arg = window title
            wchar_t buf[256] = {};
            MultiByteToWideChar(CP_UTF8, 0, argv[i], -1, buf, 256);
            cfg.window_title = buf;
        }
    }

    if (cfg.window_title.empty()) {
        fprintf(stderr, "Usage: agent.exe --window \"Game Window Title\"\n");
        fprintf(stderr, "Try: agent.exe --help\n");
        return 1;
    }

    return run_agent(cfg);
}
