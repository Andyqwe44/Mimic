/**
 * peer_udp.cpp — STUN Binding + UDP hole-punch + MPC2 (FEC + NACK).
 */
#include "peer_udp.h"
#include "../../logger/logger.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

namespace {

constexpr uint32_t kMagic = 0x3243504Du; // "MPC2" LE
constexpr uint8_t kTypeH264 = 1;
constexpr uint8_t kTypeJson = 2;
constexpr uint8_t kTypeNack = 3;
constexpr uint8_t kTypeFec = 4;
constexpr uint8_t kTypePunch = 0xFF;
constexpr uint8_t kFlagKey = 0x01;
constexpr uint8_t kFlagHasFec = 0x02;
constexpr size_t kHdr = 16;
constexpr size_t kMaxFrag = 1100;
constexpr int kFecK = 4;
constexpr DWORD kReasmTimeoutMs = 80;
constexpr DWORD kNackAfterMs = 20;
constexpr int kMaxNackPerFrag = 2;
constexpr size_t kSendRing = 32;
constexpr uint32_t kStunMagic = 0x2112A442u;

SOCKET g_sock = INVALID_SOCKET;
std::thread g_reader;
std::thread g_puncher;
std::thread g_reasm_timer;
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
PeerUdpReasmFailFn g_on_reasm_fail;
std::atomic<uint32_t> g_msg_id{1};
std::atomic<uint32_t> g_reasm_timeouts{0};
std::atomic<uint32_t> g_udp_send_ok{0};
std::atomic<uint32_t> g_nack_sent{0};
std::atomic<uint32_t> g_fec_recovered{0};

struct Reasm {
    uint16_t cnt = 0;
    uint8_t type = 0;
    uint8_t flags = 0;
    std::vector<std::vector<uint8_t>> parts;
    std::vector<uint8_t> got;
    std::vector<std::vector<uint8_t>> parity; // per FEC group
    std::vector<uint8_t> parity_got;
    DWORD started_ms = 0;
    bool nack_sent = false;
};

struct SendFrame {
    uint32_t frame_id = 0;
    uint8_t type = 0;
    uint8_t flags = 0;
    uint16_t cnt = 0;
    std::vector<std::vector<uint8_t>> parts;
    std::vector<std::vector<uint8_t>> parity;
    std::vector<uint8_t> nack_count; // per data frag
};

std::unordered_map<uint32_t, Reasm> g_reasm;
std::vector<SendFrame> g_send_ring;
size_t g_send_ring_pos = 0;

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

void write_hdr(uint8_t* pkt, uint32_t frame_id, uint16_t idx, uint16_t cnt,
               uint8_t type, uint8_t flags, uint16_t fec_g) {
    uint32_t magic = kMagic;
    memcpy(pkt, &magic, 4);
    memcpy(pkt + 4, &frame_id, 4);
    memcpy(pkt + 8, &idx, 2);
    memcpy(pkt + 10, &cnt, 2);
    pkt[12] = type;
    pkt[13] = flags;
    memcpy(pkt + 14, &fec_g, 2);
}

void lock_peer(const sockaddr_in& from) {
    bool was = g_peer_set.exchange(true);
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_peer = from;
    }
    if (!was) {
        g_ready = true;
        LOG("peer", "UDP peer locked (MPC2)");
        if (g_on_ready) g_on_ready();
    }
}

void send_punch_one(const PeerUdpCand& c) {
    if (c.ip.empty() || !c.port) return;
    uint8_t pkt[18];
    write_hdr(pkt, 0, 0, 0, kTypePunch, 0, 0);
    pkt[16] = 'P';
    pkt[17] = 'K';
    send_to(make_addr(c.ip, c.port), pkt, 18);
}

