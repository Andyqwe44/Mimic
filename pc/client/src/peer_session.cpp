/**
 * peer_session.cpp — Signaling client + LAN TCP media for peer control.
 */
#include "peer_session.h"
#include "peer_udp.h"
#include "h264_encoder.h"
#include "../../logger/logger.h"

// Declared in commands.h / main.cpp — binary H.264 → WebView2 SharedBuffer.
void peer_h264_bridge_push(const uint8_t* packed, size_t len);

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "iphlpapi.lib")

namespace {

constexpr char kWsMagic[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

PeerCallbacks g_cb;
std::atomic<bool> g_wsa{false};

std::mutex g_mtx;
std::string g_signaling_http; // http://host:port
std::string g_token;
std::string g_user;
std::string g_device_id;
std::string g_device_name = "PC";
std::vector<std::string> g_lan_ips;
std::vector<PeerDevice> g_devices;
PeerRole g_role = PeerRole::Idle;
PeerSessionInfo g_session;
std::string g_peer_device_id;
std::string g_control_mode = "human"; // human|ai
std::string g_transport = "none";     // lan|p2p|none
std::atomic<bool> g_media_ready{false};
std::atomic<bool> g_ice_started{false};

SOCKET g_sig_sock = INVALID_SOCKET;
std::thread g_sig_reader;
std::thread g_presence_hb;
std::atomic<bool> g_sig_running{false};
/** Last presence fingerprint sent over WS (name|ips|platform|proto). Empty = never sent. */
std::string g_last_presence_fp;
// Intent to stay logged in across WS flaps (parity with Android loggedIn).
std::atomic<bool> g_want_online{false};
std::atomic<bool> g_reconnecting{false};
std::atomic<int> g_reconnect_gen{0};

// LAN media
SOCKET g_lan_listen = INVALID_SOCKET;
SOCKET g_lan_sock = INVALID_SOCKET;
std::thread g_lan_accept;
std::thread g_lan_reader;
std::atomic<bool> g_lan_running{false};
uint16_t g_lan_port = 0;
std::mutex g_lan_send_mtx;
std::atomic<uint32_t> g_lan_h264_sent{0};
std::atomic<uint32_t> g_udp_h264_fallback{0};
std::atomic<uint32_t> g_lan_h264_dropped{0};
// Last IDR while controlled — resent when LAN/UDP becomes ready (parity with Android).
std::mutex g_outbound_key_mtx;
std::vector<uint8_t> g_last_outbound_key;

// Async H.264 writer — single-slot drop-old so encode thread never blocks on TCP.
std::mutex g_lan_q_mtx;
std::condition_variable g_lan_q_cv;
std::vector<uint8_t> g_lan_pending_h264;
std::atomic<bool> g_lan_writer_run{false};
std::thread g_lan_writer;

void ensure_wsa() {
    if (g_wsa.load()) return;
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    g_wsa = true;
}

bool send_all(SOCKET s, const char* data, int n) {
    int sent = 0;
    while (sent < n) {
        int r = send(s, data + sent, n - sent, 0);
        if (r <= 0) return false;
        sent += r;
    }
    return true;
}

bool recv_exact(SOCKET s, char* buf, int n) {
    int got = 0;
    while (got < n) {
        int r = recv(s, buf + got, n - got, 0);
        if (r <= 0) return false;
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
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA1_ALGORITHM, nullptr, 0) != 0) return result;
    if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
        BCryptHashData(hash, (PUCHAR)input.data(), (ULONG)input.size(), 0);
        UCHAR digest[20];
        if (BCryptFinishHash(hash, digest, 20, 0) == 0) result = b64_encode(digest, 20);
        BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return result;
}

/** Wire credential: hex(SHA-256(UTF-8(password))). Never send plaintext password. */
std::string sha256_hex(const std::string& input) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::string result;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return result;
    if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
        BCryptHashData(hash, (PUCHAR)input.data(), (ULONG)input.size(), 0);
        UCHAR digest[32];
        if (BCryptFinishHash(hash, digest, 32, 0) == 0) {
            static const char* kHex = "0123456789abcdef";
            result.resize(64);
            for (int i = 0; i < 32; ++i) {
                result[(size_t)i * 2] = kHex[digest[i] >> 4];
                result[(size_t)i * 2 + 1] = kHex[digest[i] & 0xf];
            }
        }
        BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return result;
}

std::string random_key_b64() {
    uint8_t raw[16];
    BCryptGenRandom(nullptr, raw, 16, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return b64_encode(raw, 16);
}

void fill_mask(uint8_t mask[4]) {
    BCryptGenRandom(nullptr, mask, 4, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
}

void emit_ui(const std::string& json) {
    if (g_cb.on_ui_event) g_cb.on_ui_event(json);
}

std::vector<std::string> collect_lan_ips() {
    // Enumerate all IPv4 adapters (hostname-only misses many Wi-Fi/Ethernet addrs).
    std::vector<std::string> ips;
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG size = 0;
    GetAdaptersAddresses(AF_INET, flags, nullptr, nullptr, &size);
    if (size == 0) return ips;
    std::vector<uint8_t> buf(size);
    auto* addrs = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    if (GetAdaptersAddresses(AF_INET, flags, nullptr, addrs, &size) != NO_ERROR) return ips;
    for (auto* a = addrs; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp) continue;
        if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            if (!u->Address.lpSockaddr || u->Address.lpSockaddr->sa_family != AF_INET) continue;
            auto* sa = reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr);
            char buf_ip[64] = {};
            inet_ntop(AF_INET, &sa->sin_addr, buf_ip, sizeof(buf_ip));
            std::string ip(buf_ip);
            if (ip.empty() || ip.rfind("127.", 0) == 0) continue;
            if (ip.rfind("169.254.", 0) == 0) continue; // link-local APIPA — never dial
            // Prefer private LAN first; still include others (VPN etc. last).
            bool dup = false;
            for (const auto& x : ips) if (x == ip) { dup = true; break; }
            if (dup) continue;
            bool priv = ip.rfind("10.", 0) == 0
                || ip.rfind("192.168.", 0) == 0;
            if (!priv && ip.rfind("172.", 0) == 0) {
                int second = atoi(ip.c_str() + 4);
                priv = second >= 16 && second <= 31;
            }
            if (priv) ips.insert(ips.begin(), ip);
            else ips.push_back(ip);
        }
    }
    return ips;
}

bool parse_http_url(const std::string& url, std::wstring& host, INTERNET_PORT& port, bool& https, std::string& path) {
    https = url.rfind("https://", 0) == 0;
    bool http = url.rfind("http://", 0) == 0;
    if (!https && !http) return false;
    size_t start = https ? 8 : 7;
    size_t slash = url.find('/', start);
    std::string hostport = slash == std::string::npos ? url.substr(start) : url.substr(start, slash - start);
    path = slash == std::string::npos ? "/" : url.substr(slash);
    size_t colon = hostport.find(':');
    std::string host_a = colon == std::string::npos ? hostport : hostport.substr(0, colon);
    port = colon == std::string::npos ? (https ? 443 : 80) : (INTERNET_PORT)atoi(hostport.substr(colon + 1).c_str());
    host.assign(host_a.begin(), host_a.end());
    return !host.empty();
}

