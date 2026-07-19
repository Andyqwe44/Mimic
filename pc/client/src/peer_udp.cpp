/**
 * peer_udp.cpp — STUN Binding client + UDP hole-punch + fragmented media.
 */
#include "peer_udp.h"
#include "../../logger/logger.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

namespace {

constexpr uint32_t kMagic = 0x3143504Du; // "MPC1" LE
constexpr uint8_t kTypePunch = 0xFF;
constexpr size_t kMaxFrag = 1100;
constexpr uint32_t kStunMagic = 0x2112A442u;
/** Drop incomplete UDP reassembly after this many ms (lost fragment). */
constexpr DWORD kReasmTimeoutMs = 400;

SOCKET g_sock = INVALID_SOCKET;
std::thread g_reader;
std::thread g_puncher;
std::atomic<bool> g_run{false};
std::mutex g_mtx;
sockaddr_in g_peer = {};
std::atomic<bool> g_peer_set{false};
std::atomic<bool> g_ready{false};
uint16_t g_local_port = 0;
std::vector<PeerUdpCand> g_local_cands;
std::vector<PeerUdpCand> g_remote_cands;
PeerUdpPayloadFn g_on_payload;
PeerUdpReadyFn g_on_ready;
std::atomic<uint32_t> g_msg_id{1};
std::atomic<uint32_t> g_reasm_timeouts{0};
std::atomic<uint32_t> g_udp_send_ok{0};

struct Reasm {
    uint16_t cnt = 0;
    uint8_t type = 0;
    std::vector<std::vector<uint8_t>> parts;
    std::vector<uint8_t> got;
    DWORD started_ms = 0;
};
std::unordered_map<uint32_t, Reasm> g_reasm;

void purge_stale_reasm(DWORD now) {
    for (auto it = g_reasm.begin(); it != g_reasm.end(); ) {
        if (now - it->second.started_ms > kReasmTimeoutMs) {
            uint32_t n = g_reasm_timeouts.fetch_add(1) + 1;
            if (n <= 5 || n % 30 == 0) {
                LOG_WARN("peer", "UDP reasm timeout mid=%u type=%u frags=%u (total_timeouts=%u)",
                         (unsigned)it->first, (unsigned)it->second.type,
                         (unsigned)it->second.cnt, n);
            }
            it = g_reasm.erase(it);
        } else {
            ++it;
        }
    }
}

bool send_to(const sockaddr_in& to, const uint8_t* data, size_t len) {
    if (g_sock == INVALID_SOCKET) return false;
    return sendto(g_sock, (const char*)data, (int)len, 0, (const sockaddr*)&to, sizeof(to)) == (int)len;
}

sockaddr_in make_addr(const std::string& ip, uint16_t port) {
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    inet_pton(AF_INET, ip.c_str(), &a.sin_addr);
    return a;
}

void lock_peer(const sockaddr_in& from) {
    bool was = g_peer_set.exchange(true);
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_peer = from;
    }
    if (!was) {
        g_ready = true;
        LOG("peer", "UDP P2P peer locked");
        if (g_on_ready) g_on_ready();
    }
}

void send_punch_one(const PeerUdpCand& c) {
    if (c.ip.empty() || !c.port) return;
    uint8_t pkt[16];
    uint32_t magic = kMagic;
    memcpy(pkt, &magic, 4);
    uint32_t mid = 0;
    memcpy(pkt + 4, &mid, 4);
    uint16_t z = 0;
    memcpy(pkt + 8, &z, 2);
    memcpy(pkt + 10, &z, 2);
    pkt[12] = kTypePunch;
    pkt[13] = 0;
    pkt[14] = 'P';
    pkt[15] = 'K';
    send_to(make_addr(c.ip, c.port), pkt, 16);
}

