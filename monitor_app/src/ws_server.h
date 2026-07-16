/**
 * ws_server.h — Embedded HTTP + WebSocket on :9997 (controller UI + H.264).
 *
 * Binary frames: [w:4][h:4][flags:4][reserved:4][Annex-B…]
 * Text frames: control JSON → on_control callback.
 */
#pragma once
#include <cstdint>
#include <functional>
#include <string>

struct H264Packet;

using WsControlFn = std::function<void(const std::string& json)>;
using WsNeedKeyFn = std::function<void()>;

bool ws_server_start(const std::string& static_root, WsControlFn on_control, WsNeedKeyFn on_need_key);
void ws_server_stop();
bool ws_server_has_clients();
void ws_broadcast_h264(const H264Packet& pkt);