bool try_fec_recover(Reasm& r) {
    if (r.cnt == 0) return false;
    int groups = (r.cnt + kFecK - 1) / kFecK;
    bool any = false;
    for (int g = 0; g < groups; ++g) {
        if (g >= (int)r.parity_got.size() || !r.parity_got[g]) continue;
        int missing = -1;
        int miss_count = 0;
        size_t max_len = r.parity[g].size();
        for (int i = 0; i < kFecK; ++i) {
            int idx = g * kFecK + i;
            if (idx >= (int)r.cnt) break;
            if (!r.got[idx]) {
                missing = idx;
                miss_count++;
            } else if (r.parts[idx].size() > max_len) {
                max_len = r.parts[idx].size();
            }
        }
        if (miss_count != 1 || missing < 0) continue;
        std::vector<uint8_t> recovered = r.parity[g];
        if (recovered.size() < max_len) recovered.resize(max_len, 0);
        for (int i = 0; i < kFecK; ++i) {
            int idx = g * kFecK + i;
            if (idx >= (int)r.cnt || idx == missing) continue;
            const auto& p = r.parts[idx];
            for (size_t b = 0; b < recovered.size(); ++b) {
                uint8_t v = (b < p.size()) ? p[b] : 0;
                recovered[b] ^= v;
            }
        }
        // Trim trailing zeros only if other frags suggest shorter — keep parity length
        // Use max of known sibling lengths as hint
        size_t use_len = recovered.size();
        for (int i = 0; i < kFecK; ++i) {
            int idx = g * kFecK + i;
            if (idx >= (int)r.cnt || idx == missing) continue;
            if (r.parts[idx].size() > 0 && r.parts[idx].size() < use_len)
                use_len = std::max(use_len, r.parts[idx].size()); // keep full XOR size
        }
        r.parts[missing] = std::move(recovered);
        r.got[missing] = 1;
        any = true;
        g_fec_recovered.fetch_add(1);
    }
    return any;
}

bool reasm_complete(const Reasm& r) {
    if (r.got.empty()) return false;
    for (uint8_t g : r.got) if (!g) return false;
    return true;
}

void emit_complete(uint32_t mid, Reasm& r) {
    size_t total = 0;
    for (auto& p : r.parts) total += p.size();
    std::vector<uint8_t> body;
    body.reserve(total);
    for (auto& p : r.parts) body.insert(body.end(), p.begin(), p.end());
    uint8_t t = r.type;
    g_reasm.erase(mid);
    if (g_on_payload) g_on_payload(t, body);
}

void send_nack_for(uint32_t frame_id, const Reasm& r) {
    if (!g_ready.load()) return;
    uint32_t bitmap = 0;
    for (size_t i = 0; i < r.got.size() && i < 32; ++i) {
        if (!r.got[i]) bitmap |= (1u << i);
    }
    if (bitmap == 0) return;
    uint8_t payload[8];
    memcpy(payload, &frame_id, 4);
    memcpy(payload + 4, &bitmap, 4);
    sockaddr_in peer;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        peer = g_peer;
    }
    uint8_t pkt[kHdr + 8];
    write_hdr(pkt, frame_id, 0, 1, kTypeNack, 0, 0);
    memcpy(pkt + kHdr, payload, 8);
    if (send_to(peer, pkt, kHdr + 8)) {
        uint32_t n = g_nack_sent.fetch_add(1) + 1;
        if (n <= 5 || n % 50 == 0)
            LOG("peer", "UDP NACK frame=%u bitmap=0x%08x #%u", frame_id, bitmap, n);
    }
}

void retransmit_nacked(uint32_t frame_id, uint32_t bitmap) {
    std::lock_guard<std::mutex> lk(g_mtx);
    SendFrame* sf = nullptr;
    for (auto& f : g_send_ring) {
        if (f.frame_id == frame_id) { sf = &f; break; }
    }
    if (!sf || sf->parts.empty()) return;
    sockaddr_in peer = g_peer;
    for (uint16_t i = 0; i < sf->cnt && i < 32; ++i) {
        if (!(bitmap & (1u << i))) continue;
        if (i >= sf->nack_count.size()) continue;
        if (sf->nack_count[i] >= kMaxNackPerFrag) continue;
        sf->nack_count[i]++;
        const auto& chunk = sf->parts[i];
        uint16_t fec_g = (uint16_t)(i / kFecK);
        std::vector<uint8_t> pkt(kHdr + chunk.size());
        write_hdr(pkt.data(), frame_id, i, sf->cnt, sf->type, sf->flags, fec_g);
        if (!chunk.empty()) memcpy(pkt.data() + kHdr, chunk.data(), chunk.size());
        send_to(peer, pkt.data(), pkt.size());
    }
}

