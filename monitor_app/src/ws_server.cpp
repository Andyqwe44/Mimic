/**
 * ws_server.cpp — Minimal RFC6455 WebSocket + static HTTP for controller_web.
 */
#include "ws_server.h"
#include "h264_encoder.h"
#include "../../logger/logger.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <string>
#include <fstream>
#include <algorithm>
#include <cstring>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "bcrypt.lib")

namespace {

constexpr uint16_t kWsPort = 9997;
constexpr char kWsMagic[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

struct WsClient {
    SOCKET sock = INVALID_SOCKET;
    std::thread reader;
    std::atomic<bool> alive{true};
};

std::mutex g_mtx;
std::vector<WsClient*> g_clients;
SOCKET g_listen = INVALID_SOCKET;
std::thread g_accept;
std::atomic<bool> g_running{false};
std::string g_static_root;
WsControlFn g_on_control;
WsNeedKeyFn g_on_need_key;

bool send_all(SOCKET s, const char* data, int n) {
    int sent = 0;
    while (sent < n) {
        int r = send(s, data + sent, n - sent, 0);
        if (r == SOCKET_ERROR) {
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK || err == WSAENOBUFS)
                return false; // caller drops frame (keep connection)
            return false;
        }
        if (r == 0) return false;
        sent += r;
    }
    return true;
}

bool recv_exact(SOCKET s, char* buf, int n) {
    int got = 0;
    while (got < n) {
        int r = recv(s, buf + got, n - got, 0);
        if (r == 0) return false;
        if (r == SOCKET_ERROR) {
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK) { Sleep(1); continue; }
            return false;
        }
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

std::string sha1_b64(const std::string& input) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::string result;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA1_ALGORITHM, nullptr, 0) != 0)
        return result;
    if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
        BCryptHashData(hash, (PUCHAR)input.data(), (ULONG)input.size(), 0);
        UCHAR digest[20];
        if (BCryptFinishHash(hash, digest, 20, 0) == 0)
            result = b64_encode(digest, 20);
        BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return result;
}

std::string header_value(const std::string& req, const char* name) {
    std::string key = std::string(name) + ":";
    size_t p = req.find(key);
    if (p == std::string::npos) {
        // case-insensitive light search
        std::string lower = req;
        for (char& c : lower) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        std::string lkey = key;
        for (char& c : lkey) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        p = lower.find(lkey);
        if (p == std::string::npos) return "";
        // map back — use original at same offset
    }
    p = req.find(':', p);
    if (p == std::string::npos) return "";
    p++;
    while (p < req.size() && (req[p] == ' ' || req[p] == '\t')) p++;
    size_t e = req.find("\r\n", p);
    if (e == std::string::npos) e = req.size();
    return req.substr(p, e - p);
}

bool path_safe(const std::string& rel) {
    if (rel.find("..") != std::string::npos) return false;
    if (rel.find(':') != std::string::npos) return false;
    return true;
}

const char* mime_for(const std::string& path) {
    if (path.size() >= 5 && path.substr(path.size() - 5) == ".html") return "text/html; charset=utf-8";
    if (path.size() >= 3 && path.substr(path.size() - 3) == ".js") return "application/javascript; charset=utf-8";
    if (path.size() >= 4 && path.substr(path.size() - 4) == ".css") return "text/css; charset=utf-8";
    if (path.size() >= 4 && path.substr(path.size() - 4) == ".svg") return "image/svg+xml";
    return "application/octet-stream";
}

