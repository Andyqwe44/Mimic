/**
 * mjpeg_server.h — MJPEG HTTP streaming server.
 *
 * Replaces monitor_web/src-tauri/src/mjpeg_server.rs.
 * Uses Winsock2 + WIC for JPEG encoding on port 9998.
 */
#pragma once
#include <cstdint>

bool mjpeg_server_start();
void mjpeg_server_stop();
void mjpeg_server_push_frame(const uint8_t* pixels, int w, int h);
bool mjpeg_server_has_clients();
