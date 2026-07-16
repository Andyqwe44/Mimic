/**
 * h264_encoder.h — Low-latency H.264 via Media Foundation (GPU DXGI preferred).
 *
 * Prefer: ID3D11Texture2D (WGC BGRA) → hardware MFT → Annex-B NALs.
 * Fallback: CPU BGRA upload / software MFT (LOG_WARN, never silent).
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
    bool encode_texture(ID3D11Texture2D* bgra_tex, int src_w, int src_h,
                        std::vector<H264Packet>& out);

    /// Upload CPU BGRA then encode.
    bool encode_bgra(const uint8_t* bgra, int src_w, int src_h, std::vector<H264Packet>& out);

private:
    struct Impl;
    bool feed_nv12_and_drain_(std::vector<H264Packet>& out);
    bool drain_output_(std::vector<H264Packet>& out);
    bool process_one_output_(std::vector<H264Packet>& out);
    bool pump_async_(std::vector<H264Packet>& out, int timeout_ms);

    Impl* impl_ = nullptr;
    bool ready_ = false;
    bool hardware_ = false;
    int w_ = 0;
    int h_ = 0;
};