std::string http_post_json(const std::string& base, const std::string& api_path, const std::string& body) {
    std::wstring host;
    INTERNET_PORT port = 80;
    bool https = false;
    std::string path;
    if (!parse_http_url(base, host, port, https, path)) return "";
    std::string full_path = api_path;
    HINTERNET hSess = WinHttpOpen(L"MimicClient/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSess) return "";
    HINTERNET hConn = WinHttpConnect(hSess, host.c_str(), port, 0);
    if (!hConn) { WinHttpCloseHandle(hSess); return ""; }
    std::wstring wpath(full_path.begin(), full_path.end());
    HINTERNET hReq = WinHttpOpenRequest(hConn, L"POST", wpath.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, https ? WINHTTP_FLAG_SECURE : 0);
    if (!hReq) {
        WinHttpCloseHandle(hConn); WinHttpCloseHandle(hSess); return "";
    }
    std::wstring headers = L"Content-Type: application/json\r\n";
    BOOL ok = WinHttpSendRequest(hReq, headers.c_str(), (DWORD)headers.size(),
                                 (LPVOID)body.data(), (DWORD)body.size(), (DWORD)body.size(), 0);
    if (!ok || !WinHttpReceiveResponse(hReq, nullptr)) {
        WinHttpCloseHandle(hReq); WinHttpCloseHandle(hConn); WinHttpCloseHandle(hSess);
        return "";
    }
    std::string resp;
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(hReq, &avail) || avail == 0) break;
        std::vector<char> buf(avail);
        DWORD read = 0;
        if (!WinHttpReadData(hReq, buf.data(), avail, &read) || read == 0) break;
        resp.append(buf.data(), read);
    }
    WinHttpCloseHandle(hReq); WinHttpCloseHandle(hConn); WinHttpCloseHandle(hSess);
    return resp;
}

// GET path on base URL. Returns body; empty on transport failure. Optional out_ms for RTT.
std::string http_get(const std::string& base, const std::string& api_path, DWORD* out_ms) {
    std::wstring host;
    INTERNET_PORT port = 80;
    bool https = false;
    std::string path;
    if (!parse_http_url(base, host, port, https, path)) return "";
    HINTERNET hSess = WinHttpOpen(L"MimicClient/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSess) return "";
    // Short timeouts so probe fails loudly instead of hanging the UI thread long.
    WinHttpSetTimeouts(hSess, 2000, 2000, 4000, 4000);
    HINTERNET hConn = WinHttpConnect(hSess, host.c_str(), port, 0);
    if (!hConn) { WinHttpCloseHandle(hSess); return ""; }
    std::wstring wpath(api_path.begin(), api_path.end());
    HINTERNET hReq = WinHttpOpenRequest(hConn, L"GET", wpath.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, https ? WINHTTP_FLAG_SECURE : 0);
    if (!hReq) {
        WinHttpCloseHandle(hConn); WinHttpCloseHandle(hSess); return "";
    }
    ULONGLONG t0 = GetTickCount64();
    BOOL ok = WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                 WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (!ok || !WinHttpReceiveResponse(hReq, nullptr)) {
        WinHttpCloseHandle(hReq); WinHttpCloseHandle(hConn); WinHttpCloseHandle(hSess);
        return "";
    }
    std::string resp;
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(hReq, &avail) || avail == 0) break;
        std::vector<char> buf(avail);
        DWORD read = 0;
        if (!WinHttpReadData(hReq, buf.data(), avail, &read) || read == 0) break;
        resp.append(buf.data(), read);
    }
    if (out_ms) *out_ms = (DWORD)(GetTickCount64() - t0);
    WinHttpCloseHandle(hReq); WinHttpCloseHandle(hConn); WinHttpCloseHandle(hSess);
    return resp;
}

std::string json_get_str_simple(const std::string& j, const char* key) {
    std::string pat = std::string("\"") + key + "\":\"";
    size_t p = j.find(pat);
    if (p == std::string::npos) {
        pat = std::string("\"") + key + "\": \"";
        p = j.find(pat);
    }
    if (p == std::string::npos) return "";
    p += pat.size();
    size_t e = p;
    while (e < j.size() && j[e] != '"') {
        if (j[e] == '\\') e += 2;
        else e++;
    }
    return j.substr(p, e - p);
}

bool json_get_bool_simple(const std::string& j, const char* key) {
    std::string pat = std::string("\"") + key + "\":";
    size_t p = j.find(pat);
    if (p == std::string::npos) return false;
    p += pat.size();
    while (p < j.size() && (j[p] == ' ')) p++;
    return j.compare(p, 4, "true") == 0;
}

bool ws_send_text(SOCKET s, const std::string& text) {
    uint8_t mask[4];
    fill_mask(mask);
    size_t len = text.size();
    uint8_t hdr[14];
    size_t hlen = 2;
    hdr[0] = 0x81;
    if (len < 126) {
        hdr[1] = (uint8_t)(0x80 | len);
    } else if (len <= 0xFFFF) {
        hdr[1] = 0x80 | 126;
        hdr[2] = (uint8_t)((len >> 8) & 0xFF);
        hdr[3] = (uint8_t)(len & 0xFF);
        hlen = 4;
    } else {
        return false;
    }
    std::vector<uint8_t> masked(len);
    for (size_t i = 0; i < len; ++i) masked[i] = (uint8_t)text[i] ^ mask[i % 4];
    if (!send_all(s, (const char*)hdr, (int)hlen)) return false;
    if (!send_all(s, (const char*)mask, 4)) return false;
    return len == 0 || send_all(s, (const char*)masked.data(), (int)len);
}

/** Client→server masked frame (text/pong/etc). */
bool ws_send_frame(SOCKET s, uint8_t opcode, const uint8_t* data, size_t len) {
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
        return false;
    }
    if (!send_all(s, (const char*)hdr, (int)hlen)) return false;
    if (!send_all(s, (const char*)mask, 4)) return false;
    if (len == 0) return true;
    std::vector<uint8_t> masked(len);
    for (size_t i = 0; i < len; ++i) masked[i] = data[i] ^ mask[i % 4];
    return send_all(s, (const char*)masked.data(), (int)len);
}

std::string presence_fingerprint(const std::vector<std::string>& ips) {
    std::string fp = g_device_name + "|windows|2|";
    for (size_t i = 0; i < ips.size(); ++i) {
        if (i) fp += ",";
        fp += ips[i];
    }
    return fp;
}

std::string build_presence_json() {
    g_lan_ips = collect_lan_ips();
    std::string ips = "[";
    for (size_t i = 0; i < g_lan_ips.size(); ++i) {
        if (i) ips += ",";
        ips += "\"" + g_lan_ips[i] + "\"";
    }
    ips += "]";
    return std::string("{\"type\":\"presence\",\"deviceName\":\"") + g_device_name +
           "\",\"lanIps\":" + ips +
           ",\"platform\":\"windows\",\"peerProto\":2}";
}

/** Send presence only when fingerprint changed (or force). Returns false on socket error. */
bool send_presence_if_changed(SOCKET s, bool force) {
    g_lan_ips = collect_lan_ips();
    std::string fp = presence_fingerprint(g_lan_ips);
    if (!force && fp == g_last_presence_fp) return true;
    std::string presence = build_presence_json();
    // rebuild used collect again — keep fp in sync with what we actually send
    fp = presence_fingerprint(g_lan_ips);
    if (!ws_send_text(s, presence)) return false;
    g_last_presence_fp = fp;
    LOG("peer", "presence sent (force=%d ips=%zu)", force ? 1 : 0, g_lan_ips.size());
    return true;
}

void ws_send_close(SOCKET s, uint16_t code) {
    uint8_t payload[2] = { (uint8_t)(code >> 8), (uint8_t)(code & 0xFF) };
    ws_send_frame(s, 0x8, payload, 2);
}

void presence_watch_loop() {
    // Local IP scan only — WS traffic only when name/LAN IPs actually change.
    // Liveness is server WS ping/pong (20s), not app-level presence spam.
    while (g_sig_running.load()) {
        for (int i = 0; i < 50 && g_sig_running.load(); ++i)
            Sleep(100); // ~5s local check
        if (!g_sig_running.load()) break;
        SOCKET s = g_sig_sock;
        if (s == INVALID_SOCKET) break;
        if (!send_presence_if_changed(s, false)) {
            LOG_WARN("peer", "presence watch send failed");
            break;
        }
    }
}

