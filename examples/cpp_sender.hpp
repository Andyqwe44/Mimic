/**
 * examples/cpp_sender.hpp — send frames via pipe or TCP
 *
 * Usage:
 *   PipeFrameSender sender;                       // stdout pipe
 *   sender.send_frame(pixels, w, h);
 *
 *   TcpFrameSender sender;                        // TCP :9999
 *   sender.connect();
 *   sender.send_frame(pixels, w, h);
 *
 * Build: 任何 C++ 项目 #include "stream_protocol.hpp" + 本文件即可
 */
#pragma once
#include <cstdint>
#include <cstdio>
#include <vector>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <io.h>
#include <fcntl.h>
#include "../common/include/stream_protocol.hpp"

#pragma comment(lib, "ws2_32.lib")

// ═══ Pipe 发送端 ═══
// 通过 stdout pipe 发送帧。接收方从 stdin 读取。
// 使用前必须 _setmode(_fileno(stdout), _O_BINARY)。
struct PipeFrameSender {
    PipeFrameSender() {
        _setmode(_fileno(stdout), _O_BINARY);   // 防止 \n→\r\n
    }

    /// 发送一帧到 stdout
    bool send_frame(const std::vector<uint8_t>& bgra, uint32_t w, uint32_t h) {
        uint8_t hdr[stream_protocol::FRAME_HEADER_SIZE];
        stream_protocol::build_frame_header(
            hdr, w, h, stream_protocol::FRAME_CH_BGRA, (uint32_t)bgra.size());
        if (fwrite(hdr, 1, sizeof(hdr), stdout) != sizeof(hdr)) return false;
        if (fwrite(bgra.data(), 1, bgra.size(), stdout) != bgra.size()) return false;
        fflush(stdout);
        return true;
    }

    /// 发送尺寸为 0 的帧（信号接收方"帧未变"）
    void send_unchanged() {
        uint8_t hdr[stream_protocol::FRAME_HEADER_SIZE] = {};
        fwrite(hdr, 1, sizeof(hdr), stdout);
        fflush(stdout);
    }
};

// ═══ TCP 发送端 ═══
// 监听指定端口，多客户端可同时连接。
struct TcpFrameSender {
    TcpFrameSender() { WSADATA wsa; WSAStartup(MAKEWORD(2,2), &wsa); }
    ~TcpFrameSender() { close(); WSACleanup(); }

    /// 启动监听
    bool listen(uint16_t port = stream_protocol::DEFAULT_TCP_PORT) {
        sock_ = socket(AF_INET, SOCK_STREAM, 0);
        if (sock_ == INVALID_SOCKET) return false;
        int opt = 1;
        setsockopt(sock_, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));
        sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = inet_addr(stream_protocol::DEFAULT_HOST);
        if (bind(sock_, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) return false;
        if (::listen(sock_, 5) == SOCKET_ERROR) return false;
        printf("[tcp] listening on %s:%d\n", stream_protocol::DEFAULT_HOST, port);
        return true;
    }

    /// 非阻塞 accept（每帧调用一次）
    void accept_clients() {
        SOCKET client = accept(sock_, nullptr, nullptr);
        if (client == INVALID_SOCKET) return;
        u_long mode = 0; ioctlsocket(client, FIONBIO, &mode);
        clients_.push_back(client);
        printf("[tcp] client connected (%zu total)\n", clients_.size());
    }

    /// 广播帧到所有客户端。断开的客户端自动移除。
    void broadcast_frame(const std::vector<uint8_t>& bgra, uint32_t w, uint32_t h) {
        uint8_t hdr[stream_protocol::FRAME_HEADER_SIZE];
        stream_protocol::build_frame_header(
            hdr, w, h, stream_protocol::FRAME_CH_BGRA, (uint32_t)bgra.size());

        auto it = clients_.begin();
        while (it != clients_.end()) {
            bool ok = (send(*it, (char*)hdr, sizeof(hdr), 0) == sizeof(hdr));
            if (ok) ok = (send(*it, (char*)bgra.data(), (int)bgra.size(), 0) == (int)bgra.size());
            if (!ok) { closesocket(*it); it = clients_.erase(it); }
            else ++it;
        }
    }

    void close() {
        for (auto c : clients_) closesocket(c);
        clients_.clear();
        if (sock_ != INVALID_SOCKET) { closesocket(sock_); sock_ = INVALID_SOCKET; }
    }

private:
    SOCKET sock_ = INVALID_SOCKET;
    std::vector<SOCKET> clients_;
};