bool stun_binding_on_sock(SOCKET s, const std::string& host, uint16_t port, std::string& out_ip, uint16_t& out_port) {
    if (s == INVALID_SOCKET) return false;
    DWORD tv = 2000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));

    sockaddr_in dest = {};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &dest.sin_addr) != 1) {
        addrinfo hints = {}, *res = nullptr;
        hints.ai_family = AF_INET;
        char portstr[16];
        snprintf(portstr, sizeof(portstr), "%u", (unsigned)port);
        if (getaddrinfo(host.c_str(), portstr, &hints, &res) != 0 || !res) return false;
        dest = *(sockaddr_in*)res->ai_addr;
        freeaddrinfo(res);
    }

    uint8_t req[20] = {};
    req[0] = 0x00; req[1] = 0x01;
    uint32_t magic_be = htonl(kStunMagic);
    memcpy(req + 4, &magic_be, 4);
    for (int i = 8; i < 20; ++i) req[i] = (uint8_t)(GetTickCount64() >> (i * 3));

    if (sendto(s, (const char*)req, 20, 0, (sockaddr*)&dest, sizeof(dest)) != 20) return false;

    // Temporarily block for STUN reply (reader not started yet).
    uint8_t resp[128];
    sockaddr_in from = {};
    int flen = sizeof(from);
    int n = recvfrom(s, (char*)resp, sizeof(resp), 0, (sockaddr*)&from, &flen);
    // restore non-blocking-ish long timeout for reader
    DWORD tv_long = 500;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv_long, sizeof(tv_long));
    if (n < 28) return false;
    uint16_t rtype = (resp[0] << 8) | resp[1];
    if (rtype != 0x0101) return false;
    uint32_t rmagic = (resp[4] << 24) | (resp[5] << 16) | (resp[6] << 8) | resp[7];
    if (rmagic != kStunMagic) return false;

    int off = 20;
    int len = (resp[2] << 8) | resp[3];
    while (off + 4 <= n && off + 4 <= 20 + len) {
        uint16_t at = (resp[off] << 8) | resp[off + 1];
        uint16_t al = (resp[off + 2] << 8) | resp[off + 3];
        off += 4;
        if (off + al > n) break;
        if (at == 0x0020 && al >= 8 && resp[off + 1] == 0x01) {
            uint16_t xport = (resp[off + 2] << 8) | resp[off + 3];
            out_port = (uint16_t)(xport ^ ((kStunMagic >> 16) & 0xffff));
            uint8_t ipb[4];
            for (int i = 0; i < 4; ++i)
                ipb[i] = (uint8_t)(resp[off + 4 + i] ^ ((kStunMagic >> (24 - 8 * i)) & 0xff));
            char ipbuf[32];
            snprintf(ipbuf, sizeof(ipbuf), "%u.%u.%u.%u", ipb[0], ipb[1], ipb[2], ipb[3]);
            out_ip = ipbuf;
            return true;
        }
        off += al;
        if (al % 4) off += 4 - (al % 4);
    }
    return false;
}

void handle_datagram(const uint8_t* data, int n, const sockaddr_in& from) {
    if (n < 14) return;
    uint32_t magic = 0;
    memcpy(&magic, data, 4);
    if (magic != kMagic) return;
    uint32_t mid = 0;
    memcpy(&mid, data + 4, 4);
    uint16_t idx = 0, cnt = 0;
    memcpy(&idx, data + 8, 2);
    memcpy(&cnt, data + 10, 2);
    uint8_t type = data[12];
    const uint8_t* payload = data + 14;
    int plen = n - 14;

    if (type == kTypePunch) {
        lock_peer(from);
        // Reply punch so initiator locks us too.
        PeerUdpCand c;
        char ip[64];
        inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
        c.ip = ip;
        c.port = ntohs(from.sin_port);
        c.typ = "peer";
        send_punch_one(c);
        return;
    }

    lock_peer(from);
    if (cnt == 0) return;
    if (idx >= cnt) return;

    uint8_t t = 0;
    std::vector<uint8_t> body;
    bool complete = false;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        DWORD now = GetTickCount();
        purge_stale_reasm(now);
        auto& r = g_reasm[mid];
        if (r.parts.empty()) {
            r.cnt = cnt;
            r.type = type;
            r.parts.resize(cnt);
            r.got.assign(cnt, 0);
            r.started_ms = now;
        }
        if (idx < r.parts.size()) {
            r.parts[idx].assign(payload, payload + plen);
            r.got[idx] = 1;
        }
        bool done = true;
        size_t total = 0;
        for (size_t i = 0; i < r.got.size(); ++i) {
            if (!r.got[i]) { done = false; break; }
            total += r.parts[i].size();
        }
        if (!done) return;
        body.reserve(total);
        for (auto& p : r.parts) body.insert(body.end(), p.begin(), p.end());
        t = r.type;
        g_reasm.erase(mid);
        complete = true;
    }
    if (complete && g_on_payload) g_on_payload(t, body);
}

