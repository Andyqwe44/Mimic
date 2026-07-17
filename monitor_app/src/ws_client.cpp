/**
 * ws_client.cpp — Minimal RFC6455 client (agent → controller_server).
 */
#include "ws_client.h"
#include "h264_encoder.h"
#include "../../logger/logger.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "bcrypt.lib")

namespace {

constexpr char kWsMagic[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

SOCKET g_sock = INVALID_SOCKET;
std::thread g_reader;
std::atomic<bool> g_running{false};
std::atomic<bool> g_connected{false};
std::mutex g_send_mtx;
WsClientTextFn g_on_text;
WsClientClosedFn g_on_closed;
std::atomic<bool> g_wsa_owned{false};

bool send_all(SOCKET s, const char* data, int n) {
    int sent = 0;
    while (sent < n) {
        int r = send(s, data + sent, n - sent, 0);
        if (r == SOCKET_ERROR || r == 0) return false;
        sent += r;
    }
    return true;
}

bool recv_exact(SOCKET s, char* buf, int n) {
    int got = 0;
    while (got < n) {
        int r = recv(s, buf + got, n - got, 0);
        if (r == 0) return false;
        if (r == SOCKET_ERROR) return false;
        got += r;
    }
    return true;
}

std::string b64_encode(const uint8_t* data, size_t len) {
    static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t n = ((uint32_t)data[i]) << 16;
        if (i + 1 < len) n |= ((uint32_t)data[i + 1]) << 8;
        if (i + 2 < len) n |= data[i + 2];
        out.push_back(T[(n >> 18) & 63]);
        out.push_back(T[(n >> 12) & 63]);
        out.push_back((i + 1 < len) ? T[(n >> 6) & 63] : '=');
        out.push_back((i + 2 < len) ? T[n & 63] : '=');
    }
    return out;
}

std::string random_key_b64() {
    uint8_t raw[16];
    BCryptGenRandom(nullptr, raw, 16, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return b64_encode(raw, 16);
}

void fill_mask(uint8_t mask[4]) {
    BCryptGenRandom(nullptr, mask, 4, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
}

bool ws_send_frame(SOCKET s, uint8_t opcode, const uint8_t* data, size_t len) {
    // Client frames MUST be masked.
    uint8_t mask[4];
    fill_mask(mask);
    uint8_t hdr[14];
    size_t hlen = 2;
    hdr[0] = (uint8_t)(0x80 | (opcode & 0x0F));
    if (len < 126) {
        hdr[1] = (uint8_t)(0x80 | len);
    } else if (len <= 0xFFFF) {
        hdr[1] = 0x80 | 126;
        hdr[2] = (uint8_t)((len >> 8) & 0xFF);
        hdr[3] = (uint8_t)(len & 0xFF);
        hlen = 4;
    } else {
        hdr[1] = 0x80 | 127;
        for (int i = 0; i < 8; ++i)
            hdr[2 + i] = (uint8_t)((len >> (56 - 8 * i)) & 0xFF);
        hlen = 10;
    }
    std::lock_guard<std::mutex> lk(g_send_mtx);
    if (!send_all(s, (const char*)hdr, (int)hlen)) return false;
    if (!send_all(s, (const char*)mask, 4)) return false;
    if (len == 0) return true;
    std::vector<uint8_t> masked(len);
    for (size_t i = 0; i < len; ++i) masked[i] = data[i] ^ mask[i % 4];
    return send_all(s, (const char*)masked.data(), (int)len);
}

bool handshake(SOCKET s, const std::string& host, uint16_t port) {
    std::string key = random_key_b64();
    char req[1024];
    snprintf(req, sizeof(req),
             "GET / HTTP/1.1\r\n"
             "Host: %s:%u\r\n"
             "Upgrade: websocket\r\n"
             "Connection: Upgrade\r\n"
             "Sec-WebSocket-Key: %s\r\n"
             "Sec-WebSocket-Version: 13\r\n"
             "\r\n",
             host.c_str(), (unsigned)port, key.c_str());
    if (!send_all(s, req, (int)strlen(req))) return false;

    std::string resp;
    char buf[2048];
    for (;;) {
        int r = recv(s, buf, sizeof(buf), 0);
        if (r <= 0) return false;
        resp.append(buf, buf + r);
        if (resp.find("\r\n\r\n") != std::string::npos) break;
        if (resp.size() > 8192) return false;
    }
    if (resp.find("101") == std::string::npos) {
        LOG_ERROR("wscli", "handshake rejected: %.80s", resp.c_str());
        return false;
    }
    return true;
}

void reader_loop() {
    SOCKET s = g_sock;
    while (g_running && s != INVALID_SOCKET) {
        uint8_t h0[2];
        if (!recv_exact(s, (char*)h0, 2)) break;
        int opcode = h0[0] & 0x0F;
        bool masked = (h0[1] & 0x80) != 0;
        uint64_t plen = h0[1] & 0x7F;
        if (plen == 126) {
            uint8_t e[2];
            if (!recv_exact(s, (char*)e, 2)) break;
            plen = ((uint64_t)e[0] << 8) | e[1];
        } else if (plen == 127) {
            uint8_t e[8];
            if (!recv_exact(s, (char*)e, 8)) break;
            plen = 0;
            for (int i = 0; i < 8; ++i) plen = (plen << 8) | e[i];
        }
        if (plen > 1024 * 1024) break;
        uint8_t mask[4] = {};
        if (masked && !recv_exact(s, (char*)mask, 4)) break;
        std::vector<uint8_t> payload((size_t)plen);
        if (plen && !recv_exact(s, (char*)payload.data(), (int)plen)) break;
        if (masked) {
            for (uint64_t i = 0; i < plen; ++i)
                payload[(size_t)i] ^= mask[i % 4];
        }
        if (opcode == 0x8) break;
        if (opcode == 0x9) {
            ws_send_frame(s, 0xA, payload.data(), payload.size());
            continue;
        }
        if (opcode == 0x1) {
            std::string json((char*)payload.data(), payload.size());
            if (g_on_text) g_on_text(json);
        }
    }
    g_connected = false;
    g_running = false;
    LOG("wscli", "disconnected from controller_server");
    if (g_on_closed) g_on_closed();
}

} // namespace

void ws_client_set_handlers(WsClientTextFn on_text, WsClientClosedFn on_closed) {
    g_on_text = std::move(on_text);
    g_on_closed = std::move(on_closed);
}

bool ws_client_connect(const std::string& host, uint16_t port) {
    ws_client_disconnect();

    if (!g_wsa_owned.load()) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
        g_wsa_owned = true;
    }

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;
    int flag = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&flag, sizeof(flag));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        // Try resolve hostname
        addrinfo hints = {}, *res = nullptr;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) {
            LOG_ERROR("wscli", "resolve failed host=%s", host.c_str());
            closesocket(s);
            return false;
        }
        addr.sin_addr = ((sockaddr_in*)res->ai_addr)->sin_addr;
        freeaddrinfo(res);
    }

    if (connect(s, (sockaddr*)&addr, sizeof(addr)) != 0) {
        LOG_ERROR("wscli", "connect %s:%u failed err=%d", host.c_str(), (unsigned)port, WSAGetLastError());
        closesocket(s);
        return false;
    }
    if (!handshake(s, host, port)) {
        closesocket(s);
        return false;
    }

    g_sock = s;
    g_running = true;
    g_connected = true;
    g_reader = std::thread(reader_loop);

    // Role handshake
    const char* hello = "{\"role\":\"agent\",\"ver\":1}";
    if (!ws_send_frame(s, 0x1, (const uint8_t*)hello, strlen(hello))) {
        LOG_ERROR("wscli", "role handshake send failed");
        ws_client_disconnect();
        return false;
    }
    LOG("wscli", "connected to controller_server %s:%u", host.c_str(), (unsigned)port);
    return true;
}

