/**
 * Game Agent Monitor — Runtime interpreter version
 *
 * Loads appwindow.slint at runtime (no pre-compilation needed).
 * Faster development iteration — edit .slint, restart, no recompile.
 *
 * Build:
 *   cl /EHsc /std:c++20 /I slint_bin/include main_interpreter.cpp
 *      /link slint_bin/lib/slint_cpp.lib user32.lib
 *      (slint_cpp.dll must be in PATH or next to exe)
 */
#define SLINT_FEATURE_FREESTANDING
#include <slint.h>
#include <chrono>
#include <thread>
#include <cstdio>
#include <cstdlib>

using namespace std::chrono;

int main() {
    printf("Game Agent Monitor — Slint Runtime Interpreter\n");

    // Compile .slint at runtime
    auto compiler = slint::ComponentCompiler::create();
    auto result = compiler->build_from_path("appwindow.slint");

    if (result.has_error()) {
        fprintf(stderr, "Slint compile error:\n%s\n", result.error().data());
        return 1;
    }

    auto app_window = result.value();
    auto instance = app_window->create();

    // ── Connect callbacks (if any) ──

    // ── Set initial properties ──
    instance->set_property("version", slint::SharedString("v0.1.0"));
    instance->set_property("status-text", slint::SharedString("Disconnected"));
    instance->set_property("connected", false);
    instance->set_property("server-addr", slint::SharedString("127.0.0.1:9999"));
    instance->set_property("fps", 0.0f);
    instance->set_property("latency-ms", 0.0f);

    // ── Status update thread ──
    std::thread([&instance]() {
        int tick = 0;
        while (true) {
            std::this_thread::sleep_for(milliseconds(1000));
            tick++;
            slint::invoke_from_event_loop([&instance, tick]() {
                instance->set_property("fps", 55.0f + (tick % 20) * 0.5f);
                instance->set_property("latency-ms", 3.0f + (tick % 8) * 0.3f);
            });
        }
    }).detach();

    // ── Event loop ──
    instance->run();
    return 0;
}
