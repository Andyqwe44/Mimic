/**
 * controller_server — Standalone HTTP + WebSocket relay.
 *
 * Listens 0.0.0.0:9997 (override: --port N, --root DIR).
 *   browser  → view H.264 + send control/config
 *   agent    → push H.264 + receive control/config (outbound from GAM)
 *
 * Binary frames: opaque [w:4][h:4][flags:4][ts:4][Annex-B…] (relay only).
 * Text: JSON; first message may declare {"role":"agent"|"browser"}.
 */
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>

#include "logger.h"

#include <atomic>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "bcrypt.lib")

namespace {

constexpr uint16_t kDefaultPort = 9997;
constexpr char kWsMagic[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

enum class Role { Pending, Browser, Agent };

struct Client {
    SOCKET sock = INVALID_SOCKET;
    std::thread reader;
    std::atomic<bool> alive{true};
    Role role = Role::Pending;
};

std::mutex g_mtx;
std::vector<Client*> g_browsers;
Client* g_agent = nullptr;
SOCKET g_listen = INVALID_SOCKET;
std::thread g_accept;
std::atomic<bool> g_running{false};
std::string g_static_root;
uint16_t g_port = kDefaultPort;

// Last config from a browser (re-sent when a new agent connects).
std::mutex g_cfg_mtx;
std::string g_last_config = R"({"type":"config","capture":"wgc","codec":"h264","input":"postmsg"})";

bool send_all(SOCKET s, const char* data, int n) {
    int sent = 0;
    while (sent < n) {
        int r = send(s, data + sent, n - sent, 0);
        if (r == SOCKET_ERROR) {
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK || err == WSAENOBUFS) return false;
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
            if (WSAGetLastError() == WSAEWOULDBLOCK) { Sleep(1); continue; }
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
        std::string lower = req;
        for (char& c : lower) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        std::string lkey = key;
        for (char& c : lkey) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        p = lower.find(lkey);
        if (p == std::string::npos) return "";
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
    return rel.find("..") == std::string::npos && rel.find(':') == std::string::npos;
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
    hdr[0] = 0x82;
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

bool ws_send_text(SOCKET s, const std::string& text) {
    uint8_t hdr[14];
    size_t len = text.size();
    size_t hlen = 2;
    hdr[0] = 0x81;
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
    return len == 0 || send_all(s, text.data(), (int)len);
}

bool ws_send_pong(SOCKET s, const uint8_t* data, size_t len) {
    uint8_t hdr[2];
    hdr[0] = 0x8A;
    if (len > 125) len = 125;
    hdr[1] = (uint8_t)len;
    if (!send_all(s, (const char*)hdr, 2)) return false;
    return len == 0 || send_all(s, (const char*)data, (int)len);
}

void drop_client_locked(Client* c) {
    c->alive = false;
    if (c->sock != INVALID_SOCKET) {
        closesocket(c->sock);
        c->sock = INVALID_SOCKET;
    }
}

void send_to_agent_unlocked(const std::string& text) {
    if (!g_agent || !g_agent->alive || g_agent->sock == INVALID_SOCKET) return;
    if (!ws_send_text(g_agent->sock, text)) {
        LOG_WARN("relay", "send to agent failed — dropping");
        drop_client_locked(g_agent);
    }
}

// Reader thread is the sole owner that deletes Client*. Broadcast only
// closes sockets / erases list entries (never delete — avoids double-free).
void broadcast_browsers_bin(const uint8_t* data, size_t len) {
    std::lock_guard<std::mutex> lk(g_mtx);
    for (auto it = g_browsers.begin(); it != g_browsers.end(); ) {
        Client* c = *it;
        if (!c->alive || c->sock == INVALID_SOCKET) {
            it = g_browsers.erase(it);
            continue;
        }
        if (!ws_send_bin(c->sock, data, len)) {
            int err = WSAGetLastError();
            if (err != WSAEWOULDBLOCK && err != WSAENOBUFS && err != 0) {
                drop_client_locked(c);
                it = g_browsers.erase(it);
                continue;
            }
        }
        ++it;
    }
}

void broadcast_browsers_text(const std::string& text) {
    std::lock_guard<std::mutex> lk(g_mtx);
    for (auto it = g_browsers.begin(); it != g_browsers.end(); ) {
        Client* c = *it;
        if (!c->alive || c->sock == INVALID_SOCKET) {
            it = g_browsers.erase(it);
            continue;
        }
        if (!ws_send_text(c->sock, text)) {
            drop_client_locked(c);
            it = g_browsers.erase(it);
            continue;
        }
        ++it;
    }
}

std::string agent_presence_json(bool online) {
    return online ? R"({"type":"agent_status","online":true})"
                  : R"({"type":"agent_status","online":false})";
}

void register_as_agent(Client* c) {
    std::string cfg;
    {
        std::lock_guard<std::mutex> lk(g_cfg_mtx);
        cfg = g_last_config;
    }
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        if (g_agent && g_agent != c) {
            LOG("relay", "replacing previous agent connection");
            // Close only — old reader thread deletes itself.
            drop_client_locked(g_agent);
            g_agent = nullptr;
        }
        c->role = Role::Agent;
        g_agent = c;
        for (auto it = g_browsers.begin(); it != g_browsers.end(); ) {
            if (*it == c) it = g_browsers.erase(it);
            else ++it;
        }
    }
    LOG("relay", "agent registered");
    broadcast_browsers_text(agent_presence_json(true));
    if (!cfg.empty()) {
        std::lock_guard<std::mutex> lk(g_mtx);
        send_to_agent_unlocked(cfg);
    }
}

void register_as_browser(Client* c) {
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        c->role = Role::Browser;
        bool found = false;
        for (auto* b : g_browsers) if (b == c) { found = true; break; }
        if (!found) g_browsers.push_back(c);
        bool agent_on = g_agent && g_agent->alive;
        ws_send_text(c->sock, agent_presence_json(agent_on));
    }
    size_t n = 0;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        n = g_browsers.size();
    }
    LOG("relay", "browser registered (n=%zu)", n);
}

bool json_has(const std::string& j, const char* needle) {
    return j.find(needle) != std::string::npos;
}

void handle_text(Client* c, const std::string& json) {
    if (c->role == Role::Pending) {
        if (json_has(json, "\"role\":\"agent\"") || json_has(json, "\"role\": \"agent\"")) {
            register_as_agent(c);
            return;
        }
        // Explicit browser or any first message → browser (control UIs connect first).
        register_as_browser(c);
        if (json_has(json, "\"role\"")) return; // handshake only
        // Fall through: treat as control/config from browser
    }

    if (c->role == Role::Agent) {
        // Status / ACK from agent → all browsers
        broadcast_browsers_text(json);
        return;
    }

    // Browser → agent
    if (json_has(json, "\"type\":\"config\"") || json_has(json, "\"type\": \"config\"")) {
        {
            std::lock_guard<std::mutex> lk(g_cfg_mtx);
            g_last_config = json;
        }
        std::lock_guard<std::mutex> lk(g_mtx);
        send_to_agent_unlocked(json);
        return;
    }
    if (json_has(json, "\"type\":\"need_key\"") || json_has(json, "\"need_key\"")) {
        std::lock_guard<std::mutex> lk(g_mtx);
        send_to_agent_unlocked(R"({"type":"need_key"})");
        return;
    }
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        send_to_agent_unlocked(json);
    }
}

