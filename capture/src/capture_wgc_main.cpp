/**
 * capture_wgc_main.cpp — standalone WGC capture CLI
 *
 * Usage: capture_wgc.exe <hwnd> [--single|--stream] [--scale N]
 *   --single : capture one frame, output to stdout, exit
 *   --stream : continuous stream until stdin 'q'
 *   --scale N: downscale max dimension to N pixels (default: no scale)
 *
 * Binary output (stdout, LE):
 *   [timestamp_us:8][w:4][h:4][ch:4][reserved:4][pixels: w*h*ch]
 *
 * Stderr: timing + status info
 */
#include "../include/capture_wgc.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <io.h>
#include <fcntl.h>
#include <thread>
#include <atomic>
#include <vector>
#include <ctime>
#include <cstdarg>

// ── File logger ────────────────────────────────────────
static FILE* g_log = nullptr;

static void log_init() {
    time_t now = time(nullptr);
    struct tm tm;
    localtime_s(&tm, &now);
    char path[512];
    // Write to project_root/log/ (exe is in capture/build/, so ../../log/)
    snprintf(path, sizeof(path), "../../log/wgc_%04d%02d%02d_%02d%02d%02d.log",
        tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
        tm.tm_hour, tm.tm_min, tm.tm_sec);
    g_log = fopen(path, "w");
    if (!g_log) {
        // Fallback: same directory as exe
        snprintf(path, sizeof(path), "wgc_%04d%02d%02d_%02d%02d%02d.log",
            tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
            tm.tm_hour, tm.tm_min, tm.tm_sec);
        g_log = fopen(path, "w");
    }
}

static void wgc_log(const char* fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    // Timestamp prefix
    uint64_t us = wgc::now_us();
    uint64_t sec = us / 1'000'000;
    uint64_t ms = (us / 1000) % 1000;
    uint64_t rem_us = us % 1000;
    fprintf(stderr, "[wgc] %llu.%03llu.%03llu %s\n",  // raw stderr for log reader
        (unsigned long long)sec, (unsigned long long)ms, (unsigned long long)rem_us, buf);
    fflush(stderr);

    if (g_log) {
        fprintf(g_log, "[%llu.%03llu.%03llu] %s\n",
            (unsigned long long)sec, (unsigned long long)ms, (unsigned long long)rem_us, buf);
        fflush(g_log);
    }
}

static void log_close() {
    if (g_log) { fclose(g_log); g_log = nullptr; }
}

static void write_u32(uint32_t v) { fwrite(&v, 4, 1, stdout); }
static void write_u64(uint64_t v) { fwrite(&v, 8, 1, stdout); }

static void scale_bgra(const uint8_t* src, int sw, int sh,
                       std::vector<uint8_t>& dst, int& dw, int& dh,
                       int max_dim) {
    float s = (float)max_dim / (float)(sw > sh ? sw : sh);
    if (s >= 1.0f) {
        dw = sw; dh = sh;
        dst.assign(src, src + sw * sh * 4);
        return;
    }
    dw = (int)(sw * s); dh = (int)(sh * s);
    dst.resize(dw * dh * 4);
    for (int y = 0; y < dh; y++) {
        int sy = (int)(y / s);
        for (int x = 0; x < dw; x++) {
            int sx = (int)(x / s);
            memcpy(dst.data() + (y * dw + x) * 4,
                   src + (sy * sw + sx) * 4, 4);
        }
    }
}