void purge_and_nack(DWORD now) {
    std::vector<std::pair<uint8_t, std::vector<uint8_t>>> completed;
    std::vector<std::pair<uint32_t, uint8_t>> failed;
    std::vector<std::pair<uint32_t, Reasm>> nack_snap;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        for (auto it = g_reasm.begin(); it != g_reasm.end(); ) {
            Reasm& r = it->second;
            DWORD age = now - r.started_ms;
            try_fec_recover(r);
            if (reasm_complete(r)) {
                size_t total = 0;
                for (auto& p : r.parts) total += p.size();
                std::vector<uint8_t> body;
                body.reserve(total);
                for (auto& p : r.parts) body.insert(body.end(), p.begin(), p.end());
                completed.push_back({r.type, std::move(body)});
                it = g_reasm.erase(it);
                continue;
            }
            if (age > kReasmTimeoutMs) {
                failed.push_back({it->first, r.type});
                it = g_reasm.erase(it);
                continue;
            }
            if (!r.nack_sent && age >= kNackAfterMs && (r.type == kTypeH264 || r.type == kTypeJson)) {
                r.nack_sent = true;
                nack_snap.push_back({it->first, r});
            }
            ++it;
        }
    }
    for (auto& c : completed) {
        if (g_on_payload) g_on_payload(c.first, c.second);
    }
    for (auto& ns : nack_snap) send_nack_for(ns.first, ns.second);
    for (auto& f : failed) {
        uint32_t n = g_reasm_timeouts.fetch_add(1) + 1;
        if (n <= 5 || n % 30 == 0) {
            LOG_WARN("peer", "UDP reasm timeout frame=%u type=%u (total=%u)",
                     f.first, (unsigned)f.second, n);
        }
        if (g_on_reasm_fail) g_on_reasm_fail(f.second);
    }
}

void reasm_timer_loop() {
    while (g_run.load()) {
        Sleep(10);
        if (!g_run.load()) break;
        purge_and_nack(GetTickCount());
    }
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

    uint8_t resp[128];
    sockaddr_in from = {};
    int flen = sizeof(from);
    int n = recvfrom(s, (char*)resp, sizeof(resp), 0, (sockaddr*)&from, &flen);
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
    if (n < (int)kHdr) return;
    uint32_t magic = 0;
    memcpy(&magic, data, 4);
    if (magic != kMagic) return;
    uint32_t mid = 0;
    memcpy(&mid, data + 4, 4);
    uint16_t idx = 0, cnt = 0;
    memcpy(&idx, data + 8, 2);
    memcpy(&cnt, data + 10, 2);
    uint8_t type = data[12];
    uint8_t flags = data[13];
    uint16_t fec_g = 0;
    memcpy(&fec_g, data + 14, 2);
    const uint8_t* payload = data + kHdr;
    int plen = n - (int)kHdr;

    if (type == kTypePunch) {
        lock_peer(from);
        PeerUdpCand c;
        char ip[64];
        inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
        c.ip = ip;
        c.port = ntohs(from.sin_port);
        c.typ = "peer";
        send_punch_one(c);
        return;
    }

    if (type == kTypeNack) {
        lock_peer(from);
        if (plen >= 8) {
            uint32_t fid = 0, bitmap = 0;
            memcpy(&fid, payload, 4);
            memcpy(&bitmap, payload + 4, 4);
            retransmit_nacked(fid, bitmap);
        }
        return;
    }

    lock_peer(from);

    if (type == kTypeFec) {
        std::lock_guard<std::mutex> lk(g_mtx);
        auto it = g_reasm.find(mid);
        if (it == g_reasm.end()) return;
        Reasm& r = it->second;
        int groups = (r.cnt + kFecK - 1) / kFecK;
        if (fec_g >= groups) return;
        if ((int)r.parity.size() < groups) {
            r.parity.resize(groups);
            r.parity_got.assign(groups, 0);
        }
        r.parity[fec_g].assign(payload, payload + plen);
        r.parity_got[fec_g] = 1;
        try_fec_recover(r);
        if (reasm_complete(r)) {
            size_t total = 0;
            for (auto& p : r.parts) total += p.size();
            std::vector<uint8_t> body;
            body.reserve(total);
            for (auto& p : r.parts) body.insert(body.end(), p.begin(), p.end());
            uint8_t t = r.type;
            g_reasm.erase(mid);
            if (g_on_payload) g_on_payload(t, body);
        }
        return;
    }

    if (cnt == 0 || idx >= cnt) return;
    if (type != kTypeH264 && type != kTypeJson) return;

    bool complete = false;
    uint8_t t = 0;
    std::vector<uint8_t> body;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        DWORD now = GetTickCount();
        auto& r = g_reasm[mid];
        if (r.parts.empty()) {
            r.cnt = cnt;
            r.type = type;
            r.flags = flags;
            r.parts.resize(cnt);
            r.got.assign(cnt, 0);
            int groups = (cnt + kFecK - 1) / kFecK;
            r.parity.resize(groups);
            r.parity_got.assign(groups, 0);
            r.started_ms = now;
        }
        if (idx < r.parts.size()) {
            r.parts[idx].assign(payload, payload + plen);
            r.got[idx] = 1;
        }
        try_fec_recover(r);
        if (!reasm_complete(r)) return;
        size_t total = 0;
        for (auto& p : r.parts) total += p.size();
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

std::vector<PeerUdpCand> collect_host_cands(uint16_t port) {
    std::vector<PeerUdpCand> cands;
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
            c.port = port;
            c.typ = "host";
            cands.push_back(c);
        }
        freeaddrinfo(res);
    }
    return cands;
}

