#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mtpadb::mtpadbd::wireless_protocol {

constexpr std::uint32_t kMaximumFrameSize = 256 * 1024;
constexpr std::size_t kMaximumPeerIdSize = 128;
constexpr std::size_t kMaximumPeerCount = 1024;

using Frame = std::vector<std::uint8_t>;

enum class Command : std::uint8_t {
    kStatus = 1,
    kPairStart = 2,
    kStop = 3,
    kRevoke = 4,
};

struct Request {
    Command command = Command::kStatus;
    std::string value;
};

struct Status {
    bool enabled = false;
    std::string bind_address;
    std::uint16_t pairing_port = 0;
    std::uint16_t connect_port = 0;
    bool pairing_listener_open = false;
    bool connect_listener_open = false;
    bool pairing_mdns_published = false;
    bool connect_mdns_published = false;
    std::vector<std::string> paired_peer_ids;
};

struct Response {
    Command command = Command::kStatus;
    bool success = false;
    Status status;
    std::uint16_t pairing_port = 0;
    std::uint16_t connect_port = 0;
    std::string pairing_code;
};

bool is_numeric_ip_address(std::string_view address) noexcept;
bool is_safe_peer_id(std::string_view peer_id) noexcept;

bool encode_request(const Request& request, Frame& frame);
bool decode_request(const std::uint8_t* data, std::size_t size, Request& request);
bool encode_response(const Response& response, Frame& frame);
bool decode_response(const std::uint8_t* data, std::size_t size, Response& response);

// Frames contain a four-byte big-endian length followed by a bounded protocol body.
bool read_frame(int fd, Frame& frame);
bool write_frame(int fd, const Frame& frame);

}  // namespace mtpadb::mtpadbd::wireless_protocol
