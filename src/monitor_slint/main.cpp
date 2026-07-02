/**
 * Game Agent Monitor — Central Hub (Slint C++)
 *
 * MAA-Meow style: monitor is the single entry point.
 * Launches game, AI server, and agent. Shows real-time game view.
 */

#define SLINT_FEATURE_FREESTANDING
#define WIN32_LEAN_AND_MEAN
#include "appwindow.h"
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <mutex>
#include <chrono>
#include <vector>

// ── Process Manager ──
struct ManagedProcess {
    PROCESS_INFORMATION pi;
    std::string name;
    bool running = false;
};

static std::vector<ManagedProcess> g_processes;
static std::mutex g_log_mutex;
static std::string g_log_buffer;

static void append_log(const char* msg) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    auto t = std::chrono::system_clock::now();
    auto tt = std::chrono::system_clock::to_time_t(t);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        t.time_since_epoch()) % 1000;

    char timebuf[32];
    std::tm local_tm;
    localtime_s(&local_tm, &tt);
    snprintf(timebuf, sizeof(timebuf), "%02d:%02d:%02d.%03lld",
             local_tm.tm_hour, local_tm.tm_min, local_tm.tm_sec, ms.count());
    g_log_buffer += std::string(timebuf) + "  " + msg + "\n";
}

static bool launch_process(const char* cmdline, const char* name) {
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};

    char cmd[512];
    strncpy_s(cmd, cmdline, 511);

    if (!CreateProcessA(nullptr, cmd, nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        char buf[256];
        snprintf(buf, sizeof(buf), "[ERR] Failed to launch %s: %s", name, cmdline);
        append_log(buf);
        return false;
    }

    char buf[256];
    snprintf(buf, sizeof(buf), "[OK] Launched %s (PID %d)", name, pi.dwProcessId);
    append_log(buf);

    g_processes.push_back({pi, name, true});
    return true;
}

static void kill_all_processes() {
    for (auto& p : g_processes) {
        if (p.running) {
            TerminateProcess(p.pi.hProcess, 0);
            CloseHandle(p.pi.hProcess);
            CloseHandle(p.pi.hThread);
            p.running = false;
            char buf[128];
            snprintf(buf, sizeof(buf), "[--] Stopped %s", p.name.c_str());
            append_log(buf);
        }
    }
    g_processes.clear();
}

// ── Main ──
int main() {
    printf("Game Agent Monitor v0.1.0 — Central Hub\n");

    auto app = AppWindow::create();

    app->set_version("v0.1.0");
    app->set_status_text("Idle");
    app->set_connected(false);
    app->set_task_running(false);
    app->set_task_name("TicTacToe");
    app->set_server_addr("127.0.0.1:9999");
    app->set_log_text("");
    app->set_fps(0.0f);
    app->set_latency_ms(0.0f);

    // ── Start Task callback ──
    app->on_start_task([&app]() {
        append_log("=== Starting TicTacToe Task ===");

        // 1. Launch AI server (Python)
        launch_process("python ai\\ai_server.py --port 9999", "AI Server");

        // 2. Launch game
        launch_process("game\\main.exe --server 127.0.0.1 9999 --auto", "Game Engine");

        // 3. Mark running
        app->set_task_running(true);
        app->set_status_text("Running");
        app->set_connected(true);
        app->set_log_text(slint::SharedString(g_log_buffer));

        append_log("=== Task Started ===");
        app->set_log_text(slint::SharedString(g_log_buffer));
    });

    // ── Stop Task callback ──
    app->on_stop_task([&app]() {
        append_log("=== Stopping Task ===");
        kill_all_processes();
        app->set_task_running(false);
        app->set_status_text("Idle");
        app->set_connected(false);
        app->set_log_text(slint::SharedString(g_log_buffer));
        append_log("=== All processes stopped ===");
        app->set_log_text(slint::SharedString(g_log_buffer));
    });

    // ── Status update timer ──
    std::thread([&app]() {
        int tick = 0;
        while (true) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            tick++;
            slint::invoke_from_event_loop([&app, tick]() {
                // Simulated FPS/latency for now
                if (g_processes.size() > 0) {
                    app->set_fps(58.0f + (tick % 10) * 0.3f);
                    app->set_latency_ms(3.5f + (tick % 6) * 0.4f);
                }
            });
        }
    }).detach();

    // ── Event loop ──
    app->run();

    // Cleanup on exit
    kill_all_processes();
    return 0;
}
