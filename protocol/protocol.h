/**
 * protocol/protocol.h — wire format constants shared by C, C++, Rust, Python.
 *
 * This is the single source of truth for what goes over the wire.
 * All other modules (transport, payload) depend on this, never vice versa.
 *
 * Frame on the wire (binary, little-endian):
 *   [magic:4][payload_size:4][type_tag:4][payload_body: payload_size bytes]
 *
 * magic         = 0x4D415246 ("FRAM" LE) — identifies this protocol
 * payload_size  = byte count of payload_body only (NOT including type_tag)
 * type_tag      = what kind of payload follows (see PayloadType enum)
 * payload_body  = type-specific data, exactly payload_size bytes
 *
 * Keep in sync with: protocol/protocol.rs, protocol/protocol.py
 */
#pragma once
#include <cstdint>

// ═══════════════════════════════════════════════════════════
// Wire format constants
// ═══════════════════════════════════════════════════════════

#define PROTOCOL_MAGIC         0x4D415246u  // "FRAM" LE
#define PROTOCOL_FRAME_HEADER  12u          // magic(4) + size(4) + type_tag(4)
#define PROTOCOL_TYPE_OFFSET   8u           // type_tag starts at byte 8

// ═══════════════════════════════════════════════════════════
// Payload type tags — add new types here
// ═══════════════════════════════════════════════════════════

typedef enum {
    PAYLOAD_TYPE_NONE         = 0,   // unchanged / heartbeat
    PAYLOAD_TYPE_BGRA_FRAME   = 1,   // BGRA: [w:4][h:4][ch:4][reserved:4][pixels...] (fallback only)
    PAYLOAD_TYPE_H264_STREAM  = 2,   // H.264 Annex-B: [w:4][h:4][flags:4][reserved:4][nal...]
                                     // flags bit0 = keyframe. Preferred remote video path.
    PAYLOAD_TYPE_CONTROL_MSG  = 3,   // JSON action (mousedown/up/move/key/text…). Agent fills hwnd/method.
    PAYLOAD_TYPE_CAPABILITIES = 4,   // capability bitmask (future)
} PayloadType;

// ═══════════════════════════════════════════════════════════
// Network / pipe defaults
// ═══════════════════════════════════════════════════════════

#define PROTOCOL_DEFAULT_TCP_PORT  9999u
#define PROTOCOL_DEFAULT_PIPE_NAME "tictactoe_stream"

// ═══════════════════════════════════════════════════════════
// Helper: build a frame header [magic:4][size:4][type:4]
// ═══════════════════════════════════════════════════════════

inline void protocol_build_header(uint8_t out[12], uint32_t payload_size, uint32_t type_tag) {
    out[0]  = (uint8_t)(PROTOCOL_MAGIC & 0xFF);
    out[1]  = (uint8_t)((PROTOCOL_MAGIC >> 8) & 0xFF);
    out[2]  = (uint8_t)((PROTOCOL_MAGIC >> 16) & 0xFF);
    out[3]  = (uint8_t)((PROTOCOL_MAGIC >> 24) & 0xFF);
    out[4]  = (uint8_t)(payload_size & 0xFF);
    out[5]  = (uint8_t)((payload_size >> 8) & 0xFF);
    out[6]  = (uint8_t)((payload_size >> 16) & 0xFF);
    out[7]  = (uint8_t)((payload_size >> 24) & 0xFF);
    out[8]  = (uint8_t)(type_tag & 0xFF);
    out[9]  = (uint8_t)((type_tag >> 8) & 0xFF);
    out[10] = (uint8_t)((type_tag >> 16) & 0xFF);
    out[11] = (uint8_t)((type_tag >> 24) & 0xFF);
}

// ═══════════════════════════════════════════════════════════
// Helper: parse a frame header → (ok, payload_size, type_tag)
// ═══════════════════════════════════════════════════════════

inline bool protocol_parse_header(const uint8_t hdr[12], uint32_t& payload_size, uint32_t& type_tag) {
    uint32_t magic = (uint32_t)hdr[0] | ((uint32_t)hdr[1]<<8)
                   | ((uint32_t)hdr[2]<<16) | ((uint32_t)hdr[3]<<24);
    if (magic != PROTOCOL_MAGIC) return false;
    payload_size = (uint32_t)hdr[4] | ((uint32_t)hdr[5]<<8)
                 | ((uint32_t)hdr[6]<<16) | ((uint32_t)hdr[7]<<24);
    type_tag     = (uint32_t)hdr[8] | ((uint32_t)hdr[9]<<8)
                 | ((uint32_t)hdr[10]<<16) | ((uint32_t)hdr[11]<<24);
    return true;
}
