/**
 * Screen Capture Module - Abstract Interface
 *
 * Pluggable backends:
 *   - DXGI Desktop Duplication (primary, 1-2ms, GPU-accelerated)
 *   - GDI BitBlt (fallback, 5-10ms, pure CPU)
 */
#pragma once
#include <cstdint>
#include <vector>
#include <memory>
#include "../../common/include/types.hpp"

struct FrameBuffer {
    int width = 0;
    int height = 0;
    int channels = 0;   // 3=BGR, 4=BGRA
    std::vector<uint8_t> data;
    uint64_t timestamp_us = 0;
};

/// Configuration for DXGI capture backend
struct DxgiOptions {
    bool skip_virtual_adapters = false;   // skip "Virtual"/"Remote"/"Indirect"
    bool skip_solid_outputs = false;      // skip outputs returning solid-color frames
    int  min_output_width  = 0;           // 0 = no minimum
    int  min_output_height = 0;
};

class ICaptureBackend {
public:
    virtual ~ICaptureBackend() = default;

    /** One-time initialization. Returns false if backend unavailable. */
    virtual bool init() = 0;

    /** Capture current screen region into out. Returns false on error. */
    virtual bool capture(FrameBuffer& out, const Rect* region = nullptr) = 0;

    /** Find window by title and return its bounding rect. */
    virtual bool get_window_rect(const wchar_t* title, Rect& out) = 0;

    /** Backend name for logging. */
    virtual const char* name() const = 0;

    /** Release all resources. */
    virtual void shutdown() = 0;
};

/**
 * Factory: auto-select best available backend.
 * Tries DXGI first, falls back to GDI.
 */
std::unique_ptr<ICaptureBackend> create_capture_backend();

/** Factory with DXGI options (skip virtual adapters, solid-output detection, etc.) */
std::unique_ptr<ICaptureBackend> create_capture_backend(const DxgiOptions& opts);

/** High-precision timestamp in microseconds */
uint64_t capture_now_us();

/** Clamp capture region to screen bounds. Returns false if region is invalid. */
bool clamp_region(int& x, int& y, int& w, int& h, int limit_w, int limit_h);

/** Find window by title (exact or partial match). */
bool find_window_rect(const wchar_t* title, Rect& out);
