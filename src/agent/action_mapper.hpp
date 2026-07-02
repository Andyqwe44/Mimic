/**
 * Generic Action Mapper
 *
 * Maps model output tokens to physical input events.
 * The model outputs tokens like: MOVE(0.3,0.5), CLICK_LEFT, KEY_PRESS(W), WAIT(100)
 * These are screen-normalized coordinates (0..1) for spatial actions,
 * and virtual key codes for keyboard actions.
 *
 * Game-agnostic: no game-specific knowledge. Pure token → OS event translation.
 */
#pragma once
#include "../input/input.hpp"
#include <vector>
#include <string>

// --- Action Token Types ---
// These are what the model outputs (discrete tokens + continuous params)

enum class ActionToken : uint8_t {
    MOUSE_MOVE_ABS = 0,   // + float x_norm, float y_norm (0..1 normalized coords)
    MOUSE_MOVE_REL = 1,   // + int dx, int dy (pixel deltas)
    MOUSE_CLICK    = 2,   // + float x_norm, float y_norm, uint8 btn (0=L,1=R,2=M)
    MOUSE_DOWN     = 3,   // + uint8 btn
    MOUSE_UP       = 4,   // + uint8 btn
    KEY_PRESS      = 5,   // + uint16 vk_code
    KEY_RELEASE    = 6,   // + uint16 vk_code
    KEY_TAP        = 7,   // + uint16 vk_code, int duration_ms
    WAIT           = 8,   // + int ms
    SCROLL         = 9,   // + int delta
    NOOP           = 255, // padding token (no action)
};

// --- Decoded Action ---
// Parsed from token stream, ready for IInputBackend
struct DecodedAction {
    ActionToken type = ActionToken::NOOP;
    int x = 0, y = 0;          // absolute pixel coords (after denormalization)
    int dx = 0, dy = 0;        // relative deltas
    MouseButton btn = MouseButton::Left;
    uint16_t vk_code = 0;
    int duration_ms = 0;
    int scroll_delta = 0;
};

// --- Token → DecodedAction ---

/**
 * Decode a stream of action tokens + parameters into a sequence of
 * physical input actions.
 *
 * Token format (compact, for model output):
 *   Each action = 1 byte token_type + variable params
 *   MOUSE_MOVE_ABS: [token=0][x_norm_float][y_norm_float]  = 9 bytes
 *   MOUSE_CLICK:    [token=2][x_norm_float][y_norm_float][btn_byte] = 10 bytes
 *   KEY_PRESS:      [token=5][vk_hi][vk_lo] = 3 bytes
 *   WAIT:           [token=8][ms_hi][ms_lo]  = 3 bytes
 *   NOOP:           [token=255]               = 1 byte
 *
 * Model outputs a fixed-length token sequence (e.g., 32 tokens = 32*4 bytes = 128B).
 * Tokens after first NOOP are ignored.
 */
class ActionDecoder {
public:
    ActionDecoder(int screen_w, int screen_h)
        : screen_w_(screen_w), screen_h_(screen_h) {}

    /** Decode raw bytes from model into action list */
    std::vector<DecodedAction> decode(const std::vector<uint8_t>& raw) const;

    /** Convert DecodedAction → GameAction for IInputBackend */
    static GameAction to_game_action(const DecodedAction& da);

    /** Convert entire decoded list to GameAction list */
    static std::vector<GameAction> to_game_actions(const std::vector<DecodedAction>& decoded);

private:
    int screen_w_, screen_h_;
};

// --- High-Level Mapper ---

/**
 * GenericActionMapper: replaces game-specific mappers.
 * Receives decoded actions that already contain absolute pixel coordinates,
 * and passes them directly to the input backend.
 * No game knowledge required.
 */
class GenericActionMapper {
public:
    explicit GenericActionMapper(IInputBackend* backend) : backend_(backend) {}

    /** Execute a sequence of decoded actions */
    bool execute(const std::vector<DecodedAction>& actions);

    /** Execute a single decoded action */
    bool execute_one(const DecodedAction& action);

private:
    IInputBackend* backend_;
};