void handle_bin(Client* c, const uint8_t* data, size_t len) {
    if (c->role == Role::Pending) {
        // Binary before handshake → assume agent (pushing frames)
        register_as_agent(c);
    }
    if (c->role != Role::Agent) return;
    broadcast_browsers_bin(data, len);
}

void client_reader(Client* c) {
    while (g_running && c->alive) {
        uint8_t h0[2];
        if (!recv_exact(c->sock, (char*)h0, 2)) break;
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
        if (plen > 4 * 1024 * 1024) break;
        uint8_t mask[4] = {};
        if (masked && !recv_exact(c->sock, (char*)mask, 4)) break;
        std::vector<uint8_t> payload((size_t)plen);
        if (plen && !recv_exact(c->sock, (char*)payload.data(), (int)plen)) break;
        if (masked) {
            for (uint64_t i = 0; i < plen; ++i)
                payload[(size_t)i] ^= mask[i % 4];
        }
        if (opcode == 0x8) break;
        if (opcode == 0x9) {
            ws_send_pong(c->sock, payload.data(), payload.size());
            continue;
        }
        if (opcode == 0x1) {
            handle_text(c, std::string((char*)payload.data(), payload.size()));
        } else if (opcode == 0x2) {
            handle_bin(c, payload.data(), payload.size());
        }
    }
    c->alive = false;
    bool was_agent = false;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        if (g_agent == c) {
            g_agent = nullptr;
            was_agent = true;
        }
        for (auto it = g_browsers.begin(); it != g_browsers.end(); ) {
            if (*it == c) it = g_browsers.erase(it);
            else ++it;
        }
        if (c->sock != INVALID_SOCKET) {
            closesocket(c->sock);
            c->sock = INVALID_SOCKET;
        }
    }
    if (was_agent) {
        LOG("relay", "agent disconnected");
        broadcast_browsers_text(agent_presence_json(false));
    }
    // Detach before delete so ~thread does not std::terminate (we are this thread).
    std::thread self = std::move(c->reader);
    delete c;
    if (self.joinable()) self.detach();
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

        std::string req;
        char buf[2048];
        for (;;) {
            int r = recv(s, buf, sizeof(buf), 0);
            if (r <= 0) { closesocket(s); s = INVALID_SOCKET; break; }
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
            u_long nb = 1;
            ioctlsocket(s, FIONBIO, &nb);
            auto* c = new Client();
            c->sock = s;
            c->reader = std::thread(client_reader, c);
            // Held as Pending until role handshake; not in lists yet.
            LOG("relay", "WS peer connected (pending role)");
            continue;
        }

        if (req.rfind("GET ", 0) == 0) {
            size_t sp = req.find(' ', 4);
            std::string path = (sp != std::string::npos) ? req.substr(4, sp - 4) : "/";
            serve_file(s, path);
        }
        closesocket(s);
    }
}