void reader_loop() {
    uint8_t buf[2048];
    while (g_run.load()) {
        sockaddr_in from = {};
        int flen = sizeof(from);
        int n = recvfrom(g_sock, (char*)buf, sizeof(buf), 0, (sockaddr*)&from, &flen);
        if (n <= 0) {
            if (!g_run.load()) break;
            continue;
        }
        handle_datagram(buf, n, from);
    }
}

void puncher_loop() {
    for (int i = 0; i < 40 && g_run.load() && !g_ready.load(); ++i) {
        std::vector<PeerUdpCand> rem;
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            rem = g_remote_cands;
        }
        for (const auto& c : rem) send_punch_one(c);
        Sleep(250);
    }
}

} // namespace

bool peer_udp_start(const std::string& stun_host, uint16_t stun_port,
                    PeerUdpPayloadFn on_payload, PeerUdpReadyFn on_ready) {
    peer_udp_stop();
    g_on_payload = std::move(on_payload);
    g_on_ready = std::move(on_ready);
    g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_sock == INVALID_SOCKET) return false;
    sockaddr_in local = {};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;
    local.sin_port = 0;
    if (bind(g_sock, (sockaddr*)&local, sizeof(local)) != 0) {
        closesocket(g_sock);
        g_sock = INVALID_SOCKET;
        return false;
    }
    int namelen = sizeof(local);
    getsockname(g_sock, (sockaddr*)&local, &namelen);
    g_local_port = ntohs(local.sin_port);

    std::vector<PeerUdpCand> cands;
    // Host candidates from local adapters (best-effort via gethostname).
    char hostname[256] = {};
    gethostname(hostname, sizeof(hostname));
    addrinfo hints = {}, *res = nullptr;
    hints.ai_family = AF_INET;
    if (getaddrinfo(hostname, nullptr, &hints, &res) == 0) {
        for (addrinfo* p = res; p; p = p->ai_next) {
            char ip[64];
            inet_ntop(AF_INET, &((sockaddr_in*)p->ai_addr)->sin_addr, ip, sizeof(ip));
            if (strncmp(ip, "127.", 4) == 0) continue;
            PeerUdpCand c;
            c.ip = ip;
            c.port = g_local_port;
            c.typ = "host";
            cands.push_back(c);
        }
        freeaddrinfo(res);
    }

    std::string sip;
    uint16_t sport = 0;
    if (!stun_host.empty() && stun_binding_on_sock(g_sock, stun_host, stun_port, sip, sport)) {
        PeerUdpCand c;
        c.ip = sip;
        c.port = sport;
        c.typ = "srflx";
        cands.push_back(c);
        LOG("peer", "STUN srflx %s:%u", sip.c_str(), (unsigned)sport);
    } else {
        LOG_WARN("peer", "STUN binding failed host=%s:%u", stun_host.c_str(), (unsigned)stun_port);
    }

    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_local_cands = cands;
        g_remote_cands.clear();
        g_reasm.clear();
        g_peer_set = false;
        g_ready = false;
        memset(&g_peer, 0, sizeof(g_peer));
    }
    g_run = true;
    g_reader = std::thread(reader_loop);
    LOG("peer", "UDP media listen port=%u cands=%zu", (unsigned)g_local_port, cands.size());
    return !cands.empty() || g_local_port != 0;
}

void peer_udp_stop() {
    g_run = false;
    g_ready = false;
    g_peer_set = false;
    if (g_sock != INVALID_SOCKET) {
        closesocket(g_sock);
        g_sock = INVALID_SOCKET;
    }
    if (g_reader.joinable()) {
        if (g_reader.get_id() != std::this_thread::get_id()) g_reader.join();
        else g_reader.detach();
    }
    if (g_puncher.joinable()) {
        if (g_puncher.get_id() != std::this_thread::get_id()) g_puncher.join();
        else g_puncher.detach();
    }
    std::lock_guard<std::mutex> lk(g_mtx);
    g_local_cands.clear();
    g_remote_cands.clear();
    g_reasm.clear();
    g_local_port = 0;
}

