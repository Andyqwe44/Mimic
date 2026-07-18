/**
 * common/transport/tcp.hpp — broadcast frames over TCP.
 *
 * Depends on: protocol/protocol.h
 * Does NOT depend on payload.
 */
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <winsock2.h>
#include <ws2tcpip.h>
#include "../../../shared/protocol/protocol.h"

#pragma comment(lib, "ws2_32.lib")

namespace transport {

struct TcpSender {
    TcpSender() { WSADATA wsa; WSAStartup(MAKEWORD(2,2), &wsa); }
    ~TcpSender() { close(); WSACleanup(); }

    bool listen(uint16_t port = PROTOCOL_DEFAULT_TCP_PORT) {
        sock_ = socket(AF_INET, SOCK_STREAM, 0);
        if (sock_ == INVALID_SOCKET) return false;
        int opt = 1; setsockopt(sock_, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));
        sockaddr_in addr = {};
        addr.sin_family = AF_INET; addr.sin_port = htons(port);
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        return bind(sock_, (sockaddr*)&addr, sizeof(addr)) != SOCKET_ERROR
            && ::listen(sock_, 5) != SOCKET_ERROR;
    }

    void accept_clients() {
        SOCKET c = accept(sock_, nullptr, nullptr);
        if (c == INVALID_SOCKET) return;
        u_long mode = 0; ioctlsocket(c, FIONBIO, &mode);
        clients_.push_back(c);
    }

    // Send all bytes, looping to handle short writes. Returns true on success.
    static bool send_all(SOCKET s, const char* data, int len) {
        int sent = 0;
        while (sent < len) {
            int n = send(s, data + sent, len - sent, 0);
            if (n <= 0) return false;
            sent += n;
        }
        return true;
    }

    void broadcast(uint32_t type_tag, const void* payload, uint32_t size) {
        uint8_t hdr[PROTOCOL_FRAME_HEADER];
        protocol_build_header(hdr, size, type_tag);
        auto it = clients_.begin();
        while (it != clients_.end()) {
            bool ok = send_all(*it, (char*)hdr, sizeof(hdr));
            if (ok && size > 0) ok = send_all(*it, (char*)payload, (int)size);
            if (!ok) { closesocket(*it); it = clients_.erase(it); }
            else ++it;
        }
    }

    void close() {
        for (auto c : clients_) closesocket(c);
        if (sock_ != INVALID_SOCKET) { closesocket(sock_); sock_ = INVALID_SOCKET; }
    }

private:
    SOCKET sock_ = INVALID_SOCKET;
    std::vector<SOCKET> clients_;
};

} // namespace transport
