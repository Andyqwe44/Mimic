/**
 * Frame Preprocessing Pipeline
 */
#include "preprocess.hpp"
#include <cstring>
#include <algorithm>

FramePreprocessor::FramePreprocessor(const PreprocessConfig& cfg)
    : cfg_(cfg) {
    // Ring buffer: frame_stack * H * W floats
    ring_buffer_.resize(cfg_.frame_stack * cfg_.target_height * cfg_.target_width);
}

void FramePreprocessor::reset() {
    frame_count_ = 0;
    write_pos_ = 0;
    std::fill(ring_buffer_.begin(), ring_buffer_.end(), 0.0f);
}

bool FramePreprocessor::process(const FrameBuffer& frame, float* tensor_out) {
    int H = cfg_.target_height;
    int W = cfg_.target_width;
    int frame_size = H * W;

    // Resize + grayscale current frame into ring buffer
    float* slot = ring_buffer_.data() + write_pos_ * frame_size;
    resize_bilinear_grayscale(frame.data.data(), frame.width, frame.height,
                               frame.channels, frame.width * frame.channels,
                               slot);

    // Update ring buffer position
    write_pos_ = (write_pos_ + 1) % cfg_.frame_stack;
    if (frame_count_ < cfg_.frame_stack) frame_count_++;

    // Write output in temporal order (oldest first)
    if (tensor_out) {
        // Output: (C, H, W) format for ONNX
        if (frame_count_ < cfg_.frame_stack) {
            // Not full yet: duplicate last frame to fill
            for (int c = 0; c < cfg_.frame_stack; c++) {
                int src_idx = (c < frame_count_) ? c : (frame_count_ - 1);
                int ring_idx = (write_pos_ - frame_count_ + src_idx + cfg_.frame_stack)
                               % cfg_.frame_stack;
                memcpy(tensor_out + c * frame_size,
                       ring_buffer_.data() + ring_idx * frame_size,
                       frame_size * sizeof(float));
            }
        } else {
            // Ring buffer is full: output in order
            for (int c = 0; c < cfg_.frame_stack; c++) {
                int ring_idx = (write_pos_ + c) % cfg_.frame_stack;
                memcpy(tensor_out + c * frame_size,
                       ring_buffer_.data() + ring_idx * frame_size,
                       frame_size * sizeof(float));
            }
        }
    }

    return frame_count_ >= cfg_.frame_stack;
}

/**
 * Bilinear resize + grayscale in one pass.
 * src_stride = bytes per row (width * channels, may include padding)
 */
void FramePreprocessor::resize_bilinear_grayscale(
    const uint8_t* src, int src_w, int src_h,
    int src_channels, int src_stride,
    float* dst) {

    int dst_w = cfg_.target_width;
    int dst_h = cfg_.target_height;
    float scale_x = (float)src_w / (float)dst_w;
    float scale_y = (float)src_h / (float)dst_h;

    int channels = src_channels >= 3 ? src_channels : 1;

    for (int dy = 0; dy < dst_h; dy++) {
        float sy = (dy + 0.5f) * scale_y - 0.5f;
        int y0 = (int)sy;
        int y1 = y0 + 1;
        float fy = sy - (float)y0;

        // Clamp
        y0 = std::max(0, std::min(y0, src_h - 1));
        y1 = std::max(0, std::min(y1, src_h - 1));

        for (int dx = 0; dx < dst_w; dx++) {
            float sx = (dx + 0.5f) * scale_x - 0.5f;
            int x0 = (int)sx;
            int x1 = x0 + 1;
            float fx = sx - (float)x0;

            x0 = std::max(0, std::min(x0, src_w - 1));
            x1 = std::max(0, std::min(x1, src_w - 1));

            // Sample 4 corners, compute grayscale, then bilinear
            const uint8_t* p00 = src + y0 * src_stride + x0 * channels;
            const uint8_t* p01 = src + y0 * src_stride + x1 * channels;
            const uint8_t* p10 = src + y1 * src_stride + x0 * channels;
            const uint8_t* p11 = src + y1 * src_stride + x1 * channels;

            float gray00, gray01, gray10, gray11;
            if (channels == 1) {
                gray00 = *p00;
                gray01 = *p01;
                gray10 = *p10;
                gray11 = *p11;
            } else {
                // ITU-R BT.601 grayscale (BGRA: p[0]=B,p[1]=G,p[2]=R)
                gray00 = 0.299f * p00[2] + 0.587f * p00[1] + 0.114f * p00[0];
                gray01 = 0.299f * p01[2] + 0.587f * p01[1] + 0.114f * p01[0];
                gray10 = 0.299f * p10[2] + 0.587f * p10[1] + 0.114f * p10[0];
                gray11 = 0.299f * p11[2] + 0.587f * p11[1] + 0.114f * p11[0];
            }

            // Bilinear interpolation
            float top = gray00 + (gray01 - gray00) * fx;
            float bottom = gray10 + (gray11 - gray10) * fx;
            float pixel = top + (bottom - top) * fy;

            // Normalize and store
            dst[dy * dst_w + dx] = pixel * cfg_.scale;
        }
    }
}
