/**
 * peer_udp.h — STUN (srflx) + UDP hole-punch peer media (no TURN).
 * Same payload framing as LAN once a peer is locked:
 *   reassembled stream uses [type:u8][len:u32 LE][payload] semantics via callbacks.
 * On-wire UDP datagrams use MPC1 fragments (see peer_udp.cpp).
 */
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct PeerUdpCand {
    std::string ip;
    uint16_t port = 0;
    std::string typ; // host | srflx
};

using PeerUdpPayloadFn = std::function<void(uint8_t type, const std::vector<uint8_t>& payload)>;
using PeerUdpReadyFn = std::function<void()>;

bool peer_udp_start(const std::string& stun_host, uint16_t stun_port,
                    PeerUdpPayloadFn on_payload, PeerUdpReadyFn on_ready);
void peer_udp_stop();
bool peer_udp_ready();
uint16_t peer_udp_local_port();
std::vector<PeerUdpCand> peer_udp_local_cands();

/// Begin punching toward remote candidates (call after exchanging ice_offer/answer).
void peer_udp_set_remote_cands(const std::vector<PeerUdpCand>& cands);

bool peer_udp_send(uint8_t type, const uint8_t* data, size_t len);

/// JSON array fragment for signaling: [{"ip":"..","port":N,"typ":"srflx"},...]
std::string peer_udp_cands_json(const std::vector<PeerUdpCand>& cands);
bool peer_udp_parse_cands_json(const std::string& json_arr, std::vector<PeerUdpCand>& out);
