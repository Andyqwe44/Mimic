/**
 * Screen Capture Test
 * Usage: capture_test.exe [window_title]
 *
 * Captures the specified window (or full screen) and prints stats.
 */
#include "capture.hpp"
#include "preprocess.hpp"
#include <cstdio>
#include <cstdlib>
#include <windows.h>

int main(int argc, char* argv[]) {
    const wchar_t* window_title = nullptr;
    wchar_t title_buf[256] = {};

    if (argc > 1) {
        // Convert to wide string
        MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, title_buf, 256);
        window_title = title_buf;
        printf("Target window: %s\n", argv[1]);
    } else {
        printf("Target: full screen\n");
    }

    // Create backend
    auto capture = create_capture_backend();
    printf("Backend: %s\n", capture->name());

    // Get window rect
    Rect region = {};
    if (window_title) {
        if (capture->get_window_rect(window_title, region)) {
            printf("Window: x=%d y=%d w=%d h=%d\n", region.x, region.y, region.w, region.h);
        } else {
            printf("Window not found, using full screen\n");
        }
    }

    // Preprocessor
    FramePreprocessor preproc;

    // Capture 10 frames and measure latency
    FrameBuffer buf;
    float tensor[4 * 84 * 84] = {};
    double total_capture = 0;
    double total_preprocess = 0;

    printf("\nCapturing 10 frames...\n");
    for (int i = 0; i < 10; i++) {
        auto t0 = capture_now_us();

        if (!capture->capture(buf, window_title ? &region : nullptr)) {
            printf("Capture failed at frame %d\n", i);
            continue;
        }
        auto t1 = capture_now_us();

        preproc.process(buf, tensor);
        auto t2 = capture_now_us();

        double cap_ms = (t1 - t0) / 1000.0;
        double pre_ms = (t2 - t1) / 1000.0;
        total_capture += cap_ms;
        total_preprocess += pre_ms;

        printf("  Frame %d: capture=%.2fms  preprocess=%.2fms  size=%dx%d\n",
               i, cap_ms, pre_ms, buf.width, buf.height);

        Sleep(100);  // 100ms between captures
    }

    printf("\nAverages over 10 frames:\n");
    printf("  Capture:     %.2fms\n", total_capture / 10.0);
    printf("  Preprocess:  %.2fms\n", total_preprocess / 10.0);
    printf("  Total:       %.2fms\n", (total_capture + total_preprocess) / 10.0);

    capture->shutdown();
    return 0;
}
