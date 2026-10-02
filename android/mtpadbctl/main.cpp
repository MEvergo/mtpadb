#include "wireless_control_protocol.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <openssl/crypto.h>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>

namespace {
namespace wireless_protocol = mtpadb::mtpadbd::wireless_protocol;

class SensitiveResponseWiper {
  public:
    SensitiveResponseWiper(wireless_protocol::Frame& frame, wireless_protocol::Response& response)
        : frame_(frame), response_(response) {}
    ~SensitiveResponseWiper() {
        if (!response_.pairing_code.empty()) {
            OPENSSL_cleanse(response_.pairing_code.data(), response_.pairing_code.size());
        }
        if (!frame_.empty()) OPENSSL_cleanse(frame_.data(), frame_.size());
    }

    SensitiveResponseWiper(const SensitiveResponseWiper&) = delete;
    SensitiveResponseWiper& operator=(const SensitiveResponseWiper&) = delete;

  private:
    wireless_protocol::Frame& frame_;
    wireless_protocol::Response& response_;
};

constexpr char kControlSocketPath[] = "/dev/socket/mtpadbd-control";

class ScopedFd {
  public:
    explicit ScopedFd(int fd = -1) noexcept : fd_(fd) {}
    ~ScopedFd() {
        if (fd_ >= 0) ::close(fd_);
    }

    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;
    int get() const noexcept { return fd_; }

  private:
    int fd_;
};

void print_usage() {
    std::cerr << "Usage:\n"
                 "  mtpadbctl wireless status\n"
                 "  mtpadbctl wireless pair-start --bind-address <wifi-ip>\n"
                 "  mtpadbctl wireless stop\n"
                 "  mtpadbctl wireless revoke <peer-fingerprint>\n";
}

bool parse_request(int argc, char** argv, mtpadb::mtpadbd::wireless_protocol::Request& request) {
    using mtpadb::mtpadbd::wireless_protocol::Command;
    if (argc < 3 || std::string_view(argv[1]) != "wireless") return false;

    const std::string_view action(argv[2]);
    if (action == "status" && argc == 3) {
        request.command = Command::kStatus;
        return true;
    }
    if (action == "pair-start" && argc == 5 && std::string_view(argv[3]) == "--bind-address") {
        request.command = Command::kPairStart;
        request.value = argv[4];
        return mtpadb::mtpadbd::wireless_protocol::is_numeric_ip_address(request.value);
    }
    if (action == "stop" && argc == 3) {
        request.command = Command::kStop;
        return true;
    }
    if (action == "revoke" && argc == 4) {
        request.command = Command::kRevoke;
        request.value = argv[3];
        return mtpadb::mtpadbd::wireless_protocol::is_safe_peer_id(request.value);
    }
    return false;
}

std::string format_endpoint(const std::string& address, std::uint16_t port) {
    if (address.empty() || port == 0) return "unavailable";
    if (address.find(':') != std::string::npos) {
        return "[" + address + "]:" + std::to_string(port);
    }
    return address + ":" + std::to_string(port);
}

const char* state(bool value, const char* positive, const char* negative) {
    return value ? positive : negative;
}

void print_status(const mtpadb::mtpadbd::wireless_protocol::Status& status) {
    std::cout << "Wireless: " << state(status.enabled, "enabled", "disabled") << '\n'
              << "Bind address: " << (status.bind_address.empty() ? "(none)" : status.bind_address)
              << '\n'
              << "Pairing endpoint: " << format_endpoint(status.bind_address, status.pairing_port)
              << '\n'
              << "Connect endpoint: " << format_endpoint(status.bind_address, status.connect_port)
              << '\n'
              << "Pairing listener: " << state(status.pairing_listener_open, "open", "closed")
              << '\n'
              << "Connect listener: " << state(status.connect_listener_open, "open", "closed")
              << '\n'
              << "Pairing mDNS: " << state(status.pairing_mdns_published, "published", "not published")
              << '\n'
              << "Connect mDNS: " << state(status.connect_mdns_published, "published", "not published")
              << '\n'
              << "Paired peers/fingerprints:" << '\n';
    if (status.paired_peer_ids.empty()) {
        std::cout << "  (none)\n";
        return;
    }
    for (const std::string& peer_id : status.paired_peer_ids) std::cout << "  " << peer_id << '\n';
}

int connect_control_socket() {
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const std::size_t path_length = sizeof(kControlSocketPath) - 1;
    if (path_length >= sizeof(address.sun_path)) return -1;
    std::memcpy(address.sun_path, kControlSocketPath, path_length + 1);

    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    const socklen_t address_length = static_cast<socklen_t>(
            offsetof(sockaddr_un, sun_path) + path_length + 1);
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&address), address_length) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

}  // namespace

int main(int argc, char** argv) {
    if (::geteuid() != 0) {
        std::cerr << "mtpadbctl must run as root.\n";
        return 1;
    }

    mtpadb::mtpadbd::wireless_protocol::Request request;
    if (!parse_request(argc, argv, request)) {
        print_usage();
        return 2;
    }

    mtpadb::mtpadbd::wireless_protocol::Frame request_frame;
    if (!mtpadb::mtpadbd::wireless_protocol::encode_request(request, request_frame)) {
        print_usage();
        return 2;
    }
    ScopedFd socket_fd(connect_control_socket());
    if (socket_fd.get() < 0) {
        std::cerr << "Unable to connect to mtpadbd control socket.\n";
        return 1;
    }

    using namespace mtpadb::mtpadbd::wireless_protocol;
    Frame response_frame;
    Response response;
    SensitiveResponseWiper response_wiper(response_frame, response);
    if (!write_frame(socket_fd.get(), request_frame) || !read_frame(socket_fd.get(), response_frame) ||
        !decode_response(response_frame.data(), response_frame.size(), response) ||
        response.command != request.command) {
        std::cerr << "Invalid response from mtpadbd control socket.\n";
        return 1;
    }
    if (!response.success) {
        std::cerr << "Wireless request failed.\n";
        return 1;
    }

    switch (request.command) {
        case Command::kStatus:
            print_status(response.status);
            break;
        case Command::kPairStart:
            std::cout << "Pairing endpoint: " << format_endpoint(request.value, response.pairing_port)
                      << '\n'
                      << "Connect endpoint: " << format_endpoint(request.value, response.connect_port)
                      << '\n'
                      << "Pairing code: " << response.pairing_code << '\n';
            break;
        case Command::kStop:
            std::cout << "Wireless listeners stopped.\n";
            break;
        case Command::kRevoke:
            std::cout << "Paired peer revoked.\n";
            break;
    }
    std::cout.flush();
    return 0;
}