bool peer_udp_ready() { return g_ready.load(); }
uint16_t peer_udp_local_port() { return g_local_port; }

std::vector<PeerUdpCand> peer_udp_local_cands() {
    std::lock_guard<std::mutex> lk(g_mtx);
    return g_local_cands;
}

void peer_udp_set_remote_cands(const std::vector<PeerUdpCand>& cands) {
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_remote_cands = cands;
    }
    if (g_puncher.joinable()) {
        // restart puncher
        g_run = true;
    }
    if (g_puncher.joinable()) {
        // join old if finished
        if (g_ready.load()) return;
    }
    // Always spawn fresh punch bursts
    if (g_puncher.joinable()) {
        try {
            if (g_puncher.joinable() && g_puncher.get_id() != std::this_thread::get_id()) {
                // previous may still be running — detach and start new
                g_puncher.detach();
            }
        } catch (...) {}
    }
    g_puncher = std::thread(puncher_loop);
}

bool peer_udp_send(uint8_t type, const uint8_t* data, size_t len) {
    if (!g_ready.load() || g_sock == INVALID_SOCKET) return false;
    sockaddr_in peer;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        peer = g_peer;
    }
    uint32_t mid = g_msg_id.fetch_add(1);
    uint16_t cnt = (uint16_t)((len + kMaxFrag - 1) / kMaxFrag);
    if (cnt == 0) cnt = 1;
    for (uint16_t i = 0; i < cnt; ++i) {
        size_t off = (size_t)i * kMaxFrag;
        size_t chunk = (off >= len) ? 0 : (len - off < kMaxFrag ? len - off : kMaxFrag);
        std::vector<uint8_t> pkt(14 + chunk);
        uint32_t magic = kMagic;
        memcpy(pkt.data(), &magic, 4);
        memcpy(pkt.data() + 4, &mid, 4);
        memcpy(pkt.data() + 8, &i, 2);
        memcpy(pkt.data() + 10, &cnt, 2);
        pkt[12] = type;
        pkt[13] = 0;
        if (chunk) memcpy(pkt.data() + 14, data + off, chunk);
        if (!send_to(peer, pkt.data(), pkt.size())) return false;
    }
    uint32_t n = g_udp_send_ok.fetch_add(1) + 1;
    if (type == 1 && (n <= 3 || n % 120 == 0)) {
        LOG("peer", "UDP H264 send #%u bytes=%zu frags=%u", n, len, (unsigned)cnt);
    }
    return true;
}

uint32_t peer_udp_reasm_timeouts() { return g_reasm_timeouts.load(); }

std::string peer_udp_cands_json(const std::vector<PeerUdpCand>& cands) {
    std::string s = "[";
    for (size_t i = 0; i < cands.size(); ++i) {
        if (i) s += ",";
        char buf[160];
        snprintf(buf, sizeof(buf), "{\"ip\":\"%s\",\"port\":%u,\"typ\":\"%s\"}",
                 cands[i].ip.c_str(), (unsigned)cands[i].port, cands[i].typ.c_str());
        s += buf;
    }
    s += "]";
    return s;
}

bool peer_udp_parse_cands_json(const std::string& json_arr, std::vector<PeerUdpCand>& out) {
    out.clear();
    size_t i = 0;
    while (i < json_arr.size()) {
        size_t ipos = json_arr.find("\"ip\":\"", i);
        if (ipos == std::string::npos) break;
        ipos += 6;
        size_t iend = json_arr.find('"', ipos);
        if (iend == std::string::npos) break;
        PeerUdpCand c;
        c.ip = json_arr.substr(ipos, iend - ipos);
        size_t ppos = json_arr.find("\"port\":", iend);
        if (ppos == std::string::npos) break;
        c.port = (uint16_t)atoi(json_arr.c_str() + ppos + 7);
        size_t tpos = json_arr.find("\"typ\":\"", iend);
        if (tpos != std::string::npos && tpos < json_arr.find('{', iend + 1)) {
            tpos += 7;
            size_t tend = json_arr.find('"', tpos);
            if (tend != std::string::npos) c.typ = json_arr.substr(tpos, tend - tpos);
        } else c.typ = "srflx";
        if (!c.ip.empty() && c.port) out.push_back(c);
        i = iend + 1;
    }
    return !out.empty();
}