bool start_common(PeerUdpPayloadFn on_payload, PeerUdpReadyFn on_ready,
                  PeerUdpReasmFailFn on_reasm_fail, const std::string& stun_host, uint16_t stun_port) {
    peer_udp_stop();
    g_on_payload = std::move(on_payload);
    g_on_ready = std::move(on_ready);
    g_on_reasm_fail = std::move(on_reasm_fail);
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

    std::vector<PeerUdpCand> cands = collect_host_cands(g_local_port);

    if (!stun_host.empty()) {
        std::string sip;
        uint16_t sport = 0;
        if (stun_binding_on_sock(g_sock, stun_host, stun_port, sip, sport)) {
            PeerUdpCand c;
            c.ip = sip;
            c.port = sport;
            c.typ = "srflx";
            cands.push_back(c);
            LOG("peer", "STUN srflx %s:%u", sip.c_str(), (unsigned)sport);
        } else {
            LOG_WARN("peer", "STUN binding failed host=%s:%u", stun_host.c_str(), (unsigned)stun_port);
        }
    }

    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_local_cands = cands;
        g_remote_cands.clear();
        g_reasm.clear();
        g_send_ring.clear();
        g_send_ring.resize(kSendRing);
        g_send_ring_pos = 0;
        g_peer_set = false;
        g_ready = false;
        memset(&g_peer, 0, sizeof(g_peer));
    }
    g_run = true;
    g_reader = std::thread(reader_loop);
    g_reasm_timer = std::thread(reasm_timer_loop);
    LOG("peer", "UDP MPC2 listen port=%u cands=%zu", (unsigned)g_local_port, cands.size());
    return g_local_port != 0;
}

} // namespace

bool peer_udp_start(const std::string& stun_host, uint16_t stun_port,
                    PeerUdpPayloadFn on_payload, PeerUdpReadyFn on_ready,
                    PeerUdpReasmFailFn on_reasm_fail) {
    return start_common(std::move(on_payload), std::move(on_ready), std::move(on_reasm_fail),
                        stun_host, stun_port);
}

bool peer_udp_start_lan(PeerUdpPayloadFn on_payload, PeerUdpReadyFn on_ready,
                        PeerUdpReasmFailFn on_reasm_fail) {
    return start_common(std::move(on_payload), std::move(on_ready), std::move(on_reasm_fail),
                        "", 0);
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
    if (g_reasm_timer.joinable()) {
        if (g_reasm_timer.get_id() != std::this_thread::get_id()) g_reasm_timer.join();
        else g_reasm_timer.detach();
    }
    if (g_puncher.joinable()) {
        if (g_puncher.get_id() != std::this_thread::get_id()) g_puncher.join();
        else g_puncher.detach();
    }
    std::lock_guard<std::mutex> lk(g_mtx);
    g_local_cands.clear();
    g_remote_cands.clear();
    g_reasm.clear();
    g_send_ring.clear();
    g_local_port = 0;
    g_on_payload = nullptr;
    g_on_ready = nullptr;
    g_on_reasm_fail = nullptr;
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
        try {
            if (g_puncher.get_id() != std::this_thread::get_id()) g_puncher.detach();
        } catch (...) {}
    }
    g_puncher = std::thread(puncher_loop);
}