bool ws_handshake(SOCKET s, const std::string& host, uint16_t port, const std::string& path_query) {
    std::string key = random_key_b64();
    char req[1024];
    snprintf(req, sizeof(req),
             "GET %s HTTP/1.1\r\nHost: %s:%u\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
             "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n",
             path_query.c_str(), host.c_str(), (unsigned)port, key.c_str());
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
    return resp.find("101") != std::string::npos;
}

void lan_stop_unlocked();
bool open_signaling_ws(const std::string& http_base, const std::string& token);
void schedule_ws_reconnect();

void lan_send_frame_sync(uint8_t type, const uint8_t* data, size_t len) {
    // Prefer reliable LAN TCP whenever the socket is up (H.264 + control).
    // UDP is only for true P2P when LAN is down — fragmented H.264 over UDP
    // causes macroblock corruption on any lost fragment (same-LAN tearing).
    std::lock_guard<std::mutex> lk(g_lan_send_mtx);
    if (g_lan_sock == INVALID_SOCKET) {
        if (peer_udp_ready()) {
            if (peer_udp_send(type, data, len) && type == 1) {
                uint32_t n = g_udp_h264_fallback.fetch_add(1) + 1;
                if (n <= 3 || n % 120 == 0) {
                    LOG("peer", "H264 via UDP fallback #%u bytes=%zu udp_reasm_to=%u",
                        n, len, peer_udp_reasm_timeouts());
                }
            }
        }
        return;
    }
    uint8_t hdr[5];
    hdr[0] = type;
    uint32_t n = (uint32_t)len;
    memcpy(hdr + 1, &n, 4);
    if (!send_all(g_lan_sock, (const char*)hdr, 5)) {
        closesocket(g_lan_sock);
        g_lan_sock = INVALID_SOCKET;
        g_media_ready = false;
        g_transport = "none";
        LOG("peer", "LAN send failed → transport=none");
        emit_ui(R"({"type":"peer_transport","mode":"none"})");
        return;
    }
    if (len) send_all(g_lan_sock, (const char*)data, (int)len);
    if (type == 1) {
        uint32_t c = g_lan_h264_sent.fetch_add(1) + 1;
        if (c <= 3 || c % 120 == 0) {
            LOG("peer", "H264 via LAN TCP #%u bytes=%zu dropped=%u",
                c, len, g_lan_h264_dropped.load());
        }
    }
}

void lan_writer_loop() {
    while (g_lan_writer_run.load()) {
        std::vector<uint8_t> frame;
        {
            std::unique_lock<std::mutex> lk(g_lan_q_mtx);
            g_lan_q_cv.wait_for(lk, std::chrono::milliseconds(50), [] {
                return !g_lan_pending_h264.empty() || !g_lan_writer_run.load();
            });
            if (!g_lan_writer_run.load() && g_lan_pending_h264.empty()) break;
            if (g_lan_pending_h264.empty()) continue;
            frame.swap(g_lan_pending_h264);
        }
        lan_send_frame_sync(1, frame.data(), frame.size());
    }
}

void lan_ensure_writer() {
    if (g_lan_writer_run.load()) return;
    bool expected = false;
    if (!g_lan_writer_run.compare_exchange_strong(expected, true)) return;
    if (g_lan_writer.joinable()) {
        g_lan_writer.join();
    }
    g_lan_writer = std::thread(lan_writer_loop);
}

void lan_stop_writer() {
    g_lan_writer_run = false;
    g_lan_q_cv.notify_all();
    if (g_lan_writer.joinable()) g_lan_writer.join();
    std::lock_guard<std::mutex> lk(g_lan_q_mtx);
    g_lan_pending_h264.clear();
}

void lan_send_frame(uint8_t type, const uint8_t* data, size_t len) {
    if (type == 1) {
        // Drop-old single slot — encode/WGC path must not block on TCP flush.
        {
            std::lock_guard<std::mutex> lk(g_lan_q_mtx);
            if (!g_lan_pending_h264.empty()) {
                uint32_t d = g_lan_h264_dropped.fetch_add(1) + 1;
                if (d <= 3 || d % 60 == 0) {
                    LOG("peer", "LAN H264 drop-old #%u (keep latest)", d);
                }
            }
            g_lan_pending_h264.assign(data, data + len);
        }
        lan_ensure_writer();
        g_lan_q_cv.notify_one();
        return;
    }
    lan_send_frame_sync(type, data, len);
}

std::mutex g_frame_mtx;
std::vector<uint8_t> g_last_frame;

/** @return true if slot updated (caller may emit peer_frame). */
bool peer_store_frame_(const uint8_t* data, size_t len) {
    std::lock_guard<std::mutex> lk(g_frame_mtx);
    auto flags_at = [](const uint8_t* p, size_t n) -> uint32_t {
        if (n < 12) return 0;
        uint32_t f = 0;
        memcpy(&f, p + 8, 4);
        return f;
    };
    const bool new_key = (flags_at(data, len) & 1u) != 0;
    const bool old_key = (flags_at(g_last_frame.data(), g_last_frame.size()) & 1u) != 0;
    // Never let a delta overwrite an unread IDR — that causes WebCodecs blur until next GOP.
    if (!g_last_frame.empty() && old_key && !new_key)
        return false;
    g_last_frame.assign(data, data + len);
    return true;
}

void handle_lan_payload(uint8_t type, const std::vector<uint8_t>& payload) {
    if (type == 1) {
        if (payload.size() < 16) return;
        // Prefer SharedBuffer (no base64). Keep slot for peer_get_frame fallback only.
        peer_h264_bridge_push(payload.data(), payload.size());
        peer_store_frame_(payload.data(), payload.size());
        return;
    }
    if (type == 2) {
        std::string json((char*)payload.data(), payload.size());
        if (json.find("\"type\":\"control\"") != std::string::npos ||
            json.find("\"type\": \"control\"") != std::string::npos) {
            if (g_cb.on_control) {
                size_t p = json.find("\"action\"");
                if (p != std::string::npos) {
                    size_t b = json.find('{', p);
                    if (b != std::string::npos) {
                        int depth = 0;
                        size_t e = b;
                        for (; e < json.size(); ++e) {
                            if (json[e] == '{') depth++;
                            else if (json[e] == '}') {
                                depth--;
                                if (depth == 0) { e++; break; }
                            }
                        }
                        g_cb.on_control(json.substr(b, e - b));
                        return;
                    }
                }
                g_cb.on_control(json);
            }
            return;
        }
        if (json.find("\"type\":\"need_key\"") != std::string::npos) {
            LOG("peer", "need_key from controller");
            if (g_cb.on_need_key) g_cb.on_need_key();
            return;
        }
        const bool ask_windows = json.find("\"type\":\"list_windows\"") != std::string::npos &&
                                 json.find("\"windows\"") == std::string::npos;
        const bool ask_targets = json.find("\"type\":\"list_targets\"") != std::string::npos &&
                                 json.find("\"targets\"") == std::string::npos;
        if (ask_windows || ask_targets) {
            std::string arr = g_cb.on_list_windows ? g_cb.on_list_windows() : "[]";
            // Dual-shape reply: v1 clients read windows[]; v2 also accept targets[].
            std::string resp = std::string("{\"type\":\"")
                + (ask_targets ? "list_targets" : "list_windows")
                + "\",\"peer_proto\":2,\"windows\":" + arr
                + ",\"targets\":" + arr + "}";
            lan_send_frame(2, (const uint8_t*)resp.data(), resp.size());
            return;
        }
        if (json.find("\"type\":\"set_target\"") != std::string::npos) {
            std::string h = json_get_str_simple(json, "hwnd");
            uint64_t hwnd = 0;
            if (!h.empty()) hwnd = _strtoui64(h.c_str(), nullptr, 10);
            else {
                size_t p = json.find("\"hwnd\":");
                if (p != std::string::npos) hwnd = _strtoui64(json.c_str() + p + 7, nullptr, 10);
            }
            std::string tid = json_get_str_simple(json, "id");
            if (tid.empty()) tid = json_get_str_simple(json, "target_id");
            std::string r = g_cb.on_set_target ? g_cb.on_set_target(hwnd, tid) : R"({"ok":false})";
            std::string resp = std::string("{\"type\":\"set_target_ack\",") + r.substr(1);
            lan_send_frame(2, (const uint8_t*)resp.data(), resp.size());
            return;
        }
        emit_ui(std::string("{\"type\":\"peer_msg\",\"payload\":") + json + "}");
    }
}

