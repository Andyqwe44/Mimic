/**
 * Agent CLI Entry Point
 */
#include "agent.hpp"
#include <windows.h>
#include <cstdlib>
#include <cstring>
#include "../../logger/logger.h"

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
            LOG("agent", "Visual Game Agent");
            LOG("agent", "Usage: agent.exe [options]");
            LOG("agent", "Options:");
            LOG("agent", "  --window TITLE     Game window title (required)");
            LOG("agent", "  --server HOST:PORT AI server address (default: 127.0.0.1:9999)");
            LOG("agent", "  --interval MS      Frame interval in ms (default: 100)");
            LOG("agent", "  --games N          Max games (default: unlimited)");
            LOG("agent", "  --verbose          Show per-frame latency");
            LOG("agent", "  --dry-run          Don't simulate input (debug mode)");
            LOG("agent", "  --help             Show this help");
            return 0;
        } else if (cfg.window_title.empty()) {
            // First positional arg = window title
            wchar_t buf[256] = {};
            MultiByteToWideChar(CP_UTF8, 0, argv[i], -1, buf, 256);
            cfg.window_title = buf;
        }
    }

    if (cfg.window_title.empty()) {
        LOG("agent", "Usage: agent.exe --window \"Game Window Title\"");
        LOG("agent", "Try: agent.exe --help");
        return 1;
    }

    return run_agent(cfg);
}
