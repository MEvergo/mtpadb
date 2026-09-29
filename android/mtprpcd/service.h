#pragma once

#include <chrono>

#include "mtprpc_session.h"

namespace mtpadb::mtprpcd {

namespace crypto = protocol::crypto;

inline constexpr std::chrono::milliseconds kDefaultHandshakeTimeout{10'000};

// Serves one authenticated MTPX connection and takes ownership of connected_fd.
void serve_connection(int connected_fd, const crypto::DeviceId& device_id,
                      const crypto::Bytes& psk,
                      std::chrono::milliseconds handshake_timeout = kDefaultHandshakeTimeout);

}  // namespace mtpadb::mtprpcd