void lan_reader_loop() {
    while (g_lan_running && g_lan_sock != INVALID_SOCKET) {
        uint8_t hdr[5];
        if (!recv_exact(g_lan_sock, (char*)hdr, 5)) break;
        uint8_t type = hdr[0];
        uint32_t len = 0;
        memcpy(&len, hdr + 1, 4);
        if (len > 16 * 1024 * 1024) break;
        std::vector<uint8_t> payload(len);
        if (len && !recv_exact(g_lan_sock, (char*)payload.data(), (int)len)) break;
        handle_lan_payload(type, payload);
    }
    g_media_ready = false;
    g_transport = "none";
    LOG("peer", "LAN media disconnected");
    emit_ui(R"({"type":"peer_transport","mode":"none"})");
}

bool lan_start_listen() {
    ensure_wsa();
    lan_stop_unlocked();
    g_lan_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_lan_listen == INVALID_SOCKET) return false;
    int reuse = 1;
    setsockopt(g_lan_listen, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = 0;
    if (bind(g_lan_listen, (sockaddr*)&addr, sizeof(addr)) != 0) {
        closesocket(g_lan_listen); g_lan_listen = INVALID_SOCKET; return false;
    }
    sockaddr_in bound = {};
    int blen = sizeof(bound);
    getsockname(g_lan_listen, (sockaddr*)&bound, &blen);
    g_lan_port = ntohs(bound.sin_port);
    listen(g_lan_listen, 1);
    g_lan_running = true;
    g_lan_accept = std::thread([] {
        SOCKET c = accept(g_lan_listen, nullptr, nullptr);
        if (c == INVALID_SOCKET) return;
        // Bidirectional race: keep first ready socket, drop the late accept.
        if (g_media_ready.load() && g_lan_sock != INVALID_SOCKET) {
            closesocket(c);
            LOG("peer", "LAN accept ignored — already connected");
            return;
        }
        int flag = 1;
        setsockopt(c, IPPROTO_TCP, TCP_NODELAY, (const char*)&flag, sizeof(flag));
        {
            std::lock_guard<std::mutex> lk(g_lan_send_mtx);
            if (g_lan_sock != INVALID_SOCKET) closesocket(g_lan_sock);
            g_lan_sock = c;
        }
        g_media_ready = true;
        g_transport = "lan";
        LOG("peer", "LAN peer connected (accepted) port=%u", (unsigned)g_lan_port);
        emit_ui(R"({"type":"peer_transport","mode":"lan"})");
        if (g_cb.on_need_key) g_cb.on_need_key();
        {
            std::vector<uint8_t> key;
            {
                std::lock_guard<std::mutex> lk(g_outbound_key_mtx);
                key = g_last_outbound_key;
            }
            if (!key.empty()) {
                lan_send_frame(1, key.data(), key.size());
                LOG("peer", "resent buffered IDR (%zu bytes) after LAN accept", key.size());
            }
        }
        g_lan_reader = std::thread(lan_reader_loop);
        if (g_lan_reader.joinable()) g_lan_reader.detach();
    });
    return true;
}

bool lan_connect_to(const std::string& ip, uint16_t port) {
    if (g_media_ready.load() && g_lan_sock != INVALID_SOCKET) {
        LOG("peer", "LAN connect skipped — already ready");
        return true;
    }
    ensure_wsa();
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;
    int flag = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&flag, sizeof(flag));
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) {
        closesocket(s); return false;
    }
    // short connect timeout via nonblock
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    connect(s, (sockaddr*)&addr, sizeof(addr));
    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(s, &wfds);
    timeval tv{3, 0};
    int sel = select(0, nullptr, &wfds, nullptr, &tv);
    if (sel <= 0) {
        LOG_WARN("peer", "LAN connect timeout %s:%u", ip.c_str(), (unsigned)port);
        closesocket(s);
        return false;
    }
    // Check SO_ERROR — select writable does not mean connect succeeded.
    int soerr = 0;
    int olen = sizeof(soerr);
    getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&soerr, &olen);
    if (soerr != 0) {
        LOG_WARN("peer", "LAN connect refused %s:%u err=%d", ip.c_str(), (unsigned)port, soerr);
        closesocket(s);
        return false;
    }
    nb = 0;
    ioctlsocket(s, FIONBIO, &nb);
    if (g_media_ready.load() && g_lan_sock != INVALID_SOCKET) {
        closesocket(s);
        return true;
    }
    {
        std::lock_guard<std::mutex> lk(g_lan_send_mtx);
        if (g_lan_sock != INVALID_SOCKET) closesocket(g_lan_sock);
        g_lan_sock = s;
    }
    g_lan_running = true;
    g_media_ready = true;
    g_transport = "lan";
    LOG("peer", "LAN connected to %s:%u", ip.c_str(), (unsigned)port);
    emit_ui(R"({"type":"peer_transport","mode":"lan"})");
    if (g_cb.on_need_key) g_cb.on_need_key();
    {
        std::vector<uint8_t> key;
        {
            std::lock_guard<std::mutex> lk(g_outbound_key_mtx);
            key = g_last_outbound_key;
        }
        if (!key.empty()) {
            lan_send_frame(1, key.data(), key.size());
            LOG("peer", "resent buffered IDR (%zu bytes) after LAN connect", key.size());
        }
    }
    g_lan_reader = std::thread(lan_reader_loop);
    if (g_lan_reader.joinable()) g_lan_reader.detach();
    return true;
}

void lan_stop_unlocked() {
    g_lan_running = false;
    g_media_ready = false;
    g_transport = "none";
    g_ice_started = false;
    peer_udp_stop();
    if (g_lan_listen != INVALID_SOCKET) { closesocket(g_lan_listen); g_lan_listen = INVALID_SOCKET; }
    {
        // Close first so a writer blocked in send_all unblocks before join.
        std::lock_guard<std::mutex> lk(g_lan_send_mtx);
        if (g_lan_sock != INVALID_SOCKET) { closesocket(g_lan_sock); g_lan_sock = INVALID_SOCKET; }
    }
    lan_stop_writer();
    if (g_lan_accept.joinable()) {
        // accept may block — closing listen unblocks
        g_lan_accept.detach();
    }
    g_lan_port = 0;
}

void signal_send(const std::string& to_device, const std::string& payload_json) {
    if (g_sig_sock == INVALID_SOCKET) return;
    std::string msg = std::string("{\"type\":\"signal\",\"toDeviceId\":\"") + to_device +
                      "\",\"payload\":" + payload_json + "}";
    ws_send_text(g_sig_sock, msg);
}

