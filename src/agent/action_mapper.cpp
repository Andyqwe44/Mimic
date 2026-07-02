/**
 * Generic Action Mapper - Implementation
 */
#include "action_mapper.hpp"
#include <cstring>
#include <cstdio>

// ==================== ActionDecoder ====================

std::vector<DecodedAction> ActionDecoder::decode(const std::vector<uint8_t>& raw) const {
    std::vector<DecodedAction> result;
    size_t i = 0;

    while (i < raw.size()) {
        auto token = static_cast<ActionToken>(raw[i++]);
        if (token == ActionToken::NOOP) break;  // end of sequence

        DecodedAction da;
        da.type = token;

        switch (token) {
        case ActionToken::MOUSE_MOVE_ABS:
            if (i + 8 > raw.size()) return result;
            { float xn, yn;
              memcpy(&xn, &raw[i], 4); i += 4;
              memcpy(&yn, &raw[i], 4); i += 4;
              da.x = (int)(xn * screen_w_);
              da.y = (int)(yn * screen_h_); }
            break;

        case ActionToken::MOUSE_MOVE_REL:
            if (i + 8 > raw.size()) return result;
            memcpy(&da.dx, &raw[i], 4); i += 4;
            memcpy(&da.dy, &raw[i], 4); i += 4;
            break;

        case ActionToken::MOUSE_CLICK:
            if (i + 9 > raw.size()) return result;
            { float xn, yn; uint8_t b;
              memcpy(&xn, &raw[i], 4); i += 4;
              memcpy(&yn, &raw[i], 4); i += 4;
              b = raw[i++];
              da.x = (int)(xn * screen_w_);
              da.y = (int)(yn * screen_h_);
              da.btn = b == 1 ? MouseButton::Right : b == 2 ? MouseButton::Middle : MouseButton::Left; }
            break;

        case ActionToken::MOUSE_DOWN:
        case ActionToken::MOUSE_UP:
            if (i >= raw.size()) return result;
            { uint8_t b = raw[i++];
              da.btn = b == 1 ? MouseButton::Right : b == 2 ? MouseButton::Middle : MouseButton::Left; }
            break;

        case ActionToken::KEY_PRESS:
        case ActionToken::KEY_RELEASE:
            if (i + 2 > raw.size()) return result;
            da.vk_code = (uint16_t)((raw[i] << 8) | raw[i+1]);
            i += 2;
            break;

        case ActionToken::KEY_TAP:
            if (i + 6 > raw.size()) return result;
            da.vk_code = (uint16_t)((raw[i] << 8) | raw[i+1]); i += 2;
            memcpy(&da.duration_ms, &raw[i], 4); i += 4;
            break;

        case ActionToken::WAIT:
            if (i + 4 > raw.size()) return result;
            memcpy(&da.duration_ms, &raw[i], 4); i += 4;
            break;

        case ActionToken::SCROLL:
            if (i + 4 > raw.size()) return result;
            memcpy(&da.scroll_delta, &raw[i], 4); i += 4;
            break;

        default:
            break;
        }
        result.push_back(da);
    }
    return result;
}

GameAction ActionDecoder::to_game_action(const DecodedAction& da) {
    switch (da.type) {
    case ActionToken::MOUSE_MOVE_ABS:
        return GameAction::move_to(da.x, da.y);
    case ActionToken::MOUSE_MOVE_REL:
        return GameAction::move_rel(da.dx, da.dy);
    case ActionToken::MOUSE_CLICK:
        return GameAction::click_at(da.x, da.y, da.btn);
    case ActionToken::MOUSE_DOWN:
        return GameAction::btn_down(da.btn);
    case ActionToken::MOUSE_UP:
        return GameAction::btn_up(da.btn);
    case ActionToken::KEY_PRESS:
        return GameAction::key_down(da.vk_code);
    case ActionToken::KEY_RELEASE:
        return GameAction::key_up(da.vk_code);
    case ActionToken::KEY_TAP:
        return GameAction::key_tap(da.vk_code, da.duration_ms);
    case ActionToken::WAIT:
        return GameAction::wait_for(da.duration_ms);
    default:
        return GameAction::wait_for(0);
    }
}

std::vector<GameAction> ActionDecoder::to_game_actions(const std::vector<DecodedAction>& decoded) {
    std::vector<GameAction> result;
    result.reserve(decoded.size());
    for (auto& da : decoded)
        result.push_back(to_game_action(da));
    return result;
}

// ==================== GenericActionMapper ====================

bool GenericActionMapper::execute(const std::vector<DecodedAction>& actions) {
    for (auto& da : actions)
        if (!execute_one(da)) return false;
    return true;
}

bool GenericActionMapper::execute_one(const DecodedAction& action) {
    GameAction ga = ActionDecoder::to_game_action(action);
    return backend_->send_action(ga);
}