bool peer_udp_send(uint8_t type, const uint8_t* data, size_t len, uint8_t flags) {
    if (!g_ready.load() || g_sock == INVALID_SOCKET) return false;
    sockaddr_in peer;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        peer = g_peer;
    }
    uint32_t mid = g_msg_id.fetch_add(1);
    uint16_t cnt = (uint16_t)((len + kMaxFrag - 1) / kMaxFrag);
    if (cnt == 0) cnt = 1;
    if (cnt > 32) {
        // Cap: oversized frames skip FEC grouping beyond bitmap; still send all frags
    }
    bool use_fec = (type == kTypeH264 || type == kTypeJson) && cnt > 1;
    if (use_fec) flags = (uint8_t)(flags | kFlagHasFec);

    SendFrame sf;
    sf.frame_id = mid;
    sf.type = type;
    sf.flags = flags;
    sf.cnt = cnt;
    sf.parts.resize(cnt);
    sf.nack_count.assign(cnt, 0);
    int groups = (cnt + kFecK - 1) / kFecK;
    sf.parity.resize(groups);

    for (uint16_t i = 0; i < cnt; ++i) {
        size_t off = (size_t)i * kMaxFrag;
        size_t chunk = (off >= len) ? 0 : (len - off < kMaxFrag ? len - off : kMaxFrag);
        sf.parts[i].assign(data + off, data + off + chunk);
        uint16_t fec_g = (uint16_t)(i / kFecK);
        std::vector<uint8_t> pkt(kHdr + chunk);
        write_hdr(pkt.data(), mid, i, cnt, type, flags, fec_g);
        if (chunk) memcpy(pkt.data() + kHdr, data + off, chunk);
        if (!send_to(peer, pkt.data(), pkt.size())) return false;
    }

    if (use_fec) {
        for (int g = 0; g < groups; ++g) {
            size_t max_len = 0;
            for (int i = 0; i < kFecK; ++i) {
                int idx = g * kFecK + i;
                if (idx >= (int)cnt) break;
                if (sf.parts[idx].size() > max_len) max_len = sf.parts[idx].size();
            }
            std::vector<uint8_t> parity(max_len, 0);
            for (int i = 0; i < kFecK; ++i) {
                int idx = g * kFecK + i;
                if (idx >= (int)cnt) break;
                const auto& p = sf.parts[idx];
                for (size_t b = 0; b < max_len; ++b) {
                    uint8_t v = (b < p.size()) ? p[b] : 0;
                    parity[b] ^= v;
                }
            }
            sf.parity[g] = parity;
            std::vector<uint8_t> pkt(kHdr + parity.size());
            write_hdr(pkt.data(), mid, 0, cnt, kTypeFec, flags, (uint16_t)g);
            if (!parity.empty()) memcpy(pkt.data() + kHdr, parity.data(), parity.size());
            if (!send_to(peer, pkt.data(), pkt.size())) return false;
        }
    }

    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_send_ring[g_send_ring_pos % kSendRing] = std::move(sf);
        g_send_ring_pos++;
    }

    uint32_t n = g_udp_send_ok.fetch_add(1) + 1;
    if (type == 1 && (n <= 3 || n % 120 == 0)) {
        LOG("peer", "UDP H264 send #%u bytes=%zu frags=%u fec=%d", n, len, (unsigned)cnt, (int)use_fec);
    }
    return true;
}

uint32_t peer_udp_reasm_timeouts() { return g_reasm_timeouts.load(); }
uint32_t peer_udp_nack_sent() { return g_nack_sent.load(); }
uint32_t peer_udp_fec_recovered() { return g_fec_recovered.load(); }

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
