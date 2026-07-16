/**
 * h264_encoder.h — Low-latency H.264 via Media Foundation MFT (GPU when available).
 *
 * Input: BGRA frames from WGC CPU readback.
 * Output: Annex-B NAL units suitable for PAYLOAD_TYPE_H264_STREAM over TCP.
 */
#pragma once
#include <cstdint>
#include <vector>

struct H264Packet {
    std::vector<uint8_t> annexb; // start-code prefixed NALs for this encode call
    bool keyframe = false;
    int w = 0;
    int h = 0;
};

class H264Encoder {
public:
    H264Encoder();
    ~H264Encoder();

    H264Encoder(const H264Encoder&) = delete;
    H264Encoder& operator=(const H264Encoder&) = delete;

    // bitrate_kbps default ~4 Mbps; fps hint for rate control.
    bool init(int width, int height, int fps = 60, int bitrate_kbps = 4000);
    void shutdown();
    bool ready() const { return ready_; }

    // Encode one BGRA frame (src_w/src_h = buffer size; even crop used for NV12).
    // May produce 0..N packets. Returns false on hard failure (caller may fall back).
    bool encode_bgra(const uint8_t* bgra, int src_w, int src_h, std::vector<H264Packet>& out);

private:
    struct Impl;
    Impl* impl_ = nullptr;
    bool ready_ = false;
    int w_ = 0;
    int h_ = 0;
};
