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
#include <wrl/client.h>
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
    std::atomic<bool> cpu_readback{true};
    WgcGpuFrameCb gpu_cb = nullptr;
    void* gpu_cb_ctx = nullptr;
    std::mutex cb_mtx;

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

    void run_loop() {
        running = true;
        wgc::WgcFrame frame;
        int dbg = 0;
        while (running) {
            const bool want_cpu = cpu_readback.load(std::memory_order_acquire);
            WgcGpuFrameCb cb = nullptr;
            void* ctx = nullptr;
            {
                std::lock_guard<std::mutex> lk(cb_mtx);
                cb = gpu_cb;
                ctx = gpu_cb_ctx;
            }

            if (!want_cpu && cb) {
                Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
                int tw = 0, th = 0;
                if (!cap.wait_frame_gpu(tex, tw, th, 100)) {
                    if (!running) break;
                    continue;
                }
                dbg++;
                if (dbg % 30 == 0)
                    LOG("wgc-worker", "%d gpu-frames %dx%d", dbg, tw, th);
                // GPU encode path: no Map. max_dim scaling skipped (encoder may scale).
                cb(ctx, (void*)cap.d3d_device(), (void*)tex.Get(), tw, th);
                continue;
            }

            if (!cap.wait_frame(frame, 100)) {
                if (!running) break;
                continue;
            }
            dbg++;
            if (dbg % 30 == 0)
                LOG("wgc-worker", "%d frames, ok=%d", dbg, cap.is_ok() ? 1 : 0);

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
    }

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

            run_loop();

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
        // signal_stop() wakes wait_frame() → worker checks !running → breaks loop → exits.
        // join() is safe because FrameArrived callback is brief (flag+notify, no blocking).
        // Previously used detach() which leaked DispatcherQueue + WGC session,
        // causing crash when next capture targets same window.
        if (worker.joinable()) {
            worker.join();
        }
        // Worker has exited — safe to tear down DispatcherQueue
        if (dispatcher_ctrl_) {
            dispatcher_ctrl_.ShutdownQueueAsync();
            dispatcher_ctrl_ = nullptr;
        }
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
    if (out_ch) *out_ch = h->frame_ch;
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
    LOG("wgc", "wgc_capture_single: entry hwnd=%llu buf=%p buf_size=%d",
        (unsigned long long)(uintptr_t)hwnd, (void*)buf, buf_size);

    // CRITICAL: create DispatcherQueue BEFORE init — FrameArrived event
    // won't fire without an active DispatcherQueue on this thread.
    LOG("wgc", "wgc_capture_single: creating DispatcherQueue...");
    auto dq = wgc::create_dispatcher_queue();
    LOG("wgc", "wgc_capture_single: DispatcherQueue created");

    wgc::WgcCapture cap;
    LOG("wgc", "wgc_capture_single: calling cap.init(hwnd=%llu)...",
        (unsigned long long)(uintptr_t)hwnd);
    if (!cap.init(hwnd)) {
        LOG("wgc", "wgc_capture_single: cap.init FAILED — %s", cap.last_error());
        dq.ShutdownQueueAsync();
        return 0;
    }
    LOG("wgc", "wgc_capture_single: cap.init OK");

    wgc::WgcFrame frame;
    // Wait up to ~500ms for first frame with condition variable
    for (int i = 0; i < 50; i++) {
        if (cap.wait_frame(frame, 10)) {
            LOG("wgc", "wgc_capture_single: got frame on attempt %d — %dx%d %zu bytes",
                i, frame.width, frame.height, frame.pixels.size());
            int needed = (int)frame.pixels.size();
            LOG("wgc", "wgc_capture_single: needed=%d buf_size=%d", needed, buf_size);
            if (buf_size < needed) {
                LOG("wgc", "wgc_capture_single: buf too small! need=%d have=%d", needed, buf_size);
                cap.shutdown();
                dq.ShutdownQueueAsync();
                return 0;
            }
            LOG("wgc", "wgc_capture_single: buf_size OK, about to memcpy...");
            memcpy(buf, frame.pixels.data(), needed);
            LOG("wgc", "wgc_capture_single: memcpy OK, writing out params...");
            *out_w = frame.width;
            *out_h = frame.height;
            if (out_ch) *out_ch = frame.channels;
            LOG("wgc", "wgc_capture_single: calling cap.shutdown()...");
            cap.shutdown();
            LOG("wgc", "wgc_capture_single: cap.shutdown() OK, calling dq.ShutdownQueueAsync()...");
            dq.ShutdownQueueAsync();
            LOG("wgc", "wgc_capture_single: SUCCESS %dx%d ch=%d size=%d",
                *out_w, *out_h, out_ch ? *out_ch : 0, needed);
            return needed;
        }
        if (!cap.is_ok()) {
            LOG("wgc", "wgc_capture_single: cap not OK on attempt %d — %s",
                i, cap.last_error());
            break;
        }
    }
    LOG("wgc", "wgc_capture_single: timeout or error — no frame after %d attempts", 50);
    cap.shutdown();
    dq.ShutdownQueueAsync();
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

        h->run_loop();
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
    LOG("wgc", "wgc_capture_single_monitor: entry hmon=%p buf=%p buf_size=%d",
        (void*)hmon, (void*)buf, buf_size);

    LOG("wgc", "wgc_capture_single_monitor: creating DispatcherQueue...");
    auto dq = wgc::create_dispatcher_queue();
    LOG("wgc", "wgc_capture_single_monitor: DispatcherQueue created");

    wgc::WgcCapture cap;
    LOG("wgc", "wgc_capture_single_monitor: calling cap.init_monitor(hmon=%p)...", (void*)hmon);
    if (!cap.init_monitor(hmon)) {
        LOG("wgc", "wgc_capture_single_monitor: cap.init_monitor FAILED — %s", cap.last_error());
        dq.ShutdownQueueAsync();
        return 0;
    }
    LOG("wgc", "wgc_capture_single_monitor: cap.init_monitor OK, entering wait loop");

    wgc::WgcFrame frame;
    for (int i = 0; i < 50; i++) {
        LOG("wgc", "wgc_capture_single_monitor: wait_frame attempt %d/50...", i);
        if (cap.wait_frame(frame, 10)) {
            LOG("wgc", "wgc_capture_single_monitor: got frame on attempt %d — %dx%d %zu bytes",
                i, frame.width, frame.height, frame.pixels.size());
            int needed = (int)frame.pixels.size();
            LOG("wgc", "wgc_capture_single_monitor: needed=%d buf_size=%d", needed, buf_size);
            if (buf_size < needed) {
                LOG("wgc", "wgc_capture_single_monitor: buf too small! need=%d have=%d", needed, buf_size);
                cap.shutdown();
                dq.ShutdownQueueAsync();
                return 0;
            }
            LOG("wgc", "wgc_capture_single_monitor: buf_size OK, about to memcpy...");
            memcpy(buf, frame.pixels.data(), needed);
            LOG("wgc", "wgc_capture_single_monitor: memcpy OK, writing out params...");
            *out_w = frame.width;
            *out_h = frame.height;
            if (out_ch) *out_ch = frame.channels;
            LOG("wgc", "wgc_capture_single_monitor: calling cap.shutdown()...");
            cap.shutdown();
            LOG("wgc", "wgc_capture_single_monitor: cap.shutdown() OK, calling dq.ShutdownQueueAsync()...");
            dq.ShutdownQueueAsync();
            LOG("wgc", "wgc_capture_single_monitor: SUCCESS %dx%d ch=%d size=%d",
                *out_w, *out_h, out_ch ? *out_ch : 0, needed);
            return needed;
        }
        if (!cap.is_ok()) {
            LOG("wgc", "wgc_capture_single_monitor: cap not OK on attempt %d — %s",
                i, cap.last_error());
            break;
        }
    }
    LOG("wgc", "wgc_capture_single_monitor: timeout after 50 attempts, shutting down");
    cap.shutdown();
    dq.ShutdownQueueAsync();
    return 0;
}

void wgc_stream_set_gpu_frame_callback(WgcStreamHandle* h, WgcGpuFrameCb cb, void* ctx) {
    if (!h) return;
    std::lock_guard<std::mutex> lk(h->cb_mtx);
    h->gpu_cb = cb;
    h->gpu_cb_ctx = ctx;
}

void wgc_stream_set_cpu_readback(WgcStreamHandle* h, int enable) {
    if (!h) return;
    h->cpu_readback.store(enable != 0, std::memory_order_release);
    LOG("wgc", "cpu_readback=%d", enable != 0 ? 1 : 0);
}

int wgc_stream_pump(WgcStreamHandle* h) {
    // Worker thread owns the wait loop; pump is a no-op placeholder for API symmetry.
    (void)h;
    return 0;
}

} // extern "C"
