/**
 * peer_session.h — Peer control sessions via Mimic signaling + LAN/P2P media.
 *
 * Phase1/2: signaling WSS/WS + LAN TCP direct for H.264 + JSON control.
 * WAN ICE (libdatachannel) can plug into the same PeerMediaSink later.
 */
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct H264Packet;

enum class PeerRole { Idle, Outgoing, Ringing, Controller, Controlled };

struct PeerDevice {
    std::string deviceId;
    std::string deviceName;
    std::vector<std::string> lanIps;
};

struct PeerSessionInfo {
    std::string controllerId;
    std::string controlledId;
};

using PeerEventFn = std::function<void(const std::string& json)>; // push to WebView UI
using PeerControlFn = std::function<void(const std::string& actionJson)>;
using PeerNeedKeyFn = std::function<void()>;
using PeerListWindowsFn = std::function<std::string()>; // JSON array
using PeerSetTargetFn = std::function<std::string(uint64_t hwnd)>; // result JSON

struct PeerCallbacks {
    PeerEventFn on_ui_event;
    PeerControlFn on_control;       // Controlled: apply input
    PeerNeedKeyFn on_need_key;
    PeerListWindowsFn on_list_windows;
    PeerSetTargetFn on_set_target;
};

bool peer_init(PeerCallbacks cb);
void peer_shutdown();

/// HTTP login to signaling; then open WS. signaling_url e.g. http://127.0.0.1:8443
std::string peer_login(const std::string& signaling_url,
                       const std::string& user,
                       const std::string& password,
                       const std::string& device_name);

std::string peer_register(const std::string& signaling_url,
                          const std::string& user,
                          const std::string& password);

void peer_logout();
bool peer_online();
PeerRole peer_role();
std::string peer_status_json();

std::string peer_invite(const std::string& target_device_id);
std::string peer_accept(const std::string& from_device_id);
std::string peer_reject(const std::string& from_device_id);
std::string peer_hangup();

/// Controller: request remote window list / set target / send control / human|ai mode
std::string peer_request_windows();
std::string peer_set_remote_target(uint64_t hwnd, const std::string& title);
std::string peer_send_control(const std::string& action_json);
std::string peer_request_keyframe(); // controller → controlled need_key
std::string peer_set_control_mode(const std::string& mode); // human|ai

/// Controlled / either: push H.264 when in session as Controlled (or when streaming)
void peer_send_h264(const H264Packet& pkt);
bool peer_media_ready();
/// "lan" | "p2p" | "none"
std::string peer_transport_mode();

/// Take last received H.264 frame payload (16-byte meta + annexb). Clears buffer.
std::string peer_take_last_frame(std::vector<uint8_t>& out);