std::string exe_dir() {
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string s(path);
    size_t p = s.find_last_of("\\/");
    if (p != std::string::npos) s = s.substr(0, p);
    return s;
}

BOOL WINAPI on_console_ctrl(DWORD) {
    g_running = false;
    SOCKET s = g_listen;
    g_listen = INVALID_SOCKET;
    if (s != INVALID_SOCKET) closesocket(s);
    return TRUE;
}

void install_ctrl_handler() {
    SetConsoleCtrlHandler(on_console_ctrl, TRUE);
}

} // namespace

int main(int argc, char** argv) {
    g_static_root = exe_dir() + "\\www";
    for (int i = 1; i < argc; ++i) {
        if ((!strcmp(argv[i], "--port") || !strcmp(argv[i], "-p")) && i + 1 < argc) {
            g_port = (uint16_t)atoi(argv[++i]);
        } else if ((!strcmp(argv[i], "--root") || !strcmp(argv[i], "-r")) && i + 1 < argc) {
            g_static_root = argv[++i];
        } else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            printf("controller_server [--port 9997] [--root <static_dir>]\n");
            return 0;
        }
    }

    std::string log_dir = exe_dir() + "\\log";
    CreateDirectoryA(log_dir.c_str(), nullptr);
    capture_log_init("relay", "0.1.0", log_dir.c_str(), 3, 2000);
    capture_log_set_level(LOG_LEVEL_INFO);

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    g_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_listen == INVALID_SOCKET) {
        LOG_ERROR("relay", "socket failed");
        return 1;
    }
    int reuse = 1;
    setsockopt(g_listen, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(g_port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(g_listen, (sockaddr*)&addr, sizeof(addr)) != 0) {
        LOG_ERROR("relay", "bind :%u failed", (unsigned)g_port);
        return 1;
    }
    listen(g_listen, SOMAXCONN);
    g_running = true;
    g_accept = std::thread(accept_loop);
    LOG("relay", "controller_server HTTP+WS on 0.0.0.0:%u www='%s'",
        (unsigned)g_port, g_static_root.c_str());

    install_ctrl_handler();

    while (g_running) Sleep(200);

    if (g_listen != INVALID_SOCKET) {
        closesocket(g_listen);
        g_listen = INVALID_SOCKET;
    }
    if (g_accept.joinable()) g_accept.join();
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        if (g_agent) drop_client_locked(g_agent);
        for (auto* c : g_browsers) drop_client_locked(c);
    }
    // Readers close themselves after socket drop; brief drain.
    Sleep(300);
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_agent = nullptr;
        g_browsers.clear();
    }
    capture_log_shutdown();
    WSACleanup();
    return 0;
}
