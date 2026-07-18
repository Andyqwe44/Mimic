/**
 * Frame Preprocessing Pipeline
 *
 * FrameBuffer (BGR/BGRA, arbitrary resolution)
 *   -> Crop to game window region
 *   -> Bilinear resize to 84x84
 *   -> Grayscale conversion
 *   -> Normalize to [0,1] float32
 *   -> 4-frame stack
 *
 * Output: float32 tensor (channels, height, width) = (4, 84, 84)
 */
#pragma once
#include "capture.hpp"
#include <cstdint>
#include <vector>

/** Configuration for the preprocessor */
struct PreprocessConfig {
    int target_width = 84;
    int target_height = 84;
    int frame_stack = 4;        // number of frames to stack
    float scale = 1.0f / 255.0f; // normalization factor
};

class FramePreprocessor {
public:
    explicit FramePreprocessor(const PreprocessConfig& cfg = {});
    ~FramePreprocessor() = default;

    /**
     * Process a raw frame and add it to the frame stack.
     * @param frame      raw captured frame
     * @param tensor_out output tensor of shape (frame_stack, target_h, target_w)
     *                   pre-allocated: frame_stack * target_h * target_w floats
     * @return true if the stack is full (ready for inference)
     */
    bool process(const FrameBuffer& frame, float* tensor_out);

    /** Check if enough frames have been collected */
    bool is_ready() const { return frame_count_ >= cfg_.frame_stack; }

    /** Reset the frame stack */
    void reset();

    /** Get current frame count in the ring buffer */
    int frame_count() const { return frame_count_; }

private:
    void resize_bilinear_grayscale(const uint8_t* src, int src_w, int src_h,
                                    int src_channels, int src_stride,
                                    float* dst);
    PreprocessConfig cfg_;
    std::vector<float> ring_buffer_; // circular buffer for frame stack
    int frame_count_ = 0;
    int write_pos_ = 0;
};