bool serve_file(SOCKET s, const std::string& url_path) {
    std::string rel = url_path;
    size_t q = rel.find('?');
    if (q != std::string::npos) rel = rel.substr(0, q);
    if (rel.empty() || rel == "/") rel = "/index.html";
    if (rel[0] == '/') rel = rel.substr(1);
    if (!path_safe(rel)) {
        const char* resp = "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        send_all(s, resp, (int)strlen(resp));
        return false;
    }
    std::string full = g_static_root;
    if (!full.empty() && full.back() != '\\' && full.back() != '/') full += '\\';
    for (char& c : rel) if (c == '/') c = '\\';
    full += rel;

    std::ifstream f(full, std::ios::binary);
    if (!f) {
        const char* resp = "HTTP/1.1 404 Not Found\r\nContent-Length: 9\r\nConnection: close\r\n\r\nNot Found";
        send_all(s, resp, (int)strlen(resp));
        return false;
    }
    std::string body((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    char hdr[256];
    snprintf(hdr, sizeof(hdr),
             "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
             "Connection: close\r\nAccess-Control-Allow-Origin: *\r\n\r\n",
             mime_for(rel), body.size());
    if (!send_all(s, hdr, (int)strlen(hdr))) return false;
    return body.empty() || send_all(s, body.data(), (int)body.size());
}

bool ws_send_bin(SOCKET s, const uint8_t* data, size_t len) {
    uint8_t hdr[14];
    size_t hlen = 2;
    hdr[0] = 0x82; // FIN + binary
    if (len < 126) {
        hdr[1] = (uint8_t)len;
    } else if (len <= 0xFFFF) {
        hdr[1] = 126;
        hdr[2] = (uint8_t)((len >> 8) & 0xFF);
        hdr[3] = (uint8_t)(len & 0xFF);
        hlen = 4;
    } else {
        hdr[1] = 127;
        for (int i = 0; i < 8; ++i)
            hdr[2 + i] = (uint8_t)((len >> (56 - 8 * i)) & 0xFF);
        hlen = 10;
    }
    if (!send_all(s, (const char*)hdr, (int)hlen)) return false;
    return len == 0 || send_all(s, (const char*)data, (int)len);
}

bool ws_send_pong(SOCKET s, const uint8_t* data, size_t len) {
    uint8_t hdr[4];
    hdr[0] = 0x8A;
    if (len > 125) len = 125;
    hdr[1] = (uint8_t)len;
    if (!send_all(s, (const char*)hdr, 2)) return false;
    return len == 0 || send_all(s, (const char*)data, (int)len);
}

void client_reader(WsClient* c) {
    while (g_running && c->alive) {
        uint8_t h0[2];
        if (!recv_exact(c->sock, (char*)h0, 2)) break;
        bool fin = (h0[0] & 0x80) != 0;
        int opcode = h0[0] & 0x0F;
        bool masked = (h0[1] & 0x80) != 0;
        uint64_t plen = h0[1] & 0x7F;
        if (plen == 126) {
            uint8_t e[2];
            if (!recv_exact(c->sock, (char*)e, 2)) break;
            plen = ((uint64_t)e[0] << 8) | e[1];
        } else if (plen == 127) {
            uint8_t e[8];
            if (!recv_exact(c->sock, (char*)e, 8)) break;
            plen = 0;
            for (int i = 0; i < 8; ++i) plen = (plen << 8) | e[i];
        }
        if (plen > 1024 * 1024) break;
        uint8_t mask[4] = {};
        if (masked && !recv_exact(c->sock, (char*)mask, 4)) break;
        std::vector<uint8_t> payload((size_t)plen);
        if (plen && !recv_exact(c->sock, (char*)payload.data(), (int)plen)) break;
        if (masked) {
            for (uint64_t i = 0; i < plen; ++i)
                payload[(size_t)i] ^= mask[i % 4];
        }
        if (!fin && opcode != 0) { /* ignore fragmented for control UI */ }
        if (opcode == 0x8) break; // close
        if (opcode == 0x9) { // ping
            ws_send_pong(c->sock, payload.data(), payload.size());
            continue;
        }
        if (opcode == 0x1) { // text = control JSON
            std::string json((char*)payload.data(), payload.size());
            if (g_on_control) g_on_control(json);
        }
    }
    c->alive = false;
}

bool upgrade_ws(SOCKET s, const std::string& req) {
    std::string key = header_value(req, "Sec-WebSocket-Key");
    if (key.empty()) return false;
    std::string accept = sha1_b64(key + kWsMagic);
    char resp[512];
    snprintf(resp, sizeof(resp),
             "HTTP/1.1 101 Switching Protocols\r\n"
             "Upgrade: websocket\r\n"
             "Connection: Upgrade\r\n"
             "Sec-WebSocket-Accept: %s\r\n\r\n",
             accept.c_str());
    return send_all(s, resp, (int)strlen(resp));
}

void accept_loop() {
    while (g_running) {
        SOCKET s = accept(g_listen, nullptr, nullptr);
        if (s == INVALID_SOCKET) {
            if (g_running) Sleep(50);
            continue;
        }
        int flag = 1;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&flag, sizeof(flag));

        // Read HTTP request (blocking)
        std::string req;
        char buf[2048];
        for (;;) {
            int r = recv(s, buf, sizeof(buf), 0);
            if (r <= 0) { closesocket(s); break; }
            req.append(buf, buf + r);
            if (req.find("\r\n\r\n") != std::string::npos) break;
            if (req.size() > 8192) { closesocket(s); s = INVALID_SOCKET; break; }
        }
        if (s == INVALID_SOCKET) continue;

        bool is_ws = req.find("Upgrade: websocket") != std::string::npos ||
                     req.find("Upgrade: WebSocket") != std::string::npos ||
                     req.find("upgrade: websocket") != std::string::npos;
        if (is_ws) {
            if (!upgrade_ws(s, req)) { closesocket(s); continue; }
            // Non-blocking sends after handshake: drop frames under backpressure.
            u_long nb = 1;
            ioctlsocket(s, FIONBIO, &nb);
            auto* c = new WsClient();
            c->sock = s;
            c->reader = std::thread(client_reader, c);
            {
                std::lock_guard<std::mutex> lk(g_mtx);
                g_clients.push_back(c);
            }
            if (g_on_need_key) g_on_need_key();
            LOG("ws", "controller connected (WebSocket)");
            continue;
        }

        // Static GET
        if (req.rfind("GET ", 0) == 0) {
            size_t sp = req.find(' ', 4);
            std::string path = (sp != std::string::npos) ? req.substr(4, sp - 4) : "/";
            serve_file(s, path);
        }
        closesocket(s);
    }
}

} // namespace

