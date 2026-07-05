/**
 * common/transport/pipe.hpp — send frames over stdout pipe.
 *
 * Depends on: protocol/protocol.h
 * Does NOT depend on payload.
 *
 * Usage:
 *   transport::PipeSender pipe;
 *   pipe.send(type_tag, payload_data, payload_size);
 */
#pragma once
#include <cstdint>
#include <cstdio>
#include <vector>
#include <io.h>
#include <fcntl.h>
#include "../../protocol/protocol.h"

namespace transport {

struct PipeSender {
    PipeSender() { _setmode(_fileno(stdout), _O_BINARY); }

    /// Send a frame: header + raw payload bytes.
    bool send(uint32_t type_tag, const void* payload, uint32_t payload_size) {
        uint8_t hdr[PROTOCOL_FRAME_HEADER];
        protocol_build_header(hdr, payload_size, type_tag);
        if (fwrite(hdr, 1, sizeof(hdr), stdout) != sizeof(hdr)) return false;
        if (payload_size > 0 && fwrite(payload, 1, payload_size, stdout) != payload_size) return false;
        fflush(stdout);
        return true;
    }
};

/// Read a frame from stdin. Returns (type_tag, payload_bytes). Empty = EOF.
struct PipeReceiver {
    static constexpr uint32_t MAX_PAYLOAD = 256u * 1024 * 1024;  // 256 MiB

    bool recv(uint32_t& type_tag, std::vector<uint8_t>& payload) {
        uint8_t hdr[PROTOCOL_FRAME_HEADER];
        if (fread(hdr, 1, sizeof(hdr), stdin) != sizeof(hdr)) return false;
        uint32_t size = 0;
        if (!protocol_parse_header(hdr, size, type_tag)) return false;
        if (size > MAX_PAYLOAD) return false;
        payload.resize(size);
        if (size > 0 && fread(payload.data(), 1, size, stdin) != size) return false;
        return true;
    }
};

} // namespace transport