std::string stun_host_from_signaling() {
    // http://host:port → host
    std::string u = g_signaling_http;
    size_t p = u.find("://");
    if (p != std::string::npos) u = u.substr(p + 3);
    size_t slash = u.find('/');
    if (slash != std::string::npos) u = u.substr(0, slash);
    size_t colon = u.find(':');
    if (colon != std::string::npos) u = u.substr(0, colon);
    return u;
}

void begin_ice_p2p(const char* offer_kind) {
    if (g_ice_started.exchange(true)) {
        // already started — still re-announce cands
    } else {
        std::string host = stun_host_from_signaling();
        bool ok = peer_udp_start(
            host, 3478,
            [](uint8_t type, const std::vector<uint8_t>& payload) {
                handle_lan_payload(type, payload);
            },
            []() {
                g_transport = "p2p";
                g_media_ready = true;
                emit_ui(R"({"type":"peer_transport","mode":"p2p"})");
                LOG("peer", "P2P media ready (UDP hole-punch)");
            },
            [](uint8_t type) {
                // Lost UDP fragment → whole frame gone; ask controlled for a fresh IDR.
                if (type == 1 && g_cb.on_need_key) {
                    LOG("peer", "UDP reasm fail type=1 → need_key");
                    g_cb.on_need_key();
                }
            });
        if (!ok) {
            g_ice_started = false;
            emit_ui(R"json({"type":"peer_error","error":"ICE/STUN start failed — cannot gather candidates"})json");
            return;
        }
    }
    auto cands = peer_udp_local_cands();
    std::string payload = std::string("{\"kind\":\"") + offer_kind + "\",\"cands\":" +
                          peer_udp_cands_json(cands) + ",\"proto\":\"udp-punch\",\"ver\":1}";
    signal_send(g_peer_device_id, payload);
    LOG("peer", "sent %s cands=%zu", offer_kind, cands.size());
}

void on_remote_ice_cands(const std::string& payload) {
    size_t a = payload.find("\"cands\":");
    if (a == std::string::npos) return;
    a = payload.find('[', a);
    if (a == std::string::npos) return;
    size_t e = payload.find(']', a);
    if (e == std::string::npos) return;
    std::vector<PeerUdpCand> rem;
    if (!peer_udp_parse_cands_json(payload.substr(a, e - a + 1), rem)) {
        LOG_WARN("peer", "ICE remote cands parse empty");
        return;
    }
    if (!g_ice_started.load()) begin_ice_p2p("ice_answer");
    peer_udp_set_remote_cands(rem);
    LOG("peer", "ICE punch toward %zu remote cands", rem.size());
}

void try_establish_lan_as_controlled(const PeerSessionInfo& /*sess*/,
                                     const std::vector<std::string>& /*controller_ips*/) {
    // Controlled listens; announces port via signal
    if (!lan_start_listen()) {
        LOG_WARN("peer", "LAN listen failed");
        emit_ui(R"({"type":"peer_error","error":"lan listen failed"})");
        return;
    }
    g_lan_ips = collect_lan_ips();
    char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"kind\":\"lan_offer\",\"port\":%u,\"ips\":[", (unsigned)g_lan_port);
    std::string payload = buf;
    for (size_t i = 0; i < g_lan_ips.size(); ++i) {
        if (i) payload += ",";
        payload += "\"" + g_lan_ips[i] + "\"";
    }
    payload += "]}";
    LOG("peer", "LAN offer port=%u ips=%zu", (unsigned)g_lan_port, g_lan_ips.size());
    signal_send(g_peer_device_id, payload);
}

/** Controller also listens+offers so Controlled can dial out (Windows firewall often
 *  blocks inbound to PC; Android inbound usually works — reverse dial fixes tx=none). */
void try_establish_lan_as_controller() {
    if (!lan_start_listen()) {
        LOG_WARN("peer", "LAN listen (controller) failed — rely on dial-in only");
        return;
    }
    g_lan_ips = collect_lan_ips();
    char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"kind\":\"lan_offer\",\"port\":%u,\"ips\":[", (unsigned)g_lan_port);
    std::string payload = buf;
    for (size_t i = 0; i < g_lan_ips.size(); ++i) {
        if (i) payload += ",";
        payload += "\"" + g_lan_ips[i] + "\"";
    }
    payload += "]}";
    LOG("peer", "LAN offer (controller) port=%u ips=%zu", (unsigned)g_lan_port, g_lan_ips.size());
    signal_send(g_peer_device_id, payload);
}

void try_connect_lan_offer(const std::string& payload) {
    // Either role may receive lan_offer — dial if not already connected.
    if (g_media_ready.load() && g_lan_sock != INVALID_SOCKET) {
        LOG("peer", "lan_offer ignored — already connected");
        return;
    }
    uint16_t port = 0;
    size_t p = payload.find("\"port\":");
    if (p != std::string::npos) port = (uint16_t)atoi(payload.c_str() + p + 7);
    std::vector<std::string> ips;
    size_t a = payload.find("\"ips\":[");
    if (a != std::string::npos) {
        a += 7;
        size_t e = payload.find(']', a);
        std::string arr = payload.substr(a, e - a);
        size_t i = 0;
        while (i < arr.size()) {
            size_t q1 = arr.find('"', i);
            if (q1 == std::string::npos) break;
            size_t q2 = arr.find('"', q1 + 1);
            if (q2 == std::string::npos) break;
            std::string ip = arr.substr(q1 + 1, q2 - q1 - 1);
            if (ip.rfind("169.254.", 0) != 0 && ip.rfind("127.", 0) != 0)
                ips.push_back(ip);
            i = q2 + 1;
        }
    }
    bool ok = false;
    for (const auto& ip : ips) {
        LOG("peer", "LAN dial %s:%u …", ip.c_str(), (unsigned)port);
        if (lan_connect_to(ip, port)) { ok = true; break; }
    }
    if (!ok) {
        // Only the Controller falls through to ICE after failing to dial Controlled.
        // Controlled already listens — wait for reverse dial / ICE answer.
        if (g_role != PeerRole::Controller) {
            LOG_WARN("peer", "LAN dial-out failed (controlled still listening)");
            return;
        }
        LOG_WARN("peer", "LAN fail → ICE/STUN UDP punch (no TURN)");
        emit_ui(R"({"type":"peer_transport","mode":"ice"})");
        signal_send(g_peer_device_id, R"({"kind":"wan_probe","proto":"udp-punch","ver":1})");
        begin_ice_p2p("ice_offer");
        // Fail soft if P2P not up in 15s
        std::thread([]() {
            Sleep(15000);
            if (!peer_udp_ready() && g_transport != "lan") {
                emit_ui(R"json({"type":"peer_error","error":"Direct P2P failed (symmetric NAT or firewall). No TURN relay in this build."})json");
                emit_ui(R"({"type":"peer_transport","mode":"none"})");
            }
        }).detach();
    } else {
        signal_send(g_peer_device_id, R"({"kind":"lan_ack"})");
    }
}

void handle_signal_payload(const std::string& from, const std::string& payload) {
    if (payload.find("\"kind\":\"lan_offer\"") != std::string::npos) {
        try_connect_lan_offer(payload);
        return;
    }
    if (payload.find("\"kind\":\"lan_ack\"") != std::string::npos) {
        emit_ui(R"({"type":"peer_transport","mode":"lan"})");
        return;
    }
    if (payload.find("\"kind\":\"wan_probe\"") != std::string::npos) {
        LOG("peer", "wan_probe → start ICE as controlled");
        begin_ice_p2p("ice_answer");
        return;
    }
    if (payload.find("\"kind\":\"ice_offer\"") != std::string::npos) {
        on_remote_ice_cands(payload);
        if (g_role == PeerRole::Controlled || g_role == PeerRole::Idle)
            begin_ice_p2p("ice_answer");
        return;
    }
    if (payload.find("\"kind\":\"ice_answer\"") != std::string::npos) {
        on_remote_ice_cands(payload);
        return;
    }
    emit_ui(std::string("{\"type\":\"peer_signal\",\"from\":\"") + from +
            "\",\"payload\":" + payload + "}");
}