bool ws_server_start(const std::string& static_root, WsControlFn on_control, WsNeedKeyFn on_need_key) {
    if (g_running) return true;
    g_static_root = static_root;
    g_on_control = std::move(on_control);
    g_on_need_key = std::move(on_need_key);

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    g_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_listen == INVALID_SOCKET) return false;
    int reuse = 1;
    setsockopt(g_listen, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(kWsPort);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(g_listen, (sockaddr*)&addr, sizeof(addr)) != 0) {
        LOG_ERROR("ws", "bind :%u failed", (unsigned)kWsPort);
        closesocket(g_listen);
        g_listen = INVALID_SOCKET;
        return false;
    }
    listen(g_listen, SOMAXCONN);
    g_running = true;
    g_accept = std::thread(accept_loop);
    LOG("ws", "HTTP+WS server on 0.0.0.0:%u static='%s'", (unsigned)kWsPort, g_static_root.c_str());
    return true;
}

void ws_server_stop() {
    g_running = false;
    if (g_listen != INVALID_SOCKET) { closesocket(g_listen); g_listen = INVALID_SOCKET; }
    if (g_accept.joinable()) g_accept.join();
    std::lock_guard<std::mutex> lk(g_mtx);
    for (auto* c : g_clients) {
        c->alive = false;
        if (c->sock != INVALID_SOCKET) closesocket(c->sock);
        if (c->reader.joinable()) c->reader.join();
        delete c;
    }
    g_clients.clear();
}

bool ws_server_has_clients() {
    std::lock_guard<std::mutex> lk(g_mtx);
    return !g_clients.empty();
}

void ws_broadcast_h264(const H264Packet& pkt) {
    uint32_t flags = pkt.keyframe ? 1u : 0u;
    std::vector<uint8_t> body(16 + pkt.annexb.size());
    // meta[3] = agent encode tick (ms) for controller latency probe
    uint32_t meta[4] = { (uint32_t)pkt.w, (uint32_t)pkt.h, flags, pkt.ts_ms };
    memcpy(body.data(), meta, 16);
    if (!pkt.annexb.empty())
        memcpy(body.data() + 16, pkt.annexb.data(), pkt.annexb.size());

    std::lock_guard<std::mutex> lk(g_mtx);
    for (auto it = g_clients.begin(); it != g_clients.end(); ) {
        WsClient* c = *it;
        if (!c->alive) {
            if (c->sock != INVALID_SOCKET) { closesocket(c->sock); c->sock = INVALID_SOCKET; }
            if (c->reader.joinable()) c->reader.detach();
            delete c;
            it = g_clients.erase(it);
            continue;
        }
        // Drop frame on backpressure — do NOT disconnect (avoids multi-second TCP queue).
        if (!ws_send_bin(c->sock, body.data(), body.size())) {
            int err = WSAGetLastError();
            if (err != WSAEWOULDBLOCK && err != WSAENOBUFS && err != 0) {
                c->alive = false;
                if (c->sock != INVALID_SOCKET) { closesocket(c->sock); c->sock = INVALID_SOCKET; }
                if (c->reader.joinable()) c->reader.detach();
                delete c;
                it = g_clients.erase(it);
                continue;
            }
        }
        ++it;
    }
}
