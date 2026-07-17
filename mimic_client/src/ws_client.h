/**
 * ws_client.h — Outbound WebSocket client (GAM agent → controller_server).
 *
 * Binary: H.264 meta+Annex-B (same layout as former embedded ws_server).
 * Text: JSON control / config / need_key from server; status to server.
 */
#pragma once
#include <cstdint>
#include <functional>
#include <string>

struct H264Packet;

using WsClientTextFn = std::function<void(const std::string& json)>;
using WsClientClosedFn = std::function<void()>;

bool ws_client_connect(const std::string& host, uint16_t port);
void ws_client_disconnect();
bool ws_client_connected();
void ws_client_set_handlers(WsClientTextFn on_text, WsClientClosedFn on_closed);
void ws_client_send_h264(const H264Packet& pkt);
bool ws_client_send_text(const std::string& json);
