/**
 * TicTacToe - Network & TCP client
 */
#include "network.hpp"
#include "board.hpp"
#include <iostream>
#include <sstream>
#include <string>
#include <cstring>
#include <cstdio>
#include <csignal>
#include "../common/signals.hpp"

// ==================== RAII Winsock ====================

#ifdef _WIN32
WinsockGuard::WinsockGuard() {
    WSADATA wsa;
    ok_ = (WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
    if (!ok_) std::cerr << "Winsock init failed" << std::endl;
}
WinsockGuard::~WinsockGuard() {
    if (ok_) WSACleanup();
}
#else
WinsockGuard::WinsockGuard() { ok_ = true; }
WinsockGuard::~WinsockGuard() {}
#endif

// ==================== Connect ====================

SOCKET connect_to_server(const char* host, int port) {
    struct addrinfo hints = {};
    struct addrinfo* result = nullptr;

    hints.ai_family = AF_UNSPEC;      // IPv4 or IPv6
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    char port_str[16];
    std::snprintf(port_str, sizeof(port_str), "%d", port);

    int ret = getaddrinfo(host, port_str, &hints, &result);
    if (ret != 0) {
#ifdef _WIN32
        std::cerr << "Cannot resolve host '" << host << "': "
                  << gai_strerrorA(ret) << std::endl;
#else
        std::cerr << "Cannot resolve host '" << host << "': "
                  << gai_strerror(ret) << std::endl;
#endif
        return INVALID_SOCKET;
    }

    SOCKET sock = INVALID_SOCKET;
    for (struct addrinfo* rp = result; rp != nullptr; rp = rp->ai_next) {
        sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sock == INVALID_SOCKET) continue;

        if (connect(sock, rp->ai_addr, (int)rp->ai_addrlen) == 0)
            break;  // connected

        closesocket(sock);
        sock = INVALID_SOCKET;
    }

    freeaddrinfo(result);

    if (sock == INVALID_SOCKET) {
        std::cerr << "Cannot connect to AI server " << host << ":" << port
                  << " -- start 'python ai_server.py' first" << std::endl;
    }
    return sock;
}

// ==================== Protocol ====================

static bool send_all(SOCKET sock, const char* data, int len) {
    int sent = 0;
    while (sent < len) {
        int n = send(sock, data + sent, len - sent, 0);
        if (n == SOCKET_ERROR) {
            std::cerr << "send() failed" << std::endl;
            return false;
        }
        sent += n;
    }
    return true;
}

bool send_end(SOCKET sock, int winner) {
    std::string msg = "END " + std::to_string(winner) + "\n";
    return send_all(sock, msg.c_str(), (int)msg.size());
}

bool get_ai_move(SOCKET sock, char player, int& row, int& col, float& value) {
    // Build request: "b0 b1 ... b8 player\n"
    std::ostringstream req;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            int v = (board[i][j] == 'X') ? 1 : (board[i][j] == 'O') ? -1 : 0;
            req << v << " ";
        }
    req << (player == 'X' ? 1 : -1) << "\n";

    std::string msg = req.str();
    if (!send_all(sock, msg.c_str(), (int)msg.size()))
        return false;

    // Receive response
    char buf[256] = {};
    int n = recv(sock, buf, sizeof(buf) - 1, 0);
    if (n <= 0) {
        std::cerr << "recv() failed or connection closed" << std::endl;
        return false;
    }
    buf[n] = '\0';

    std::istringstream resp(buf);
    resp >> row >> col >> value;
    if (resp.fail()) {
        std::cerr << "Invalid response from server: " << buf << std::endl;
        return false;
    }
    return true;
}
