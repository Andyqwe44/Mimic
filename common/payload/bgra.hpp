/**
 * common/payload/bgra.hpp — BGRA pixel frame payload (type_tag = PAYLOAD_TYPE_BGRA_FRAME).
 *
 * Depends on: protocol/protocol.h (for PayloadType, PROTOCOL_FRAME_HEADER).
 * Does NOT depend on transport.
 *
 * Payload body format:
 *   [w:4 LE][h:4 LE][ch:4 LE][reserved:4][pixels: w*h*ch bytes]
 *
 * Usage (pack):
 *   auto payload = bgra_pack(pixels, w, h, 4);
 *   // payload ready to hand to transport
 *
 * Usage (unpack):
 *   auto frame = bgra_unpack(payload);
 *   // frame.w, frame.h, frame.ch, frame.pixels
 */
#pragma once
#include <cstdint>
#include <vector>
#include <cstring>

namespace payload {

struct BgraFrame {
    uint32_t width;
    uint32_t height;
    uint32_t channels;
    std::vector<uint8_t> pixels;
};

constexpr uint32_t HEADER_SIZE = 16u;  // w(4)+h(4)+ch(4)+reserved(4)

/// Pack BGRA pixels → payload bytes.
inline std::vector<uint8_t> bgra_pack(const uint8_t* pixels, uint32_t w, uint32_t h, uint32_t ch) {
    std::vector<uint8_t> out(HEADER_SIZE + w * h * ch);
    auto w32 = [&](size_t off, uint32_t v) {
        out[off] = (uint8_t)v; out[off+1] = (uint8_t)(v>>8);
        out[off+2] = (uint8_t)(v>>16); out[off+3] = (uint8_t)(v>>24);
    };
    w32(0, w); w32(4, h); w32(8, ch); w32(12, 0);
    memcpy(out.data() + HEADER_SIZE, pixels, w * h * ch);
    return out;
}

inline std::vector<uint8_t> bgra_pack(const std::vector<uint8_t>& pixels, uint32_t w, uint32_t h, uint32_t ch) {
    return bgra_pack(pixels.data(), w, h, ch);
}

/// Unpack payload bytes → BgraFrame.
/// Returns empty frame (width=0) on invalid/malformed input.
inline BgraFrame bgra_unpack(const uint8_t* payload, size_t payload_size) {
    BgraFrame f = {};
    if (payload_size < HEADER_SIZE) return f;
    auto r32 = [](const uint8_t* p) -> uint32_t { return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24); };
    f.width    = r32(payload);
    f.height   = r32(payload + 4);
    f.channels = r32(payload + 8);
    // Validate: non-zero, reasonable dimensions, no overflow
    if (f.width == 0 || f.height == 0 || f.channels == 0) return BgraFrame{};
    if (f.width > 16384 || f.height > 16384 || f.channels > 16) return BgraFrame{};
    // Check w*h*ch overflow before multiplication
    uint64_t px_u64 = (uint64_t)f.width * f.height * f.channels;
    if (px_u64 > (uint64_t)SIZE_MAX || px_u64 > 1024ULL*1024*1024) return BgraFrame{};
    size_t px_size = (size_t)px_u64;
    if (payload_size >= HEADER_SIZE + px_size) {
        f.pixels.assign(payload + HEADER_SIZE, payload + HEADER_SIZE + px_size);
    }
    return f;
}

inline BgraFrame bgra_unpack(const std::vector<uint8_t>& payload) {
    return bgra_unpack(payload.data(), payload.size());
}

} // namespace payload
