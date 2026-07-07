/**
 * capture_wgc_ffi.cpp — FFI implementation for WgcCapture.
 *
 * Manages background capture thread with DispatcherQueue (required for
 * FrameArrived event delivery). Uses condition_variable for efficient
 * frame waiting instead of busy-polling.
 *
 * Frame scaling: if max_dim > 0, frames are downscaled while preserving
 * aspect ratio (bilinear-like integer sampling).
 */
#include "../include/capture_wgc.hpp"
#include "../include/capture_wgc_ffi.h"
#include "../../logger/logger.h"
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <cstring>
#include <algorithm>

// ── Simple integer-ratio downscale (preserves aspect ratio) ──
static void scale_bgra_keep_aspect(const uint8_t* src, int sw, int sh,
                                    uint8_t* dst, int dw, int dh) {
    for (int dy = 0; dy < dh; dy++) {
        int sy = dy * sh / dh;
        for (int dx = 0; dx < dw; dx++) {
            int sx = dx * sw / dw;
            int si = (sy * sw + sx) * 4;
            int di = (dy * dw + dx) * 4;
            dst[di]     = src[si];
            dst[di + 1] = src[si + 1];
            dst[di + 2] = src[si + 2];
            dst[di + 3] = src[si + 3];
        }
    }
}

struct WgcStreamHandle {
    wgc::WgcCapture cap;
    std::thread worker;
    winrt::Windows::System::DispatcherQueueController dispatcher_ctrl_{nullptr};
    std::atomic<bool> running{false};
    std::atomic<bool> has_frame{false};
    std::atomic<bool> init_ok{false};

    // Init synchronization: start() blocks until worker signals init done
    std::mutex init_mtx;
    std::condition_variable init_cv;
    bool init_done = false;
    const char* init_error = nullptr;

    // Mutex-protected latest frame
    std::mutex mtx;
    std::vector<uint8_t> frame_buf;
    int frame_w = 0, frame_h = 0, frame_ch = 4;
    int max_dim = 0; // 0 = no scaling

    bool start(HWND hwnd, int _max_dim) {
        max_dim = _max_dim;
        worker = std::thread([this, hwnd]() {
            // CRITICAL: create DispatcherQueue BEFORE init.
            // FrameArrived event won't fire without an active DispatcherQueue.
            dispatcher_ctrl_ = wgc::create_dispatcher_queue();

            if (!cap.init(hwnd)) {
                {
                    std::lock_guard<std::mutex> lk(init_mtx);
                    init_error = cap.last_error();
                    init_done = true;
                }
                init_cv.notify_one();
                dispatcher_ctrl_.ShutdownQueueAsync(); // fire-and-forget — avoid STA deadlock on own thread
                dispatcher_ctrl_ = nullptr;
                return;
            }

            init_ok = true;
            {
                std::lock_guard<std::mutex> lk(init_mtx);
                init_done = true;
            }
            init_cv.notify_one();

            running = true;
            wgc::WgcFrame frame;
            int dbg = 0;
            while (running) {
                // Wait up to 100ms for a frame (condition variable, not busy-wait)
                if (!cap.wait_frame(frame, 100)) {
                    if (!running) break;
                    continue;
                }
                dbg++;
                if (dbg % 30 == 0) {
                    LOG("wgc-worker", "%d frames, ok=%d", dbg, cap.is_ok() ? 1 : 0);
                }

                // Scale if max_dim is set and frame is larger
                if (max_dim > 0 && (frame.width > max_dim || frame.height > max_dim)) {
                    float scale = (float)max_dim / (float)(std::max(frame.width, frame.height));
                    int dw = (int)(frame.width * scale);
                    int dh = (int)(frame.height * scale);
                    if (dw < 1) dw = 1;
                    if (dh < 1) dh = 1;

                    std::vector<uint8_t> scaled(dw * dh * 4);
                    scale_bgra_keep_aspect(frame.pixels.data(), frame.width, frame.height,
                                            scaled.data(), dw, dh);

                    std::lock_guard<std::mutex> lk(mtx);
                    frame_buf = std::move(scaled);
                    frame_w = dw;
                    frame_h = dh;
                    frame_ch = frame.channels;
                    has_frame = true;
                } else {
                    std::lock_guard<std::mutex> lk(mtx);
                    frame_buf = std::move(frame.pixels);
                    frame_w = frame.width;
                    frame_h = frame.height;
                    frame_ch = frame.channels;
                    has_frame = true;
                }
            }

            // Worker exits cleanly — DispatcherQueue shutdown is handled
            // by stop() AFTER worker.join(), to avoid use-after-free crash.
        });

        // Wait for init to complete (max 5 seconds)
        {
            std::unique_lock<std::mutex> lk(init_mtx);
            if (!init_cv.wait_for(lk, std::chrono::seconds(5), [this] { return init_done; })) {
                running = false;
                if (worker.joinable()) worker.join();
                return false;
            }
        }
        if (!init_ok) {
            running = false;
            if (worker.joinable()) worker.join();
            return false;
        }
        return true;
    }

