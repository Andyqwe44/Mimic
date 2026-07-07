/**
 * Visual Game Agent - Main Loop (Phase 0: Generic pixels->actions)
 *
 * Pipeline:
 *   Capture(DXGI) -> Preprocess(84x84 grayscale stack) -> Send(server) ->
 *   Recv(action tokens) -> Decode(GenericActionMapper) -> Execute(Interception/SendInput)
 *
 * Game-agnostic: no game-specific knowledge in the agent code.
 * The model on the server decides WHAT to do based on pixels alone.
 */
#include "agent.hpp"
#include "action_mapper.hpp"
#include "../../common/include/signals.hpp"
#include "../../capture/include/capture.hpp"
#include "../../capture/include/preprocess.hpp"
#include "../../input/include/input.hpp"

#ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>
#include <chrono>
#include "../../logger/logger.h"

// ==================== TCP Client ====================

class AiServerClient {
    SOCKET sock_ = INVALID_SOCKET;
public:
    bool connect(const char* host, int port) {
        struct addrinfo hints = {}, *result = nullptr;
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
        char port_str[16];
        snprintf(port_str, sizeof(port_str), "%d", port);
        if (getaddrinfo(host, port_str, &hints, &result) != 0) return false;
        for (auto* rp = result; rp; rp = rp->ai_next) {
            sock_ = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
            if (sock_ == INVALID_SOCKET) continue;
            if (::connect(sock_, rp->ai_addr, (int)rp->ai_addrlen) == 0) break;
            closesocket(sock_); sock_ = INVALID_SOCKET;
        }
        freeaddrinfo(result);
        if (sock_ != INVALID_SOCKET) {
            // Set recv timeout once at connect time
            DWORD tv = 5000;
            setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, (char*)&tv, sizeof(tv));
        }
        return sock_ != INVALID_SOCKET;
    }

    /** Send preprocessed frame tensor (binary) */
    bool send_tensor(const float* data, int channels, int height, int width) {
        // Pack header: total_size + C + H + W in one buffer
        int data_size = channels * height * width * (int)sizeof(float);
        uint32_t header[4] = {
            htonl((uint32_t)(data_size + 12)),
            htonl((uint32_t)channels),
            htonl((uint32_t)height),
            htonl((uint32_t)width)
        };
        if (!send_all((char*)header, sizeof(header))) return false;
        return send_all((char*)data, data_size);
    }

    /** Receive action tokens from server */
    bool recv_action_tokens(std::vector<uint8_t>& tokens) {
        uint8_t buf[512];  // enough for 32 tokens x ~10 bytes each
        int n = recv(sock_, (char*)buf, sizeof(buf), 0);
        if (n <= 0) return false;

        tokens.assign(buf, buf + n);
        return true;
    }

    void disconnect() {
        if (sock_ != INVALID_SOCKET) { closesocket(sock_); sock_ = INVALID_SOCKET; }
    }
    bool ok() const { return sock_ != INVALID_SOCKET; }

private:
    bool send_all(const char* data, int len) {
        int sent = 0;
        while (sent < len) {
            int n = send(sock_, data + sent, len - sent, 0);
            if (n == SOCKET_ERROR) return false;
            sent += n;
        }
        return true;
    }
};

// ==================== Main Loop ====================

int run_agent(const AgentConfig& cfg) {
    setup_global_signals();

    LOG("agent", "=== Generic Visual Game Agent ===");
    LOG("agent", "Window: %ls", cfg.window_title.c_str());
    LOG("agent", "Server: %s:%d", cfg.server_host.c_str(), cfg.server_port);
    LOG("agent", "Interval: %dms  Max games: %d  Verbose: %s  Dry-run: %s",
           cfg.frame_interval_ms, cfg.max_games,
           cfg.verbose ? "yes" : "no", cfg.dry_run ? "YES" : "no");

    // 1. Capture backend
    auto capture = create_capture_backend();
    LOG("agent", "Capture: %s", capture->name());

    // 2. Input backend
    auto input = create_input_backend();
    LOG("agent", "Input: %s", input->name());

    // 3. Find game window
    Rect game_rect = {};
    if (!capture->get_window_rect(cfg.window_title.c_str(), game_rect)) {
        LOG("agent", "ERROR: Cannot find window '%ls'", cfg.window_title.c_str());
        return 1;
    }
    LOG("agent", "Window rect: x=%d y=%d w=%d h=%d", game_rect.x, game_rect.y,
           game_rect.w, game_rect.h);

    // 4. Connect to AI server
    AiServerClient server;
    if (!server.connect(cfg.server_host.c_str(), cfg.server_port)) {
        LOG("agent", "ERROR: Cannot connect to %s:%d",
                cfg.server_host.c_str(), cfg.server_port);
        LOG("agent", "Start server: python server/model_server.py");
        return 1;
    }
    LOG("agent", "Connected to server.");

    // 5. Action mapper
    ActionDecoder decoder(game_rect.w, game_rect.h);
    GenericActionMapper mapper(input.get());

    // 6. Preprocessor
    FramePreprocessor preproc;

    // 7. Main loop
    FrameBuffer frame_buf;
    float tensor[4 * 84 * 84] = {};
    std::vector<uint8_t> raw_tokens;
    raw_tokens.reserve(512);
    int frame_count = 0;

    LOG("agent", "Agent loop running. Ctrl+C to stop.");

    while (!g_quit_flag) {
        auto t0 = capture_now_us();

        // Capture
        if (!capture->capture(frame_buf, &game_rect)) {
            sleep_ms(10); continue;
        }
        auto t1 = capture_now_us();

        // Preprocess
        if (!preproc.process(frame_buf, tensor)) {
            sleep_ms(10); continue;  // waiting for frame stack
        }
        auto t2 = capture_now_us();

        // Send to server
        if (!server.send_tensor(tensor, 4, 84, 84)) {
            LOG("agent", "ERROR: Send error");
            break;
        }

        // Receive action tokens (reuse buffer)
        raw_tokens.clear();
        if (!server.recv_action_tokens(raw_tokens)) {
            LOG("agent", "ERROR: Recv error/timeout");
            continue;
        }
        auto t3 = capture_now_us();

        // Decode tokens
        auto decoded = decoder.decode(raw_tokens);

        // Execute (unless dry-run)
        if (!cfg.dry_run) {
            mapper.execute(decoded);
        }
        auto t4 = capture_now_us();

        // Logging
        frame_count++;
        if (cfg.verbose) {
            LOG("agent", "[%d] cap=%.1fms pre=%.1fms net=%.1fms act=%.1fms total=%.1fms tokens=%zu",
                   frame_count,
                   (t1 - t0) / 1000.0, (t2 - t1) / 1000.0,
                   (t3 - t2) / 1000.0, (t4 - t3) / 1000.0,
                   (t4 - t0) / 1000.0, decoded.size());
        } else if (frame_count % 10 == 0) {
            LOG("agent", ".");
        }

        sleep_ms(cfg.frame_interval_ms);
    }

    LOG("agent", "Frames processed: %d", frame_count);
    server.disconnect();
    capture->shutdown();
    input->shutdown();
    return 0;
}
