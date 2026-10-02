#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "wireless_control_protocol.h"

#include <arpa/inet.h>
#include <sys/socket.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <limits>
#include <utility>

namespace mtpadb::mtpadbd::wireless_protocol {
namespace {

constexpr std::uint8_t kVersion = 1;
constexpr std::uint8_t kRequestKind = 1;
constexpr std::uint8_t kResponseKind = 2;
constexpr std::size_t kMaximumBindAddressSize = 64;

bool valid_command(Command command) noexcept {
    switch (command) {
        case Command::kStatus:
        case Command::kPairStart:
        case Command::kStop:
        case Command::kRevoke:
            return true;
    }
    return false;
}

bool is_ascii_alphanumeric(char value) noexcept {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
           (value >= '0' && value <= '9');
}

bool append(Frame& frame, const void* data, std::size_t size) {
    if (size == 0) return true;
    if (frame.size() > kMaximumFrameSize || size > kMaximumFrameSize - frame.size()) return false;
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    frame.insert(frame.end(), bytes, bytes + size);
    return true;
}

bool append_byte(Frame& frame, std::uint8_t value) {
    return append(frame, &value, sizeof(value));
}

bool append_u16(Frame& frame, std::uint16_t value) {
    const std::array<std::uint8_t, 2> bytes = {
            static_cast<std::uint8_t>(value >> 8), static_cast<std::uint8_t>(value)};
    return append(frame, bytes.data(), bytes.size());
}

bool append_string(Frame& frame, std::string_view value, std::size_t maximum_size) {
    return value.size() <= maximum_size && value.size() <= std::numeric_limits<std::uint16_t>::max() &&
           append_u16(frame, static_cast<std::uint16_t>(value.size())) &&
           append(frame, value.data(), value.size());
}

class Reader {
  public:
    Reader(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}

    bool read_byte(std::uint8_t& value) {
        if (remaining() < 1) return false;
        value = data_[offset_++];
        return true;
    }

    bool read_u16(std::uint16_t& value) {
        if (remaining() < 2) return false;
        value = static_cast<std::uint16_t>((static_cast<std::uint16_t>(data_[offset_]) << 8) |
                                           data_[offset_ + 1]);
        offset_ += 2;
        return true;
    }

    bool read_string(std::string& value, std::size_t maximum_size) {
        std::uint16_t length = 0;
        if (!read_u16(length) || length > maximum_size || remaining() < length) return false;
        value.assign(reinterpret_cast<const char*>(data_ + offset_), length);
        offset_ += length;
        return true;
    }

    bool complete() const noexcept { return offset_ == size_; }
    std::size_t remaining() const noexcept { return size_ - offset_; }