void ws_client_disconnect() {
    g_running = false;
    g_connected = false;
    SOCKET s = g_sock;
    g_sock = INVALID_SOCKET;
    if (s != INVALID_SOCKET) {
        closesocket(s);
    }
    if (g_reader.joinable()) {
        if (g_reader.get_id() != std::this_thread::get_id())
            g_reader.join();
        else
            g_reader.detach();
    }
}

bool ws_client_connected() {
    return g_connected.load();
}

bool ws_client_send_text(const std::string& json) {
    if (!g_connected.load() || g_sock == INVALID_SOCKET) return false;
    return ws_send_frame(g_sock, 0x1, (const uint8_t*)json.data(), json.size());
}

void ws_client_send_h264(const H264Packet& pkt) {
    if (!g_connected.load() || g_sock == INVALID_SOCKET) return;
    uint32_t flags = pkt.keyframe ? 1u : 0u;
    std::vector<uint8_t> body(16 + pkt.annexb.size());
    uint32_t meta[4] = { (uint32_t)pkt.w, (uint32_t)pkt.h, flags, pkt.ts_ms };
    memcpy(body.data(), meta, 16);
    if (!pkt.annexb.empty())
        memcpy(body.data() + 16, pkt.annexb.data(), pkt.annexb.size());
    if (!ws_send_frame(g_sock, 0x2, body.data(), body.size())) {
        int err = WSAGetLastError();
        if (err != WSAEWOULDBLOCK && err != WSAENOBUFS)
            LOG_WARN("wscli", "send h264 failed err=%d", err);
    }
}
