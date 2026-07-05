/**
 * wgc_bench_send.cpp — WGC capture → TCP send benchmark
 *
 * Full pipeline with per-stage microsecond timestamps:
 *   1. WGC FramePool capture (GPU)
 *   2. GPU→CPU readback via staging texture
 *   3. Pack into wire protocol (FRAM header + BGRA payload)
 *   4. TCP send to receiver
 *
 * Usage:
 *   wgc_bench_send.exe <hwnd> [--port N] [--scale N] [--no-wait]
 *     --port N    : TCP port (default 9999)
 *     --scale N   : downscale max dimension (default 1280)
 *     --no-wait   : don't sleep between frames (max FPS)
 *
 * Build:
 *   cl.exe /EHsc /std:c++17 /I ../capture/include /I ../common/include
 *         wgc_bench_send.cpp ../capture/src/capture_wgc.cpp
 *         d3d11.lib dxgi.lib windowsapp.lib user32.lib gdi32.lib ws2_32.lib
 *         /Fe:wgc_bench_send.exe
 *
 * Output (stderr): per-frame timing breakdown + periodic FPS summary
 */
#ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
#endif
#define NOMINMAX
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <atomic>
#include <thread>
#include <io.h>
#include <fcntl.h>
#include <intrin.h>  // _mm_pause()

#include "../capture/include/capture_wgc.hpp"
#include "../protocol/protocol.h"
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;

// ── TCP client sender ──────────────────────────────────
struct TcpClient {
    SOCKET sock_ = INVALID_SOCKET;
    bool connected_ = false;

    bool connect_to(const char* host, uint16_t port) {
        sock_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock_ == INVALID_SOCKET) return false;

        // Set TCP_NODELAY for low latency (disable Nagle)
        int nodelay = 1;
        setsockopt(sock_, IPPROTO_TCP, TCP_NODELAY, (char*)&nodelay, sizeof(nodelay));

        // Large send buffer
        int sndbuf = 8 * 1024 * 1024;  // 8MB
        setsockopt(sock_, SOL_SOCKET, SO_SNDBUF, (char*)&sndbuf, sizeof(sndbuf));

        sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        inet_pton(AF_INET, host, &addr.sin_addr);

        if (connect(sock_, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
            fprintf(stderr, "[tcp] connect failed: %d\n", WSAGetLastError());
            closesocket(sock_);
            sock_ = INVALID_SOCKET;
            return false;
        }

        connected_ = true;
        fprintf(stderr, "[tcp] connected to %s:%d\n", host, port);
        return true;
    }

    /// Send a frame: [magic:4][body_size:4][type_tag:4][body: body_size bytes]
    /// Matches Rust protocol::build_header + pipe::send_frame on the wire.
    bool send_frame(uint32_t type_tag, const void* body, uint32_t body_size) {
        uint8_t hdr[PROTOCOL_FRAME_HEADER];
        // protocol_build_header sets payload_size field = first arg.
        // Rust build_header(payload.len(), type_tag) passes body bytes only.
        // So we pass body_size here (NOT body_size+4).
        protocol_build_header(hdr, body_size, type_tag);

        // Send 12-byte header, then body
        if (::send(sock_, (char*)hdr, 12, 0) != 12) return false;
        if (body_size > 0) {
            if (::send(sock_, (char*)body, (int)body_size, 0) != (int)body_size) return false;
        }
        return true;
    }

    void disconnect() {
        if (sock_ != INVALID_SOCKET) {
            closesocket(sock_);
            sock_ = INVALID_SOCKET;
        }
        connected_ = false;
    }
};

// ── BGRA payload packer (matches protocol BGRA frame format) ──
static std::vector<uint8_t> pack_bgra_payload(const uint8_t* pixels,
    uint32_t w, uint32_t h, uint32_t ch) {
    // BGRA payload: [w:4][h:4][ch:4][reserved:4][pixels...]
    size_t total = 16 + (size_t)w * h * ch;
    std::vector<uint8_t> out(total);
    out[0] = (uint8_t)(w & 0xFF);  out[1] = (uint8_t)((w >> 8) & 0xFF);
    out[2] = (uint8_t)((w >> 16) & 0xFF); out[3] = (uint8_t)((w >> 24) & 0xFF);
    out[4] = (uint8_t)(h & 0xFF);  out[5] = (uint8_t)((h >> 8) & 0xFF);
    out[6] = (uint8_t)((h >> 16) & 0xFF); out[7] = (uint8_t)((h >> 24) & 0xFF);
    out[8] = (uint8_t)(ch & 0xFF); out[9] = (uint8_t)((ch >> 8) & 0xFF);
    out[10] = (uint8_t)((ch >> 16) & 0xFF); out[11] = (uint8_t)((ch >> 24) & 0xFF);
    out[12] = out[13] = out[14] = out[15] = 0;  // reserved
    memcpy(out.data() + 16, pixels, (size_t)w * h * ch);
    return out;
}