    void stop() {
        running = false;
        cap.signal_stop();
        // Don't join — the worker checks `running` and exits on its own.
        // Joining can hang if the worker is blocked in a WinRT callback.
        // The thread resources are cleaned up when the process exits.
        if (worker.joinable()) {
            worker.detach();
        }
        // Don't shutdown cap here — worker may still be using it.
        // Don't shutdown dispatcher — worker owns it.
    }
};

extern "C" {

// ── WinRT apartment lifecycle (call once at process level) ──

void wgc_init_apartment(void) {
    wgc::init_apartment();
}

void wgc_deinit_apartment(void) {
    wgc::uninit_apartment();
}

// ── Stream API ──

WgcStreamHandle* wgc_stream_start(HWND hwnd, int max_dim) {
    auto* h = new WgcStreamHandle();
    if (!h->start(hwnd, max_dim)) {
        delete h;
        return nullptr;
    }
    return h;
}

int wgc_stream_read(WgcStreamHandle* h, uint8_t* buf, int buf_size,
                    int* out_w, int* out_h, int* out_ch) {
    if (!h || !h->has_frame.load(std::memory_order_acquire)) return 0;
    std::lock_guard<std::mutex> lk(h->mtx);
    if (h->frame_buf.empty()) return 0;
    int needed = (int)h->frame_buf.size();
    if (buf_size < needed) return 0; // buffer too small
    memcpy(buf, h->frame_buf.data(), needed);
    *out_w = h->frame_w;
    *out_h = h->frame_h;
    *out_ch = h->frame_ch;
    h->has_frame.store(false, std::memory_order_release);
    return needed;
}

int wgc_stream_is_ok(WgcStreamHandle* h) {
    if (!h) return 0;
    return h->init_ok.load() && h->running.load() && h->cap.is_ok() ? 1 : 0;
}

void wgc_stream_stop(WgcStreamHandle* h) {
    if (!h) return;
    h->stop();
    delete h;
}

void wgc_stream_signal_stop(WgcStreamHandle* h) {
    if (!h) return;
    h->running = false;
    h->cap.signal_stop();
}

// ── Single-frame capture ──

int wgc_capture_single(HWND hwnd, uint8_t* buf, int buf_size,
                       int* out_w, int* out_h, int* out_ch) {
    // CRITICAL: create DispatcherQueue BEFORE init — FrameArrived event
    // won't fire without an active DispatcherQueue on this thread.
    auto dq = wgc::create_dispatcher_queue();

    wgc::WgcCapture cap;
    if (!cap.init(hwnd)) { dq.ShutdownQueueAsync(); /* fire-and-forget */; return 0; }

    wgc::WgcFrame frame;
    // Wait up to ~500ms for first frame with condition variable
    for (int i = 0; i < 50; i++) {
        if (cap.wait_frame(frame, 10)) {
            int needed = (int)frame.pixels.size();
            if (buf_size < needed) {
                cap.shutdown();
                dq.ShutdownQueueAsync(); /* fire-and-forget */;
                return 0;
            }
            memcpy(buf, frame.pixels.data(), needed);
            *out_w = frame.width;
            *out_h = frame.height;
            *out_ch = frame.channels;
            cap.shutdown();
            dq.ShutdownQueueAsync(); /* fire-and-forget */;
            return needed;
        }
        if (!cap.is_ok()) break;
    }
    cap.shutdown();
    dq.ShutdownQueueAsync(); /* fire-and-forget */;
    return 0;
}

// ── Monitor capture (desktop) ──

WgcStreamHandle* wgc_stream_start_monitor(HMONITOR hmon, int max_dim) {
    auto* h = new WgcStreamHandle();
    h->max_dim = max_dim;
    h->worker = std::thread([h, hmon]() {
        h->dispatcher_ctrl_ = wgc::create_dispatcher_queue();

        if (!h->cap.init_monitor(hmon)) {
            {
                std::lock_guard<std::mutex> lk(h->init_mtx);
                h->init_error = h->cap.last_error();
                h->init_done = true;
            }
            h->init_cv.notify_one();
            if (h->dispatcher_ctrl_) {
                h->dispatcher_ctrl_.ShutdownQueueAsync(); // fire-and-forget — avoid STA deadlock on own thread
                h->dispatcher_ctrl_ = nullptr;
            }
            return;
        }

        h->init_ok = true;
        {
            std::lock_guard<std::mutex> lk(h->init_mtx);
            h->init_done = true;
        }
        h->init_cv.notify_one();

        h->running = true;
        wgc::WgcFrame frame;
        while (h->running) {
            if (!h->cap.wait_frame(frame, 100)) {
                if (!h->running) break;
                continue;
            }

            if (h->max_dim > 0 && (frame.width > h->max_dim || frame.height > h->max_dim)) {
                float scale = (float)h->max_dim / (float)(std::max(frame.width, frame.height));
                int dw = (int)(frame.width * scale);
                int dh = (int)(frame.height * scale);
                if (dw < 1) dw = 1;
                if (dh < 1) dh = 1;

                std::vector<uint8_t> scaled(dw * dh * 4);
                scale_bgra_keep_aspect(frame.pixels.data(), frame.width, frame.height,
                                        scaled.data(), dw, dh);

                std::lock_guard<std::mutex> lk(h->mtx);
                h->frame_buf = std::move(scaled);
                h->frame_w = dw;
                h->frame_h = dh;
                h->frame_ch = frame.channels;
                h->has_frame = true;
            } else {
                std::lock_guard<std::mutex> lk(h->mtx);
                h->frame_buf = std::move(frame.pixels);
                h->frame_w = frame.width;
                h->frame_h = frame.height;
                h->frame_ch = frame.channels;
                h->has_frame = true;
            }
        }

        // DispatcherQueue shutdown handled by stop() after worker.join()
    });

    // Wait for init (max 5 seconds)
    {
        std::unique_lock<std::mutex> lk(h->init_mtx);
        if (!h->init_cv.wait_for(lk, std::chrono::seconds(5), [h] { return h->init_done; })) {
            h->running = false;
            if (h->worker.joinable()) h->worker.detach(); // detach: worker may be stuck in WinRT
            delete h;
            return nullptr;
        }
    }
    if (!h->init_ok) {
        h->running = false;
        if (h->worker.joinable()) h->worker.detach(); // detach: worker may be stuck in WinRT
        delete h;
        return nullptr;
    }
    return h;
}

int wgc_capture_single_monitor(HMONITOR hmon, uint8_t* buf, int buf_size,
                               int* out_w, int* out_h, int* out_ch) {
    auto dq = wgc::create_dispatcher_queue();

    wgc::WgcCapture cap;
    if (!cap.init_monitor(hmon)) { dq.ShutdownQueueAsync(); /* fire-and-forget */; return 0; }

    wgc::WgcFrame frame;
    for (int i = 0; i < 50; i++) {
        if (cap.wait_frame(frame, 10)) {
            int needed = (int)frame.pixels.size();
            if (buf_size < needed) {
                cap.shutdown();
                dq.ShutdownQueueAsync(); /* fire-and-forget */;
                return 0;
            }
            memcpy(buf, frame.pixels.data(), needed);
            *out_w = frame.width;
            *out_h = frame.height;
            *out_ch = frame.channels;
            cap.shutdown();
            dq.ShutdownQueueAsync(); /* fire-and-forget */;
            return needed;
        }
        if (!cap.is_ok()) break;
    }
    cap.shutdown();
    dq.ShutdownQueueAsync(); /* fire-and-forget */;
    return 0;
}

} // extern "C"