void parse_devices_and_emit(const std::string& json) {
    emit_ui(json); // already {type:devices,...}
}

void on_sig_message(const std::string& json) {
    std::string type = json_get_str_simple(json, "type");
    if (type == "devices") {
        parse_devices_and_emit(json);
        return;
    }
    if (type == "invite") {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_role = PeerRole::Ringing;
        emit_ui(json);
        return;
    }
    if (type == "invite_sent") {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_role = PeerRole::Outgoing;
        emit_ui(json);
        return;
    }
    if (type == "invite_rejected") {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_role = PeerRole::Idle;
        emit_ui(json);
        return;
    }
    if (type == "session_start") {
        std::string controller = json_get_str_simple(json, "controllerId");
        // nested session object
        size_t p = json.find("\"session\"");
        std::string ctrl = json_get_str_simple(json, "controllerId");
        if (ctrl.empty() && p != std::string::npos) {
            // parse from session block roughly
            size_t c = json.find("\"controllerId\":\"", p);
            if (c != std::string::npos) {
                c += 16;
                size_t e = json.find('"', c);
                ctrl = json.substr(c, e - c);
            }
        }
        std::string controlled;
        size_t d = json.find("\"controlledId\":\"");
        if (d != std::string::npos) {
            d += 16;
            size_t e = json.find('"', d);
            controlled = json.substr(d, e - d);
        }
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            g_session.controllerId = ctrl;
            g_session.controlledId = controlled;
            if (g_device_id == ctrl) {
                g_role = PeerRole::Controller;
                g_peer_device_id = controlled;
            } else {
                g_role = PeerRole::Controlled;
                g_peer_device_id = ctrl;
            }
        }
        emit_ui(json);
        if (g_role == PeerRole::Controlled) {
            try_establish_lan_as_controlled(g_session, {});
        } else if (g_role == PeerRole::Controller) {
            // Reverse dial: Controlled can connect into us if our inbound is firewalled.
            try_establish_lan_as_controller();
        }
        return;
    }
    if (type == "session_end") {
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            g_role = PeerRole::Idle;
            g_session = {};
            g_peer_device_id.clear();
        }
        lan_stop_unlocked();
        emit_ui(json);
        return;
    }
    if (type == "signal") {
        std::string from = json_get_str_simple(json, "fromDeviceId");
        size_t p = json.find("\"payload\":");
        if (p == std::string::npos) return;
        p += 10;
        while (p < json.size() && json[p] == ' ') p++;
        std::string payload;
        if (json[p] == '{') {
            int depth = 0;
            size_t e = p;
            for (; e < json.size(); ++e) {
                if (json[e] == '{') depth++;
                else if (json[e] == '}') {
                    depth--;
                    if (depth == 0) { e++; break; }
                }
            }
            payload = json.substr(p, e - p);
        }
        handle_signal_payload(from, payload);
        return;
    }
    if (type == "error" || type == "hello" || type == "session_state") {
        emit_ui(json);
        return;
    }
    emit_ui(json);
}

void sig_reader_loop() {
    SOCKET s = g_sig_sock;
    while (g_sig_running && s != INVALID_SOCKET) {
        uint8_t h0[2];
        if (!recv_exact(s, (char*)h0, 2)) break;
        int opcode = h0[0] & 0x0F;
        bool masked = (h0[1] & 0x80) != 0;
        uint64_t plen = h0[1] & 0x7F;
        if (plen == 126) {
            uint8_t e[2];
            if (!recv_exact(s, (char*)e, 2)) break;
            plen = ((uint64_t)e[0] << 8) | e[1];
        } else if (plen == 127) break;
        uint8_t mask[4] = {};
        if (masked && !recv_exact(s, (char*)mask, 4)) break;
        std::vector<uint8_t> payload((size_t)plen);
        if (plen && !recv_exact(s, (char*)payload.data(), (int)plen)) break;
        if (masked)
            for (uint64_t i = 0; i < plen; ++i) payload[(size_t)i] ^= mask[i % 4];
        if (opcode == 0x8) break;
        if (opcode == 0x9) {
            // Server ping → reply pong (was wrongly labeled "pong" and ignored).
            ws_send_frame(s, 0xA, payload.data(), payload.size());
            continue;
        }
        if (opcode == 0xA) continue; // ignore unsolicited pong
        if (opcode == 0x1) {
            on_sig_message(std::string((char*)payload.data(), payload.size()));
        }
    }
    g_sig_running = false;
    if (g_sig_sock == s) g_sig_sock = INVALID_SOCKET;
    LOG("peer", "signaling disconnected");
    // Drop local call immediately if we were mid-session (other side already got session_end).
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        if (g_role != PeerRole::Idle) {
            g_role = PeerRole::Idle;
            g_session = {};
            g_peer_device_id.clear();
            lan_stop_unlocked();
            emit_ui(R"({"type":"session_end","reason":"signaling_drop"})");
        }
    }
    if (g_want_online.load() && !g_token.empty()) {
        schedule_ws_reconnect();
    } else {
        g_reconnecting = false;
        emit_ui(R"({"type":"peer_offline"})");
    }
}

void schedule_ws_reconnect() {
    if (!g_want_online.load() || g_token.empty() || g_signaling_http.empty()) return;
    const int gen = ++g_reconnect_gen;
    g_reconnecting = true;
    emit_ui(R"({"type":"peer_reconnecting","reason":"ws_drop"})");
    std::thread([gen]() {
        int attempt = 0;
        while (g_want_online.load() && gen == g_reconnect_gen.load() && !g_token.empty()) {
            if (g_sig_running.load() && g_sig_sock != INVALID_SOCKET) {
                g_reconnecting = false;
                return;
            }
            int delay_ms = 400;
            if (attempt > 0) {
                int shift = attempt - 1;
                if (shift > 5) shift = 5;
                delay_ms = 1000 * (1 << shift);
                if (delay_ms > 30000) delay_ms = 30000;
            }
            for (int i = 0; i < delay_ms / 50 && g_want_online.load() && gen == g_reconnect_gen.load(); ++i)
                Sleep(50);
            if (!g_want_online.load() || gen != g_reconnect_gen.load()) return;

            std::string http, tok;
            {
                std::lock_guard<std::mutex> lk(g_mtx);
                http = g_signaling_http;
                tok = g_token;
            }
            if (http.empty() || tok.empty()) break;

            if (g_sig_reader.joinable() && g_sig_reader.get_id() != std::this_thread::get_id()) {
                try { g_sig_reader.join(); } catch (...) { /* */ }
            }
            if (g_presence_hb.joinable() && g_presence_hb.get_id() != std::this_thread::get_id()) {
                try { g_presence_hb.join(); } catch (...) { /* */ }
            }

            LOG("peer", "ws reconnect attempt=%d", attempt + 1);
            if (open_signaling_ws(http, tok)) {
                g_reconnecting = false;
                emit_ui(R"({"type":"peer_online","reconnected":true})");
                LOG("peer", "ws reconnected");
                return;
            }
            ++attempt;
            if (attempt >= 12) break;
        }
        if (gen != g_reconnect_gen.load()) return;
        g_reconnecting = false;
        g_want_online = false;
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            g_token.clear();
        }
        emit_ui(R"({"type":"peer_offline"})");
        LOG_WARN("peer", "ws reconnect gave up");
    }).detach();
}