  private:
    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t offset_ = 0;
};

bool read_boolean(Reader& reader, bool& value) {
    std::uint8_t encoded = 0;
    if (!reader.read_byte(encoded) || encoded > 1) return false;
    value = encoded != 0;
    return true;
}

bool append_boolean(Frame& frame, bool value) {
    return append_byte(frame, value ? 1 : 0);
}

bool read_exact(int fd, void* buffer, std::size_t size) {
    auto* bytes = static_cast<std::uint8_t*>(buffer);
    std::size_t offset = 0;
    while (offset < size) {
        const ssize_t result = ::recv(fd, bytes + offset, size - offset, 0);
        if (result < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (result == 0) return false;
        offset += static_cast<std::size_t>(result);
    }
    return true;
}

bool write_exact(int fd, const void* buffer, std::size_t size) {
    const auto* bytes = static_cast<const std::uint8_t*>(buffer);
    std::size_t offset = 0;
    while (offset < size) {
        const ssize_t result = ::send(fd, bytes + offset, size - offset, MSG_NOSIGNAL);
        if (result < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (result == 0) return false;
        offset += static_cast<std::size_t>(result);
    }
    return true;
}

}  // namespace

bool is_numeric_ip_address(std::string_view address) noexcept {
    if (address.empty() || address.size() > kMaximumBindAddressSize ||
        address.find('\0') != std::string_view::npos) {
        return false;
    }
    std::array<char, kMaximumBindAddressSize + 1> text{};
    std::memcpy(text.data(), address.data(), address.size());
    in_addr ipv4{};
    if (::inet_pton(AF_INET, text.data(), &ipv4) == 1) {
        return ipv4.s_addr != htonl(INADDR_ANY);
    }
    in6_addr ipv6{};
    return ::inet_pton(AF_INET6, text.data(), &ipv6) == 1 && !IN6_IS_ADDR_UNSPECIFIED(&ipv6);
}

bool is_safe_peer_id(std::string_view peer_id) noexcept {
    if (peer_id.empty() || peer_id.size() > kMaximumPeerIdSize ||
        !is_ascii_alphanumeric(peer_id.front())) {
        return false;
    }
    return std::all_of(peer_id.begin(), peer_id.end(), [](char value) {
        return is_ascii_alphanumeric(value) || value == '-' || value == '_' || value == '.';
    });
}
bool encode_request(const Request& request, Frame& frame) {
    frame.clear();
    if (!valid_command(request.command) || !append_byte(frame, kVersion) ||
        !append_byte(frame, kRequestKind) ||
        !append_byte(frame, static_cast<std::uint8_t>(request.command))) {
        return false;
    }
    switch (request.command) {
        case Command::kStatus:
        case Command::kStop:
            if (!request.value.empty()) return false;
            break;
        case Command::kPairStart:
            if (!is_numeric_ip_address(request.value) ||
                !append_string(frame, request.value, kMaximumBindAddressSize)) {
                return false;
            }
            break;
        case Command::kRevoke:
            if (!is_safe_peer_id(request.value) ||
                !append_string(frame, request.value, kMaximumPeerIdSize)) {
                return false;
            }
            break;
    }
    return true;
}

bool decode_request(const std::uint8_t* data, std::size_t size, Request& request) {
    if (data == nullptr || size < 3 || size > kMaximumFrameSize) return false;
    Reader reader(data, size);
    std::uint8_t version = 0;
    std::uint8_t kind = 0;
    std::uint8_t encoded_command = 0;
    if (!reader.read_byte(version) || !reader.read_byte(kind) ||
        !reader.read_byte(encoded_command) || version != kVersion || kind != kRequestKind) {
        return false;
    }
    Request decoded;
    decoded.command = static_cast<Command>(encoded_command);
    if (!valid_command(decoded.command)) return false;
    switch (decoded.command) {
        case Command::kStatus:
        case Command::kStop:
            break;
        case Command::kPairStart:
            if (!reader.read_string(decoded.value, kMaximumBindAddressSize) ||
                !is_numeric_ip_address(decoded.value)) {
                return false;
            }
            break;
        case Command::kRevoke:
            if (!reader.read_string(decoded.value, kMaximumPeerIdSize) ||
                !is_safe_peer_id(decoded.value)) {
                return false;
            }
            break;
    }
    if (!reader.complete()) return false;
    request = std::move(decoded);
    return true;
}

bool encode_response(const Response& response, Frame& frame) {
    frame.clear();
    if (!valid_command(response.command) || !append_byte(frame, kVersion) ||
        !append_byte(frame, kResponseKind) ||
        !append_byte(frame, static_cast<std::uint8_t>(response.command)) ||
        !append_boolean(frame, response.success)) {
        return false;
    }
    if (!response.success) {
        return response.pairing_code.empty() && response.pairing_port == 0 &&
               response.connect_port == 0;
    }

    switch (response.command) {
        case Command::kStatus: {
            const Status& status = response.status;
            if (!response.pairing_code.empty() || response.pairing_port != 0 ||
                response.connect_port != 0 ||
                (!status.bind_address.empty() && !is_numeric_ip_address(status.bind_address)) ||
                status.paired_peer_ids.size() > kMaximumPeerCount ||
                !append_boolean(frame, status.enabled) ||
                !append_string(frame, status.bind_address, kMaximumBindAddressSize) ||
                !append_u16(frame, status.pairing_port) ||
                !append_u16(frame, status.connect_port) ||
                !append_boolean(frame, status.pairing_listener_open) ||
                !append_boolean(frame, status.connect_listener_open) ||
                !append_boolean(frame, status.pairing_mdns_published) ||
                !append_boolean(frame, status.connect_mdns_published) ||
                !append_u16(frame, static_cast<std::uint16_t>(status.paired_peer_ids.size()))) {
                return false;
            }
            for (const std::string& peer_id : status.paired_peer_ids) {
                if (!is_safe_peer_id(peer_id) || !append_string(frame, peer_id, kMaximumPeerIdSize)) {
                    return false;
                }
            }
            return true;
        }
        case Command::kPairStart:
            return response.pairing_port != 0 && response.connect_port != 0 &&
                   response.pairing_port != response.connect_port &&
                   response.pairing_code.size() == 6 &&
                   std::all_of(response.pairing_code.begin(), response.pairing_code.end(),
                               [](char value) { return value >= '0' && value <= '9'; }) &&
                   append_u16(frame, response.pairing_port) &&
                   append_u16(frame, response.connect_port) &&
                   append(frame, response.pairing_code.data(), response.pairing_code.size());
        case Command::kStop:
        case Command::kRevoke:
            return response.pairing_code.empty() && response.pairing_port == 0 &&
                   response.connect_port == 0;
    }
    return false;
}

bool decode_response(const std::uint8_t* data, std::size_t size, Response& response) {
    if (data == nullptr || size < 4 || size > kMaximumFrameSize) return false;
    Reader reader(data, size);
    std::uint8_t version = 0;
    std::uint8_t kind = 0;
    std::uint8_t encoded_command = 0;
    Response decoded;
    if (!reader.read_byte(version) || !reader.read_byte(kind) ||
        !reader.read_byte(encoded_command) || !read_boolean(reader, decoded.success) ||
        version != kVersion || kind != kResponseKind) {
        return false;
    }
    decoded.command = static_cast<Command>(encoded_command);
    if (!valid_command(decoded.command)) return false;
    if (!decoded.success) {
        if (!reader.complete()) return false;
        response = std::move(decoded);
        return true;
    }

    switch (decoded.command) {
        case Command::kStatus: {
            Status& status = decoded.status;
            std::uint16_t peer_count = 0;
            if (!read_boolean(reader, status.enabled) ||
                !reader.read_string(status.bind_address, kMaximumBindAddressSize) ||
                (!status.bind_address.empty() && !is_numeric_ip_address(status.bind_address)) ||
                !reader.read_u16(status.pairing_port) || !reader.read_u16(status.connect_port) ||
                !read_boolean(reader, status.pairing_listener_open) ||
                !read_boolean(reader, status.connect_listener_open) ||
                !read_boolean(reader, status.pairing_mdns_published) ||
                !read_boolean(reader, status.connect_mdns_published) ||
                !reader.read_u16(peer_count) || peer_count > kMaximumPeerCount) {
                return false;
            }
            status.paired_peer_ids.reserve(peer_count);
            for (std::uint16_t i = 0; i < peer_count; ++i) {
                std::string peer_id;
                if (!reader.read_string(peer_id, kMaximumPeerIdSize) || !is_safe_peer_id(peer_id)) {
                    return false;
                }
                status.paired_peer_ids.push_back(std::move(peer_id));
            }
            break;
        }
        case Command::kPairStart: {
            if (!reader.read_u16(decoded.pairing_port) || !reader.read_u16(decoded.connect_port) ||
                decoded.pairing_port == 0 || decoded.connect_port == 0 ||
                decoded.pairing_port == decoded.connect_port || reader.remaining() != 6) {
                return false;
            }
            decoded.pairing_code.assign(reinterpret_cast<const char*>(data + size - 6), 6);
            if (!std::all_of(decoded.pairing_code.begin(), decoded.pairing_code.end(),
                             [](char value) { return value >= '0' && value <= '9'; })) {
                return false;
            }
            // The Reader cursor must include the six code bytes.
            std::uint8_t ignored = 0;
            for (int i = 0; i < 6; ++i) {
                if (!reader.read_byte(ignored)) return false;
            }
            break;
        }
        case Command::kStop:
        case Command::kRevoke:
            break;
    }
    if (!reader.complete()) return false;
    response = std::move(decoded);
    return true;
}

bool read_frame(int fd, Frame& frame) {
    std::array<std::uint8_t, 4> prefix{};
    if (!read_exact(fd, prefix.data(), prefix.size())) return false;
    const std::uint32_t size = (static_cast<std::uint32_t>(prefix[0]) << 24) |
                               (static_cast<std::uint32_t>(prefix[1]) << 16) |
                               (static_cast<std::uint32_t>(prefix[2]) << 8) |
                               static_cast<std::uint32_t>(prefix[3]);
    if (size < 3 || size > kMaximumFrameSize) return false;
    frame.resize(size);
    return read_exact(fd, frame.data(), frame.size());
}

bool write_frame(int fd, const Frame& frame) {
    if (frame.size() < 3 || frame.size() > kMaximumFrameSize) return false;
    const std::uint32_t size = static_cast<std::uint32_t>(frame.size());
    const std::array<std::uint8_t, 4> prefix = {
            static_cast<std::uint8_t>(size >> 24), static_cast<std::uint8_t>(size >> 16),
            static_cast<std::uint8_t>(size >> 8), static_cast<std::uint8_t>(size)};
    return write_exact(fd, prefix.data(), prefix.size()) &&
           write_exact(fd, frame.data(), frame.size());
}

}  // namespace mtpadb::mtpadbd::wireless_protocol
