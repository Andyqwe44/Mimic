/**
 * Stream Protocol — shared constants across C++, Rust, Python.
 *
 * Frame format (binary, little-endian):
 *   [magic:4 "FRAM"][w:4][h:4][ch:4][size:4][pixels: size bytes]
 *
 * magic = 0x4D415246 ("FRAM" in LE)
 * w, h  = frame dimensions (pixels)
 * ch    = color channels (4 = BGRA)
 * size  = w * h * ch bytes of raw pixel data
 *
 * Keep in sync with:
 *   monitor_web/src-tauri/src/stream_protocol.rs
 *   model/stream_protocol.py
 */
#pragma once
#include <cstdint>

namespace stream_protocol {

// ── Network ─────────────────────────────────────────────
constexpr uint16_t DEFAULT_TCP_PORT   = 9999;
constexpr char     DEFAULT_HOST[]     = "127.0.0.1";

// ── Pipe ────────────────────────────────────────────────
constexpr char     DEFAULT_PIPE_NAME[] = "tictactoe_stream";  // → \\.\pipe\tictactoe_stream

// ── Frame format constants ──────────────────────────────
constexpr uint32_t FRAME_MAGIC        = 0x4D415246;  // "FRAM" LE
constexpr uint32_t FRAME_MAGIC_OFFSET = 0;
constexpr uint32_t FRAME_WIDTH_OFFSET = 4;
constexpr uint32_t FRAME_HEIGHT_OFFSET= 8;
constexpr uint32_t FRAME_CH_OFFSET    = 12;
constexpr uint32_t FRAME_SIZE_OFFSET  = 16;
constexpr uint32_t FRAME_HEADER_SIZE  = 20;          // 5 × uint32_t
constexpr uint32_t FRAME_CH_BGRA      = 4;

// ── Pixel scaling ───────────────────────────────────────
constexpr int      MAX_FRAME_DIM      = 640;          // max width/height after scale

// ── Capabilities (bitmask) ──────────────────────────────
enum Capability : uint32_t {
    CAP_NONE        = 0,
    CAP_BGRA_RAW    = 1 << 0,   // raw BGRA pixels
    CAP_H264_STREAM = 1 << 1,   // H.264 NAL units (future)
    CAP_DESKTOP     = 1 << 2,   // can capture desktop
    CAP_WINDOW      = 1 << 3,   // can capture specific windows
};

// ── Helper: build frame header into buffer[20] ──────────
inline void build_frame_header(uint8_t* hdr, uint32_t w, uint32_t h, uint32_t ch, uint32_t size) {
    auto w32 = [&](uint32_t v) { *hdr++ = (uint8_t)v; *hdr++ = (uint8_t)(v>>8); *hdr++ = (uint8_t)(v>>16); *hdr++ = (uint8_t)(v>>24); };
    w32(FRAME_MAGIC); w32(w); w32(h); w32(ch); w32(size);
}

// ── Helper: parse frame header from buffer[20] ──────────
inline void parse_frame_header(const uint8_t* hdr, uint32_t& magic, uint32_t& w, uint32_t& h, uint32_t& ch, uint32_t& size) {
    auto r32 = [&](const uint8_t* p) -> uint32_t { return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24); };
    magic = r32(hdr); w = r32(hdr+4); h = r32(hdr+8); ch = r32(hdr+12); size = r32(hdr+16);
}

} // namespace stream_protocol