bool open_signaling_ws(const std::string& http_base, const std::string& token) {
    std::wstring whost;
    INTERNET_PORT port = 80;
    bool https = false;
    std::string path;
    if (!parse_http_url(http_base, whost, port, https, path)) return false;
    std::string host(whost.begin(), whost.end());
    ensure_wsa();
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        addrinfo hints = {}, *res = nullptr;
        hints.ai_family = AF_INET;
        if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) {
            closesocket(s); return false;
        }
        addr.sin_addr = ((sockaddr_in*)res->ai_addr)->sin_addr;
        freeaddrinfo(res);
    }
    if (connect(s, (sockaddr*)&addr, sizeof(addr)) != 0) {
        closesocket(s); return false;
    }
    std::string pq = std::string("/ws?token=") + token;
    if (!ws_handshake(s, host, port, pq)) {
        closesocket(s); return false;
    }
    g_sig_sock = s;
    g_sig_running = true;
    g_last_presence_fp.clear();
    g_sig_reader = std::thread(sig_reader_loop);
    if (g_presence_hb.joinable()) {
        // previous login should have joined; detach if somehow still running
        g_presence_hb.detach();
    }
    g_presence_hb = std::thread(presence_watch_loop);
    // Announce once on connect; further sends only on IP/name change.
    send_presence_if_changed(s, true);
    ws_send_text(s, R"({"type":"list_devices"})");
    return true;
}

std::string role_str(PeerRole r) {
    switch (r) {
    case PeerRole::Outgoing: return "outgoing";
    case PeerRole::Ringing: return "ringing";
    case PeerRole::Controller: return "controller";
    case PeerRole::Controlled: return "controlled";
    default: return "idle";
    }
}

} // namespace

bool peer_init(PeerCallbacks cb) {
    g_cb = std::move(cb);
    ensure_wsa();
    return true;
}

void peer_shutdown() {
    peer_logout();
}

std::string peer_register(const std::string& signaling_url,
                          const std::string& user,
                          const std::string& password) {
    std::string pass_hash = sha256_hex(password);
    if (pass_hash.size() != 64) return R"({"ok":false,"error":"hash failed"})";
    char body[768];
    snprintf(body, sizeof(body), "{\"user\":\"%s\",\"passHash\":\"%s\"}",
             user.c_str(), pass_hash.c_str());
    std::string resp = http_post_json(signaling_url, "/api/register", body);
    if (resp.empty()) return R"({"ok":false,"error":"network"})";
    return resp;
}

std::string peer_probe(const std::string& signaling_url) {
    std::string base = signaling_url;
    while (!base.empty() && base.back() == '/') base.pop_back();
    if (base.empty()) return R"({"ok":false,"error":"empty url"})";
    DWORD ms = 0;
    std::string resp = http_get(base, "/health", &ms);
    if (resp.empty()) {
        LOG_WARN("peer", "probe unreachable url=%s", base.c_str());
        return R"({"ok":false,"error":"unreachable"})";
    }
    bool ok = (resp.find("\"ok\":true") != std::string::npos) ||
              (resp.find("\"ok\": true") != std::string::npos);
    if (!ok) {
        LOG_WARN("peer", "probe bad body url=%s body=%.120s", base.c_str(), resp.c_str());
        return R"({"ok":false,"error":"bad health response"})";
    }
    int node_count = 0;
    std::string cluster = http_get(base, "/api/cluster", nullptr);
    if (!cluster.empty()) {
        // Prefer nodes array length; fall back to nodeCount from health.
        size_t pos = cluster.find("\"nodes\"");
        if (pos != std::string::npos) {
            int depth = 0;
            bool in_arr = false;
            for (size_t i = pos; i < cluster.size(); ++i) {
                char c = cluster[i];
                if (c == '[') { in_arr = true; depth = 1; node_count = 0; continue; }
                if (!in_arr) continue;
                if (c == '[') depth++;
                else if (c == ']') { depth--; if (depth == 0) break; }
                else if (c == '{' && depth == 1) node_count++;
            }
        }
    }
    if (node_count <= 0) {
        // health may include nodeCount
        size_t p = resp.find("\"nodeCount\"");
        if (p != std::string::npos) {
            p = resp.find(':', p);
            if (p != std::string::npos) node_count = atoi(resp.c_str() + p + 1);
        }
        if (node_count <= 0) node_count = 1;
    }
    char out[192];
    snprintf(out, sizeof(out), "{\"ok\":true,\"rtt_ms\":%u,\"node_count\":%d}",
             (unsigned)ms, node_count);
    LOG("peer", "probe ok url=%s rtt=%ums nodes=%d", base.c_str(), (unsigned)ms, node_count);
    return out;
}

std::string peer_login(const std::string& signaling_url,
                       const std::string& user,
                       const std::string& password,
                       const std::string& device_name) {
    peer_logout();
    g_signaling_http = signaling_url;
    while (!g_signaling_http.empty() && g_signaling_http.back() == '/') g_signaling_http.pop_back();
    g_device_name = device_name.empty() ? "PC" : device_name;
    g_lan_ips = collect_lan_ips();
    char hostname[256] = {};
    gethostname(hostname, sizeof(hostname));
    // stable-ish device id from hostname hash
    uint32_t h = 2166136261u;
    for (const char* p = hostname; *p; ++p) h = (h ^ (uint8_t)*p) * 16777619u;
    char did[32];
    snprintf(did, sizeof(did), "dev-%08x", h);
    g_device_id = did;

    std::string ips = "[";
    for (size_t i = 0; i < g_lan_ips.size(); ++i) {
        if (i) ips += ",";
        ips += "\"" + g_lan_ips[i] + "\"";
    }
    ips += "]";
    std::string pass_hash = sha256_hex(password);
    if (pass_hash.size() != 64) return R"({"ok":false,"error":"hash failed"})";
    char body[1536];
    snprintf(body, sizeof(body),
             "{\"user\":\"%s\",\"passHash\":\"%s\",\"deviceId\":\"%s\",\"deviceName\":\"%s\","
             "\"lanIps\":%s,\"platform\":\"windows\",\"peerProto\":2}",
             user.c_str(), pass_hash.c_str(), g_device_id.c_str(), g_device_name.c_str(), ips.c_str());
    std::string resp = http_post_json(g_signaling_http, "/api/login", body);
    if (resp.empty() || !json_get_bool_simple(resp, "ok")) {
        if (resp.empty()) return R"({"ok":false,"error":"network"})";
        return resp;
    }
    g_token = json_get_str_simple(resp, "token");
    std::string did2 = json_get_str_simple(resp, "deviceId");
    if (!did2.empty()) g_device_id = did2;
    g_user = user;
    if (!open_signaling_ws(g_signaling_http, g_token)) {
        return R"({"ok":false,"error":"ws connect failed"})";
    }
    g_want_online = true;
    g_reconnecting = false;
    LOG("peer", "logged in user=%s device=%s", user.c_str(), g_device_id.c_str());
    char out[512];
    snprintf(out, sizeof(out),
             "{\"ok\":true,\"user\":\"%s\",\"deviceId\":\"%s\",\"deviceName\":\"%s\"}",
             user.c_str(), g_device_id.c_str(), g_device_name.c_str());
    return out;
}

