/**
 * Minimal WAN candidate exchange over signaling (STUN-ready hook).
 * Full libdatachannel ICE plugs into the same `signal` messages:
 *   {kind:"ice_offer", sdp} / {kind:"ice_answer", sdp} / {kind:"ice_cand", cand}
 * Until linked, LAN path is preferred; WAN reports clear failure (no server relay).
 */
#pragma once
#include <string>

/// Build a wan_probe payload announcing willingness to try ICE (placeholder).
inline std::string peer_wan_probe_payload() {
    return R"({"kind":"wan_probe","proto":"ice-datachannel","ver":1})";
}