int main(int argc, char* argv[]) {
    log_init();
    winrt::init_apartment(winrt::apartment_type::multi_threaded);

    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stdin), _O_BINARY);
    setvbuf(stderr, NULL, _IONBF, 0);

    if (argc < 2) {
        wgc_log("Usage: capture_wgc.exe <hwnd> [--single|--stream] [--scale N]");
        return 1;
    }

    HWND hwnd = (HWND)(ULONG_PTR)_strtoui64(argv[1], nullptr, 10);
    bool single = false, stream = false;
    int max_dim = 0;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--single") == 0) single = true;
        else if (strcmp(argv[i], "--stream") == 0) stream = true;
        else if (strcmp(argv[i], "--scale") == 0 && i + 1 < argc)
            max_dim = atoi(argv[++i]);
    }
    if (!single && !stream) single = true;

    wgc::WgcCapture cap;
    if (!cap.init(hwnd)) {
        wgc_log("init failed: %s", cap.last_error());
        return 1;
    }

    if (single) {
        wgc::WgcFrame frame;
        wgc::WgcTiming timing;
        // FramePool needs time to produce first frame — retry up to 500ms
        bool got = false;
        for (int retry = 0; retry < 50; retry++) {
            if (cap.capture(frame, &timing)) { got = true; break; }
            Sleep(10);
        }
        if (got) {
            wgc_log("single: %dx%d cap=%lluus copy=%lluus readback=%lluus total=%lluus",
                frame.width, frame.height,
                timing.cap_us, timing.copy_us, timing.readback_us, timing.total_us);

            std::vector<uint8_t> scaled;
            int sw = frame.width, sh = frame.height;
            if (max_dim > 0)
                scale_bgra(frame.pixels.data(), frame.width, frame.height, scaled, sw, sh, max_dim);

            write_u64(frame.timestamp_us);
            write_u32((uint32_t)sw);
            write_u32((uint32_t)sh);
            write_u32(4);
            write_u32(0);  // reserved
            fwrite(max_dim > 0 ? scaled.data() : frame.pixels.data(), 1, sw * sh * 4, stdout);
            fflush(stdout);
            return 0;
        }
        wgc_log("single: no frame captured");
        return 1;
    }

    // Stream mode — warm up: force first frame quickly
    wgc_log("warming up...");
    {
        wgc::WgcFrame warm;
        bool got_warm = false;
        for (int retry = 0; retry < 100; retry++) {
            if (cap.capture(warm, nullptr)) { got_warm = true; break; }
            Sleep(10);
        }
        if (got_warm) {
            wgc_log("warm-up OK: %dx%d", warm.width, warm.height);
            // Write first frame immediately
            std::vector<uint8_t> scaled;
            int sw = warm.width, sh = warm.height;
            if (max_dim > 0)
                scale_bgra(warm.pixels.data(), warm.width, warm.height, scaled, sw, sh, max_dim);
            write_u64(warm.timestamp_us);
            write_u32((uint32_t)sw); write_u32((uint32_t)sh);
            write_u32(4); write_u32(0);
            fwrite(max_dim > 0 ? scaled.data() : warm.pixels.data(), 1, sw * sh * 4, stdout);
            fflush(stdout);
        } else {
            wgc_log("warm-up: no frame after 1s, continuing anyway");
        }
    }

    wgc_log("stream started, Ctrl+C to stop");

    std::atomic<bool> running{true};
    // Only watch stdin if it's a real terminal (not /dev/null)
    if (_isatty(_fileno(stdin))) {
        std::thread stdin_thread([&]() {
            char c;
            while (running && fread(&c, 1, 1, stdin) > 0) {
                if (c == 'q') { running = false; break; }
            }
        });
        stdin_thread.detach();
    }

    int frames = 0;
    uint64_t total_cap = 0, total_copy = 0, total_readback = 0;
    LARGE_INTEGER freq, t_start;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t_start);

    while (running) {
        wgc::WgcFrame frame;
        wgc::WgcTiming timing;
        if (!cap.capture(frame, &timing)) { Sleep(1); continue; }

        total_cap += timing.cap_us; total_copy += timing.copy_us;
        total_readback += timing.readback_us; frames++;

        std::vector<uint8_t> scaled;
        int sw = frame.width, sh = frame.height;
        if (max_dim > 0)
            scale_bgra(frame.pixels.data(), frame.width, frame.height, scaled, sw, sh, max_dim);

        write_u64(frame.timestamp_us);
        write_u32((uint32_t)sw); write_u32((uint32_t)sh);
        write_u32(4); write_u32(0);
        fwrite(max_dim > 0 ? scaled.data() : frame.pixels.data(), 1, sw * sh * 4, stdout);
        fflush(stdout);

        if (frames % 60 == 0) {
            LARGE_INTEGER t_now;
            QueryPerformanceCounter(&t_now);
            double elapsed = (double)(t_now.QuadPart - t_start.QuadPart) / freq.QuadPart;
            wgc_log("%d frames in %.2fs = %.1f FPS | "
                "avg cap=%.0fus copy=%.0fus readback=%.0fus total=%.0fus",
                frames, elapsed, frames / elapsed,
                (double)total_cap / frames, (double)total_copy / frames,
                (double)total_readback / frames,
                (double)(total_cap + total_copy + total_readback) / frames);
        }
    }

    cap.shutdown();
    wgc_log("stream exit: %d frames total", frames);
    log_close();
    return 0;
}
