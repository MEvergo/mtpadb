#pragma once

#include "mtprpc_session.h"

namespace mtpadb::mtprpcd {

namespace crypto = protocol::crypto;

// Serves one authenticated MTPX connection and takes ownership of connected_fd.
void serve_connection(int connected_fd, const crypto::DeviceId& device_id,
                      const crypto::Bytes& psk);

}  // namespace mtpadb::mtprpcd
