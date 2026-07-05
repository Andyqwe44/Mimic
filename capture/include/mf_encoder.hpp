/**
 * Media Foundation H.264 Hardware Encoder Wrapper
 *
 * GPU pipeline: BGRA texture → BGRA→NV12 (CPU) → MF HW encoder → H.264 NAL units
 * CPU NV12 conversion is ~0.3ms at 640px. GPU compute shader TODO for zero-copy.
 *
 * Encoder config: low-latency (no B-frames), baseline profile, CBR-ish.
 * Thread-safe: one encode() at a time (single producer).
 */
#pragma once
#include <cstdint>
#include <vector>
#include <d3d11.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

class MfH264Encoder {
public:
    MfH264Encoder();
    ~MfH264Encoder();

    /** One-time init. device must outlive encoder. */
    bool init(ComPtr<ID3D11Device> device, int width, int height,
              int fps = 60, int bitrate = 5000000);

    /**
     * Encode one BGRA frame → H.264 NAL unit(s).
     * bgra_texture: source BGRA on GPU (from DXGI/FramePool/WGC).
     * h264_output: appended with H.264 NAL units (start codes, not length-prefixed).
     * Returns false if encoder needs more input (no output ready yet — normal for first few frames).
     * Caller should drain encoder with encode(nullptr) before shutdown.
     */
    bool encode(ComPtr<ID3D11Texture2D> bgra_texture, std::vector<uint8_t>& h264_output);

    /** Force IDR keyframe on next encode(). */
    void request_keyframe();

    /** Drain remaining frames. Pass nullptr as texture repeatedly until returns false. */
    bool drain(std::vector<uint8_t>& h264_output);

    void shutdown();

    int width() const { return width_; }
    int height() const { return height_; }

private:
    bool find_hardware_encoder();
    void configure_encoder(int width, int height, int fps, int bitrate);
    bool set_input_type(int width, int height, int fps);
    bool set_output_type();
    bool create_resources(int width, int height);
    void bgra_to_nv12_cpu(const uint8_t* bgra, int width, int height,
                          int row_pitch, uint8_t* nv12_y, uint8_t* nv12_uv,
                          int nv12_pitch);

    ComPtr<ID3D11Device>        device_;
    ComPtr<ID3D11DeviceContext> ctx_;
    ComPtr<IMFTransform>        encoder_;

    // NV12 input for encoder (CPU→GPU upload each frame)
    ComPtr<ID3D11Texture2D>     nv12_tex_;
    // Staging for BGRA→CPU read (reused)
    ComPtr<ID3D11Texture2D>     staging_bgra_;
    int  staging_w_ = 0, staging_h_ = 0;

    int width_ = 0, height_ = 0;
    LONGLONG frame_duration_ = 0;    // 100ns units
    LONGLONG frame_pts_ = 0;
    bool need_keyframe_ = true;
    DWORD input_stream_id_ = 0;
    DWORD output_stream_id_ = 0;

    bool initialized_ = false;
};