// ── Downscale ──────────────────────────────────────────
static void scale_bgra(const uint8_t* src, int sw, int sh,
                       std::vector<uint8_t>& dst, int& dw, int& dh,
                       int max_dim) {
    float s = (float)max_dim / (float)(sw > sh ? sw : sh);
    if (s >= 1.0f) {
        dw = sw; dh = sh;
        dst.assign(src, src + sw * sh * 4);
        return;
    }
    dw = (int)(sw * s);
    dh = (int)(sh * s);
    dst.resize(dw * dh * 4);
    for (int y = 0; y < dh; y++) {
        int sy = (int)(y / s);
        memcpy(dst.data() + y * dw * 4, src + sy * sw * 4, dw * 4);
    }
}

// ── DXGI Desktop Duplication (for hwnd=0, always 60+ FPS) ──
// GPU-accelerated full-desktop capture. Works at monitor refresh rate.
struct DxgiDesktopCap {
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    IDXGIOutputDuplication* dup = nullptr;
    ComPtr<ID3D11Texture2D> staging;
    int sw = 0, sh = 0;
    bool ok = false;

    bool init() {
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
            nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx))) return false;
        ComPtr<IDXGIDevice> d; dev.As(&d);
        ComPtr<IDXGIAdapter> a; d->GetAdapter(&a);
        for (UINT oi = 0; ; oi++) {
            ComPtr<IDXGIOutput> o;
            if (FAILED(a->EnumOutputs(oi, &o))) break;
            DXGI_OUTPUT_DESC odesc; o->GetDesc(&odesc);
            if (odesc.AttachedToDesktop && odesc.DesktopCoordinates.right - odesc.DesktopCoordinates.left >= 640) {
                ComPtr<IDXGIOutput1> o1;
                if (SUCCEEDED(o.As(&o1)) && SUCCEEDED(o1->DuplicateOutput(dev.Get(), &dup))) {
                    sw = odesc.DesktopCoordinates.right - odesc.DesktopCoordinates.left;
                    sh = odesc.DesktopCoordinates.bottom - odesc.DesktopCoordinates.top;
                    ok = true; return true;
                }
            }
        }
        return false;
    }

    bool capture(std::vector<uint8_t>& pixels, int& w, int& h) {
        if (!ok) return false;
        IDXGIResource* res = nullptr; DXGI_OUTDUPL_FRAME_INFO fi = {};
        // Use 0ms timeout for non-blocking polling (max throughput).
        // Desktop duplication produces frames at vblank (~60Hz).
        if (FAILED(dup->AcquireNextFrame(0, &fi, &res))) return false;
        ComPtr<ID3D11Texture2D> src;
        res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)src.GetAddressOf()); res->Release();
        D3D11_TEXTURE2D_DESC desc; src->GetDesc(&desc);
        if (!staging || (int)desc.Width != sw || (int)desc.Height != sh) {
            D3D11_TEXTURE2D_DESC sd = {};
            sd.Width = desc.Width; sd.Height = desc.Height; sd.MipLevels = 1;
            sd.ArraySize = 1; sd.Format = desc.Format; sd.SampleDesc.Count = 1;
            sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if (FAILED(dev->CreateTexture2D(&sd, nullptr, &staging))) { dup->ReleaseFrame(); return false; }
            sw = desc.Width; sh = desc.Height;
        }
        ctx->CopyResource(staging.Get(), src.Get()); src.Reset();
        dup->ReleaseFrame();
        D3D11_MAPPED_SUBRESOURCE m = {};
        if (FAILED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
        int pitch = (int)m.RowPitch; w = (int)desc.Width; h = (int)desc.Height;
        pixels.resize(w * h * 4);
        uint8_t* dst = pixels.data(); uint8_t* s = (uint8_t*)m.pData;
        if (pitch == w * 4) memcpy(dst, s, w * h * 4);
        else for (int y = 0; y < h; y++) memcpy(dst + y * w * 4, s + y * pitch, w * 4);
        ctx->Unmap(staging.Get(), 0);
        return true;
    }

    void shutdown() {
        if (dup) { dup->Release(); dup = nullptr; }
        staging.Reset(); ctx.Reset(); dev.Reset(); ok = false;
    }
};

// ── Timing accumulator ─────────────────────────────────
struct BenchStats {
    uint64_t cap_us = 0;
    uint64_t copy_us = 0;
    uint64_t readback_us = 0;
    uint64_t pack_us = 0;
    uint64_t send_us = 0;
    uint64_t total_us = 0;
    int frames = 0;
    int drops = 0;
};

