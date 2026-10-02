#pragma once

#include <string>
#include <string_view>

namespace mtpadb::host {

enum class RpcOperation {
    kPing,
    kEcho,
};

// Uses a connected ADB service socket returned by adb_connect(). All descriptors
// are borrowed; kPing requires input_fd and output_fd to be -1. Echo data is
// streamed between input_fd and output_fd in bounded chunks.
// device_keys_directory contains lowercase-hex device-id filenames with raw
// 32-byte PSKs. The directory must be root- or effective-user-owned and not
// group/world-writable; key files must be effective-user-owned and private.
bool run_rpc(int adb_service_fd, RpcOperation operation, int input_fd, int output_fd,
             std::string_view device_keys_directory, std::string* error);

}  // namespace mtpadb::host
