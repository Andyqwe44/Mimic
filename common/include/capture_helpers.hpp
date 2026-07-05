/**
 * common/include/capture_helpers.hpp — Shared capture utility functions.
 *
 * These were duplicated across capture_stream.cpp, capture_h264.cpp,
 * and capture_single.cpp. Use from here instead.
 */
#pragma once
#include <cstdint>
#include <vector>
#include <cstring>

namespace capture_helpers {

// ── Little-endian helpers ──────────────────────────────────

inline void w32_le(uint8_t* dst, uint32_t v) {
    dst[0] = (uint8_t)v; dst[1] = (uint8_t)(v>>8);
    dst[2] = (uint8_t)(v>>16); dst[3] = (uint8_t)(v>>24);
}

inline uint32_t r32_le(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}

inline void w64_le(uint8_t* dst, uint64_t v) {
    w32_le(dst, (uint32_t)v); w32_le(dst+4, (uint32_t)(v>>32));
}

// ── BGRA scaling (nearest-neighbor) ───────────────────────

/// Scale BGRA pixels to fit within max_dim, preserving aspect ratio.
/// Returns scaled buffer and output dimensions (sw, sh).
inline std::pair<std::vector<uint8_t>, std::pair<int, int>>
scale_bgra(const uint8_t* src, int w, int h, int max_dim = 640) {
    float scale = (float)max_dim / (float)(w > h ? w : h);
    scale = scale < 1.0f ? scale : 1.0f;
    int sw = (int)(w * scale);
    int sh = (int)(h * scale);
    std::vector<uint8_t> out(sw * sh * 4);
    for (int y = 0; y < sh; y++) {
        int sy = (int)(y / scale);
        for (int x = 0; x < sw; x++) {
            int sx = (int)(x / scale);
            memcpy(&out[(y * sw + x) * 4], &src[(sy * w + sx) * 4], 4);
        }
    }
    return {std::move(out), {sw, sh}};
}

// ── Solid color detection ───────────────────────────────────

/// Sample pixels at ~400 evenly-spaced positions. Returns (sample_count, step).
inline std::pair<int, int> pixel_samples(size_t pixel_count) {
    int step = (int)((pixel_count / 1600) * 4);  // ~400 samples
    step = step < 4 ? 4 : step;
    return {(int)(pixel_count / step), step};
}

/// Returns true if all sampled pixels have the same RGB values as pixel[0].
inline bool is_solid_color(const uint8_t* pixels, size_t len) {
    if (len < 16) return len < 4;
    auto [n, step] = pixel_samples(len);
    uint8_t r0 = pixels[2], g0 = pixels[1], b0 = pixels[0];
    int same = 0;
    for (size_t i = 0; i < len; i += step) {
        if (pixels[i+2] == r0 && pixels[i+1] == g0 && pixels[i] == b0) same++;
    }
    return n > 0 && same == n;
}

// ── Magenta sentinel detection (PrintWindow failure check) ─

/// Returns true if >5% of sampled pixels are magenta (R=255,G=0,B=255),
/// indicating PrintWindow failed to render content.
inline bool has_magenta_sentinel(const uint8_t* pixels, size_t len) {
    if (len < 16) return false;
    auto [n, step] = pixel_samples(len);
    int magenta = 0;
    for (size_t i = 0; i < len; i += step) {
        if (pixels[i+2] == 255 && pixels[i+1] == 0 && pixels[i] == 255) magenta++;
    }
    return n > 0 && magenta * 20 > n;  // >5%
}

// ── Frame differ (byte-level equality) ─────────────────────

/// Returns true if two buffers are identical.
inline bool frames_equal(const uint8_t* a, const uint8_t* b, size_t len) {
    return memcmp(a, b, len) == 0;
}

} // namespace capture_helpers