// ── main ───────────────────────────────────────────────
int main(int argc, char* argv[]) {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);

    if (argc < 2) {
        fprintf(stderr, "Usage: wgc_bench_send.exe <hwnd> [--port N] [--scale N] [--no-wait]\n");
        fprintf(stderr, "  hwnd       : window handle (decimal), 0 = desktop\n");
        fprintf(stderr, "  --port N   : TCP port (default 9999)\n");
        fprintf(stderr, "  --scale N  : max dimension in pixels (default 1280, 0 = no scale)\n");
        fprintf(stderr, "  --no-wait  : no sleep between frames\n");
        return 1;
    }

    HWND hwnd = (HWND)(ULONG_PTR)_strtoui64(argv[1], nullptr, 10);
    uint16_t port = 9999;
    int max_dim = 1280;  // default: scale to 1280px for reasonable bandwidth
    bool no_wait = false;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = (uint16_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--scale") == 0 && i + 1 < argc) {
            max_dim = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--no-wait") == 0) {
            no_wait = true;
        }
    }

    bool is_desktop = (hwnd == NULL || hwnd == GetDesktopWindow());
    fprintf(stderr, "=== WGC Bench Send ===\n");
    fprintf(stderr, "  HWND: %p (%s)\n", hwnd, is_desktop ? "desktop" : "window");
    fprintf(stderr, "  Port: %d\n", port);
    fprintf(stderr, "  Scale: %d (0=off)\n", max_dim);
    fprintf(stderr, "  No-wait: %d\n", no_wait);

    // Init capture: DXGI for desktop, WGC for window
    wgc::WgcCapture wgc_cap;
    DxgiDesktopCap dxgi_cap;
    bool use_dxgi = is_desktop;

    if (use_dxgi) {
        if (!dxgi_cap.init()) {
            fprintf(stderr, "FATAL: DXGI init failed\n");
            return 1;
        }
        fprintf(stderr, "[dxgi] desktop capture %dx%d\n", dxgi_cap.sw, dxgi_cap.sh);
    } else {
        if (!wgc_cap.init(hwnd)) {
            fprintf(stderr, "FATAL: WGC init failed: %s\n", wgc_cap.last_error());
            return 1;
        }
    }

    // Init TCP
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    TcpClient tcp;
    int retries = 0;
    while (!tcp.connected_ && retries < 30) {
        if (tcp.connect_to("127.0.0.1", port)) break;
        fprintf(stderr, "[tcp] waiting for receiver... (retry %d)\n", ++retries);
        Sleep(500);
    }
    if (!tcp.connected_) {
        fprintf(stderr, "FATAL: could not connect to receiver\n");
        return 1;
    }

    // Pre-allocate re-usable buffers
    std::vector<uint8_t> scaled;
    BenchStats stats;
    LARGE_INTEGER freq, bench_t0;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&bench_t0);

    std::atomic<bool> running{true};
    fprintf(stderr, "[bench] capturing... (Ctrl+C to stop)\n\n");

    // Timing header
    fprintf(stderr, "%-6s | %8s %8s %8s %8s %8s | %8s %6s\n",
        "frame", "cap", "copy", "read", "pack", "send", "total", "FPS");

    uint64_t last_report_us = wgc::now_us();
    int frames_since_report = 0;

    while (running) {
        // ── Stage 1: Capture ──────────────────────────
        uint64_t s0 = wgc::now_us();
        wgc::WgcFrame frame;
        wgc::WgcTiming cap_timing;
        bool got_frame = false;

        if (use_dxgi) {
            int cw = 0, ch = 0;
            got_frame = dxgi_cap.capture(frame.pixels, cw, ch);
            frame.width = cw; frame.height = ch; frame.channels = 4;
            frame.timestamp_us = s0;
            cap_timing.total_us = wgc::now_us() - s0;
            cap_timing.cap_us = cap_timing.total_us;
            cap_timing.copy_us = 0;
            cap_timing.readback_us = 0;
        } else {
            got_frame = wgc_cap.capture(frame, &cap_timing);
        }

        if (!got_frame) {
            stats.drops++;
            if (!no_wait) Sleep(1);
            continue;
        }
        uint64_t s_cap = wgc::now_us();

        // ── Stage 1.5: Scale ──────────────────────────
        int sw = frame.width, sh = frame.height;
        uint64_t s_scale_start = wgc::now_us();
        if (max_dim > 0) {
            scale_bgra(frame.pixels.data(), frame.width, frame.height,
                      scaled, sw, sh, max_dim);
        } else {
            scaled = std::move(frame.pixels);
        }
        uint64_t s_scale_end = wgc::now_us();

        // ── Stage 2: Pack ─────────────────────────────
        auto payload = pack_bgra_payload(max_dim > 0 ? scaled.data() : frame.pixels.data(),
                                         (uint32_t)sw, (uint32_t)sh, 4);
        uint64_t s_pack = wgc::now_us();

        // ── Stage 3: Send ─────────────────────────────
        bool sent = tcp.send_frame(PAYLOAD_TYPE_BGRA_FRAME,
                                   payload.data(), (uint32_t)payload.size());
        uint64_t s_send = wgc::now_us();

        if (!sent) {
            fprintf(stderr, "[tcp] send failed, disconnected\n");
            break;
        }

        // ── Accumulate stats ──────────────────────────
        uint64_t scale_us = s_scale_end - s_scale_start;
        stats.cap_us += cap_timing.cap_us;
        stats.copy_us += cap_timing.copy_us;
        stats.readback_us += cap_timing.readback_us;
        stats.pack_us += s_pack - s_scale_end + scale_us;
        stats.send_us += s_send - s_pack;
        stats.total_us += s_send - s0;
        stats.frames++;
        frames_since_report++;

        // ── Periodic report (every 1 second) ──────────
        uint64_t now = wgc::now_us();
        if (now - last_report_us >= 1'000'000) {
            double elapsed = (now - last_report_us) / 1'000'000.0;
            double fps = frames_since_report / elapsed;
            fprintf(stderr, "%6d | %6lluus %6lluus %6lluus %6lluus %6lluus | %6lluus %5.0f\n",
                stats.frames,
                stats.cap_us / stats.frames,
                stats.copy_us / stats.frames,
                stats.readback_us / stats.frames,
                stats.pack_us / stats.frames,
                stats.send_us / stats.frames,
                stats.total_us / stats.frames,
                fps);

            last_report_us = now;
            frames_since_report = 0;
        }

        // ── Frame pacing ──────────────────────────────
        if (!no_wait) {
            // Target: 60 FPS = ~16.67ms
            uint64_t frame_us = s_send - s0;
            if (frame_us < 16000) {
                // Spin-wait for remainder (more precise than Sleep)
                uint64_t target = 16000 - frame_us;
                uint64_t wait_start = wgc::now_us();
                while (wgc::now_us() - wait_start < target) {
                    _mm_pause();  // CPU hint for spin-wait
                }
            }
        }
    }

    // ── Final report ──────────────────────────────────
    LARGE_INTEGER bench_t1;
    QueryPerformanceCounter(&bench_t1);
    double total_elapsed = (double)(bench_t1.QuadPart - bench_t0.QuadPart) / freq.QuadPart;

    fprintf(stderr, "\n=== Final Stats ===\n");
    fprintf(stderr, "Frames:      %d\n", stats.frames);
    fprintf(stderr, "Drops:       %d\n", stats.drops);
    fprintf(stderr, "Elapsed:     %.2fs\n", total_elapsed);
    fprintf(stderr, "Avg FPS:     %.1f\n", stats.frames / total_elapsed);
    fprintf(stderr, "--- Per-frame avg ---\n");
    fprintf(stderr, "  Capture:   %lluus (%.1f%%)\n",
        stats.cap_us / stats.frames,
        100.0 * (double)(stats.cap_us / stats.frames) / (double)(stats.total_us / stats.frames));
    fprintf(stderr, "  Copy:      %lluus (%.1f%%)\n",
        stats.copy_us / stats.frames,
        100.0 * (double)(stats.copy_us / stats.frames) / (double)(stats.total_us / stats.frames));
    fprintf(stderr, "  Readback:  %lluus (%.1f%%)\n",
        stats.readback_us / stats.frames,
        100.0 * (double)(stats.readback_us / stats.frames) / (double)(stats.total_us / stats.frames));
    fprintf(stderr, "  Pack:      %lluus (%.1f%%)\n",
        stats.pack_us / stats.frames,
        100.0 * (double)(stats.pack_us / stats.frames) / (double)(stats.total_us / stats.frames));
    fprintf(stderr, "  Send:      %lluus (%.1f%%)\n",
        stats.send_us / stats.frames,
        100.0 * (double)(stats.send_us / stats.frames) / (double)(stats.total_us / stats.frames));
    fprintf(stderr, "  Total:     %lluus\n", stats.total_us / stats.frames);
    fprintf(stderr, "  Bandwidth: %.1f MB/s\n",
        (double)stats.frames * (double)(max_dim > 0 ? max_dim * max_dim : 1920 * 1080) * 4
        / total_elapsed / 1024 / 1024);

    if (use_dxgi) dxgi_cap.shutdown(); else wgc_cap.shutdown();
    tcp.disconnect();
    WSACleanup();
    return 0;
}
