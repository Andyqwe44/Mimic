/**
 * h264_encoder.h — Low-latency H.264 via Media Foundation (GPU DXGI preferred).
 *
 * Prefer: ID3D11Texture2D (WGC BGRA) → hardware MFT → Annex-B NALs.
 * Fallback: CPU BGRA upload / software MFT (LOG_WARN, never silent).
 *
 * Phase-1 correctness: 3-slot BGRA/NV12 ring with MFT sample ownership.
 * MFT_IN_FLIGHT slots are never GPU-written; FREE only on proven sample Release.
 */
#pragma once
#include <cstdint>
#include <vector>

struct ID3D11Device;
struct ID3D11Texture2D;
struct IMFSample;

struct H264Packet {
    std::vector<uint8_t> annexb;
    bool keyframe = false;
    int w = 0;
    int h = 0;
    /// Capture/encode stamp (GetTickCount64 low 32) for glass-to-glass latency.
    uint32_t ts_ms = 0;
    /// Monotonic send-side sequence (also packed into flags[16..31] on the wire).
    uint32_t seq = 0;
};

class H264Encoder {
public:
    H264Encoder();
    ~H264Encoder();

    H264Encoder(const H264Encoder&) = delete;
    H264Encoder& operator=(const H264Encoder&) = delete;

    /// Bind to an existing D3D11 device (WGC's). Required for zero-copy encode_texture.
    bool init(ID3D11Device* device, int width, int height, int fps = 30, int bitrate_kbps = 6000);
    /// Create a private D3D11 device (upload path).
    bool init(int width, int height, int fps = 30, int bitrate_kbps = 6000);

    void shutdown();
    bool ready() const { return ready_; }
    bool hardware() const { return hardware_; }
    int width() const { return w_; }
    int height() const { return h_; }

    void request_keyframe();

    /// Encode a GPU BGRA texture (same device as init).
    /// capture_id: optional upstream WGC frame id for lifecycle logs (0 = none).
    bool encode_texture(ID3D11Texture2D* bgra_tex, int src_w, int src_h,
                        std::vector<H264Packet>& out, uint32_t capture_id = 0);

    /// Upload CPU BGRA then encode.
    bool encode_bgra(const uint8_t* bgra, int src_w, int src_h, std::vector<H264Packet>& out,
                     uint32_t capture_id = 0);

private:
    struct Impl;
    friend struct TrackedReleaseCb;
    bool feed_nv12_and_drain_(int slot_id, uint32_t capture_id, uint32_t submit_id,
                              std::vector<H264Packet>& out);
    bool drain_output_(std::vector<H264Packet>& out);
    bool process_one_output_(std::vector<H264Packet>& out);
    bool pump_async_(std::vector<H264Packet>& out, int timeout_ms);
    int acquire_free_slot_();
    void release_slot_prep_fail_(int slot_id);
    bool gpu_write_slot_(int slot_id, ID3D11Texture2D* src_bgra, const uint8_t* cpu_bgra,
                         int src_w);
    bool wait_gpu_idle_();

    Impl* impl_ = nullptr;
    bool ready_ = false;
    bool hardware_ = false;
    int w_ = 0;
    int h_ = 0;
};