void peer_logout() {
    g_want_online = false;
    g_reconnect_gen.fetch_add(1);
    g_reconnecting = false;
    g_sig_running = false;
    if (g_sig_sock != INVALID_SOCKET) {
        // Intentional offline: tell server immediately (skip away grace), then WS close.
        ws_send_text(g_sig_sock, R"({"type":"goodbye"})");
        ws_send_close(g_sig_sock, 1000);
        closesocket(g_sig_sock);
        g_sig_sock = INVALID_SOCKET;
    }
    g_last_presence_fp.clear();
    if (g_sig_reader.joinable()) {
        if (g_sig_reader.get_id() != std::this_thread::get_id()) g_sig_reader.join();
        else g_sig_reader.detach();
    }
    if (g_presence_hb.joinable()) {
        if (g_presence_hb.get_id() != std::this_thread::get_id()) g_presence_hb.join();
        else g_presence_hb.detach();
    }
    lan_stop_unlocked();
    std::lock_guard<std::mutex> lk(g_mtx);
    g_token.clear();
    g_role = PeerRole::Idle;
    g_session = {};
}

bool peer_online() { return g_sig_running.load() && g_sig_sock != INVALID_SOCKET; }
PeerRole peer_role() { return g_role; }

std::string peer_status_json() {
    char buf[640];
    const bool logged_in = g_want_online.load() && !g_token.empty();
    const bool reconnecting = g_reconnecting.load();
    snprintf(buf, sizeof(buf),
             "{\"ok\":true,\"online\":%s,\"logged_in\":%s,\"reconnecting\":%s,"
             "\"role\":\"%s\",\"user\":\"%s\",\"deviceId\":\"%s\","
             "\"transport\":\"%s\",\"controlMode\":\"%s\",\"mediaReady\":%s}",
             peer_online() ? "true" : "false",
             logged_in ? "true" : "false",
             reconnecting ? "true" : "false",
             role_str(g_role).c_str(),
             g_user.c_str(), g_device_id.c_str(),
             g_transport.c_str(), g_control_mode.c_str(),
             g_media_ready.load() ? "true" : "false");
    return buf;
}

std::string peer_list_devices() {
    if (!peer_online()) return R"({"ok":false,"error":"offline"})";
    if (!ws_send_text(g_sig_sock, R"({"type":"list_devices"})"))
        return R"({"ok":false,"error":"send failed"})";
    return R"({"ok":true})";
}

std::string peer_invite(const std::string& target_device_id) {
    if (!peer_online()) return R"({"ok":false,"error":"offline"})";
    if (g_role != PeerRole::Idle) return R"({"ok":false,"error":"busy"})";
    std::string msg = std::string("{\"type\":\"invite\",\"targetDeviceId\":\"") + target_device_id + "\"}";
    if (!ws_send_text(g_sig_sock, msg)) return R"({"ok":false,"error":"send failed"})";
    return R"({"ok":true})";
}

std::string peer_accept(const std::string& from_device_id) {
    if (!peer_online()) return R"({"ok":false,"error":"offline"})";
    std::string msg = std::string("{\"type\":\"invite_accept\",\"fromDeviceId\":\"") + from_device_id + "\"}";
    if (!ws_send_text(g_sig_sock, msg)) return R"({"ok":false,"error":"send failed"})";
    return R"({"ok":true})";
}

std::string peer_reject(const std::string& from_device_id) {
    if (!peer_online()) return R"({"ok":false,"error":"offline"})";
    std::string msg = std::string("{\"type\":\"invite_reject\",\"fromDeviceId\":\"") + from_device_id + "\"}";
    ws_send_text(g_sig_sock, msg);
    std::lock_guard<std::mutex> lk(g_mtx);
    g_role = PeerRole::Idle;
    return R"({"ok":true})";
}

std::string peer_hangup() {
    if (peer_online()) ws_send_text(g_sig_sock, R"({"type":"hangup"})");
    lan_stop_unlocked();
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_role = PeerRole::Idle;
        g_session = {};
    }
    // Notify UI so gates/stream can be closed (frontend also closes explicitly).
    emit_ui(R"({"type":"session_end","reason":"hangup"})");
    LOG("peer", "hangup → idle");
    return R"({"ok":true})";
}

std::string peer_request_windows() {
    if (g_role != PeerRole::Controller) return R"({"ok":false,"error":"not controller"})";
    // Prefer v2 list_targets; controlled peers that only know v1 still answer list_windows.
    const char* req = "{\"type\":\"list_targets\",\"peer_proto\":2}";
    lan_send_frame(2, (const uint8_t*)req, strlen(req));
    const char* req_v1 = "{\"type\":\"list_windows\"}";
    lan_send_frame(2, (const uint8_t*)req_v1, strlen(req_v1));
    return R"({"ok":true})";
}

std::string peer_set_remote_target(uint64_t hwnd, const std::string& title,
                                   const std::string& target_id) {
    if (g_role != PeerRole::Controller) return R"({"ok":false,"error":"not controller"})";
    char buf[768];
    if (!target_id.empty()) {
        snprintf(buf, sizeof(buf),
                 "{\"type\":\"set_target\",\"peer_proto\":2,\"id\":\"%s\",\"hwnd\":%llu,\"title\":\"%s\"}",
                 target_id.c_str(), (unsigned long long)hwnd, title.c_str());
    } else {
        snprintf(buf, sizeof(buf),
                 "{\"type\":\"set_target\",\"hwnd\":%llu,\"title\":\"%s\"}",
                 (unsigned long long)hwnd, title.c_str());
    }
    lan_send_frame(2, (const uint8_t*)buf, strlen(buf));
    return R"({"ok":true})";
}

std::string peer_send_control(const std::string& action_json) {
    if (g_role != PeerRole::Controller) return R"({"ok":false,"error":"not controller"})";
    if (g_control_mode == "ai") {
        // AI actions still go through same path when generated by UI/agent
    }
    std::string wrap = std::string("{\"type\":\"control\",\"action\":") + action_json + "}";
    lan_send_frame(2, (const uint8_t*)wrap.data(), wrap.size());
    return R"({"ok":true})";
}

std::string peer_request_keyframe() {
    if (g_role != PeerRole::Controller) return R"({"ok":false,"error":"not controller"})";
    const char* msg = "{\"type\":\"need_key\"}";
    lan_send_frame(2, (const uint8_t*)msg, strlen(msg));
    return R"({"ok":true})";
}

std::string peer_set_control_mode(const std::string& mode) {
    if (mode != "human" && mode != "ai") return R"({"ok":false,"error":"mode must be human|ai"})";
    g_control_mode = mode;
    LOG("peer", "control_mode=%s", mode.c_str());
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"ok\":true,\"controlMode\":\"%s\"}", mode.c_str());
    return buf;
}

void peer_send_h264(const H264Packet& pkt) {
    if (g_role != PeerRole::Controlled) return;
    uint32_t flags = pkt.keyframe ? 1u : 0u;
    std::vector<uint8_t> body(16 + pkt.annexb.size());
    uint32_t meta[4] = { (uint32_t)pkt.w, (uint32_t)pkt.h, flags, pkt.ts_ms };
    memcpy(body.data(), meta, 16);
    if (!pkt.annexb.empty())
        memcpy(body.data() + 16, pkt.annexb.data(), pkt.annexb.size());
    if (pkt.keyframe) {
        std::lock_guard<std::mutex> lk(g_outbound_key_mtx);
        g_last_outbound_key = body;
    }
    if (!g_media_ready.load() && !peer_udp_ready()) return;
    lan_send_frame(1, body.data(), body.size());
}

bool peer_media_ready() { return g_media_ready.load() || peer_udp_ready(); }
std::string peer_transport_mode() { return g_transport; }

// Command helper used by commands.cpp
std::string peer_take_last_frame(std::vector<uint8_t>& out) {
    std::lock_guard<std::mutex> lk(g_frame_mtx);
    out.swap(g_last_frame);
    g_last_frame.clear();
    return out.empty() ? R"({"ok":false,"error":"no frame"})" : R"({"ok":true})";
}
