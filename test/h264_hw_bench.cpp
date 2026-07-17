/**
 * h264_hw_bench — Standalone WGC → GPU H.264 → TCP proof.
 *
 * Captures primary monitor, hardware-encodes 1080p (or native), sends Annex-B
 * frames to 127.0.0.1:19999. Pair with: node test/h264_recv_bench.mjs
 *
 * Exit 0 if hardware encode sustained >= 25 fps over the run window.
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <d3d11.h>

#include "../logger/logger.h"
#include "../capture/include/capture_wgc_ffi.h"
#include "../mimic_client/src/h264_encoder.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "d3d11.lib")

static H264Encoder g_enc;
static std::mutex g_enc_mtx;
static SOCKET g_client = INVALID_SOCKET;
static std::mutex g_sock_mtx;
static std::atomic<int> g_encoded{0};
static std::atomic<int> g_sent{0};
static std::atomic<int> g_hw{0};
static std::atomic<long long> g_enc_us_sum{0};
static std::atomic<int> g_enc_us_n{0};
static std::atomic<bool> g_running{true};

static bool tcp_send_all(SOCKET s, const char* p, int n) {
    while (n > 0) {
        int r = send(s, p, n, 0);
        if (r <= 0) return false;
        p += r; n -= r;
    }
    return true;
}

static void on_gpu_frame(void* /*ctx*/, void* d3d_device, void* d3d_tex, int w, int h) {
    if (!g_running.load() || !d3d_device || !d3d_tex || w < 16 || h < 16) return;

    auto* dev = (ID3D11Device*)d3d_device;
    auto* tex = (ID3D11Texture2D*)d3d_tex;

    LARGE_INTEGER t0, t1, freq;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);

    std::lock_guard<std::mutex> lk(g_enc_mtx);
    if (!g_enc.ready()) {
        int ew = w & ~1, eh = h & ~1;
        if (ew > 1920) {
            ew = 1920;
            eh = ((int)((int64_t)h * ew / w)) & ~1;
        }
        if (!g_enc.init(dev, ew, eh, 30, 6000)) {
            LOG_ERROR("bench", "H264 init failed");
            g_running = false;
            return;
        }
        g_hw.store(g_enc.hardware() ? 1 : 0);
        LOG("bench", "encoder ready hardware=%d %dx%d", g_enc.hardware() ? 1 : 0, ew, eh);
        if (!g_enc.hardware()) {
            LOG_ERROR("bench", "HARDWARE required for this bench — aborting soft path");
            g_running = false;
            return;
        }
    }

    std::vector<H264Packet> pkts;
    int ew = w & ~1, eh = h & ~1;
    if (ew != g_enc.width() || eh != g_enc.height()) {
        // Bench keeps native size; skip mismatched frames.
        return;
    }
    if (!g_enc.encode_texture(tex, ew, eh, pkts) || pkts.empty()) return;

    QueryPerformanceCounter(&t1);
    long long us = (t1.QuadPart - t0.QuadPart) * 1000000LL / freq.QuadPart;
    g_enc_us_sum.fetch_add(us);
    g_enc_us_n.fetch_add(1);
    g_encoded.fetch_add((int)pkts.size());

    SOCKET s;
    {
        std::lock_guard<std::mutex> slk(g_sock_mtx);
        s = g_client;
    }
    if (s == INVALID_SOCKET) return;

    for (const auto& pkt : pkts) {
        uint32_t flags = pkt.keyframe ? 1u : 0u;
        uint32_t meta[5] = {
            (uint32_t)pkt.w, (uint32_t)pkt.h, flags, pkt.ts_ms, (uint32_t)pkt.annexb.size()
        };
        if (!tcp_send_all(s, (const char*)meta, 20) ||
            (!pkt.annexb.empty() &&
             !tcp_send_all(s, (const char*)pkt.annexb.data(), (int)pkt.annexb.size()))) {
            std::lock_guard<std::mutex> slk(g_sock_mtx);
            if (g_client == s) {
                closesocket(g_client);
                g_client = INVALID_SOCKET;
            }
            return;
        }
        g_sent.fetch_add(1);
    }
}

int main() {
    capture_log_init("h264_hw_bench", "0.0.0", "log", 3, 2000);
    capture_log_set_level(LOG_LEVEL_DEBUG);

    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        LOG_ERROR("bench", "WSAStartup failed");
        return 1;
    }

    SOCKET listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(19999);
    BOOL yes = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, (char*)&yes, sizeof(yes));
    if (bind(listen_sock, (sockaddr*)&addr, sizeof(addr)) != 0 ||
        listen(listen_sock, 1) != 0) {
        LOG_ERROR("bench", "bind/listen :19999 failed");
        return 1;
    }
    LOG("bench", "listening TCP 127.0.0.1:19999 — waiting for receiver (15s)...");
    // Accept BEFORE capture so the first encoded frames are not dropped.
    {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(listen_sock, &rfds);
        timeval tv{};
        tv.tv_sec = 15;
        int sel = select(0, &rfds, nullptr, nullptr, &tv);
        if (sel > 0) {
            SOCKET c = accept(listen_sock, nullptr, nullptr);
            if (c != INVALID_SOCKET) {
                int flag = 1;
                setsockopt(c, IPPROTO_TCP, TCP_NODELAY, (char*)&flag, sizeof(flag));
                g_client = c;
                LOG("bench", "receiver connected");
            }
        } else {
            LOG_WARN("bench", "no receiver within 15s — encode-only mode");
        }
    }

    wgc_init_apartment();
    HMONITOR mon = MonitorFromWindow(nullptr, MONITOR_DEFAULTTOPRIMARY);
    WgcStreamHandle* stream = wgc_stream_start_monitor(mon, 0);
    if (!stream) {
        LOG_ERROR("bench", "wgc_stream_start_monitor failed");
        g_running = false;
    } else {
        wgc_stream_set_cpu_readback(stream, 0);
        wgc_stream_set_gpu_frame_callback(stream, on_gpu_frame, nullptr);
        LOG("bench", "WGC GPU stream started — encoding 8s");
    }

    const int run_ms = 8000;
    auto t_start = std::chrono::steady_clock::now();
    while (g_running.load()) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t_start).count();
        if (elapsed >= run_ms) break;
        Sleep(50);
    }
    g_running = false;

    if (stream) wgc_stream_stop(stream);
    wgc_deinit_apartment();

    {
        std::lock_guard<std::mutex> lk(g_sock_mtx);
        if (g_client != INVALID_SOCKET) { closesocket(g_client); g_client = INVALID_SOCKET; }
    }
    closesocket(listen_sock);
    WSACleanup();

    int enc = g_encoded.load();
    int sent = g_sent.load();
    int n = g_enc_us_n.load();
    double fps = enc / (run_ms / 1000.0);
    double avg_ms = n > 0 ? (g_enc_us_sum.load() / (double)n) / 1000.0 : 0;
    LOG("bench", "RESULT hardware=%d encoded=%d sent=%d fps=%.1f avg_encode=%.2fms",
        g_hw.load(), enc, sent, fps, avg_ms);
    std::printf("RESULT hardware=%d encoded=%d sent=%d fps=%.1f avg_encode=%.2fms\n",
                g_hw.load(), enc, sent, fps, avg_ms);

    g_enc.shutdown();
    capture_log_shutdown();

    if (!g_hw.load()) return 2;
    if (fps < 25.0) return 3;
    return 0;
}
