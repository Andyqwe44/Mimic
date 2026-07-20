/**
 * peer_udp.h — STUN (srflx) + UDP hole-punch + MPC2 media (FEC + NACK).
 *
 * MPC2 on-wire (16-byte header + chunk):
 *   [magic:4 "MPC2"][frame_id:4][idx:2][cnt:2][type:1][flags:1][fec_g:2][chunk…]
 * Types: 1=H264 2=JSON 3=NACK 4=FEC_PARITY 0xFF=punch
 * flags: bit0=keyframe (media), bit1=has_fec
 * FEC: XOR parity every 4 data fragments; NACK retransmit from ring (≤2).
 * Reassembly budget ~80ms then on_reasm_fail → need_key.
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
/** Fired when an incomplete reassembly is dropped — request a keyframe for type=1. */
using PeerUdpReasmFailFn = std::function<void(uint8_t type)>;

bool peer_udp_start(const std::string& stun_host, uint16_t stun_port,
                    PeerUdpPayloadFn on_payload, PeerUdpReadyFn on_ready,
                    PeerUdpReasmFailFn on_reasm_fail = {});
/// LAN-only: bind + host cands, skip STUN (call before lan_offer).
bool peer_udp_start_lan(PeerUdpPayloadFn on_payload, PeerUdpReadyFn on_ready,
                        PeerUdpReasmFailFn on_reasm_fail = {});
void peer_udp_stop();
bool peer_udp_ready();
uint16_t peer_udp_local_port();
std::vector<PeerUdpCand> peer_udp_local_cands();

/// Begin punching toward remote candidates (call after exchanging ice_offer/answer or lan udpPort).
void peer_udp_set_remote_cands(const std::vector<PeerUdpCand>& cands);

/// Send media/control. flags: bit0=keyframe for type=1.
bool peer_udp_send(uint8_t type, const uint8_t* data, size_t len, uint8_t flags = 0);

/// Incomplete fragment assemblies dropped due to timeout (lost UDP piece).
uint32_t peer_udp_reasm_timeouts();
uint32_t peer_udp_nack_sent();
uint32_t peer_udp_fec_recovered();

/// JSON array fragment for signaling: [{"ip":"..","port":N,"typ":"srflx"},...]
std::string peer_udp_cands_json(const std::vector<PeerUdpCand>& cands);
bool peer_udp_parse_cands_json(const std::string& json_arr, std::vector<PeerUdpCand>& out);
