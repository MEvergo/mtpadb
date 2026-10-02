#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "control_server.h"

#include "wireless_control_protocol.h"

#include <openssl/crypto.h>
#include <openssl/rand.h>

#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <poll.h>
#include <string>

namespace mtpadb::mtpadbd {
namespace {

using wireless_protocol::Command;
using wireless_protocol::Frame;
using wireless_protocol::Request;
using wireless_protocol::Response;
using wireless_protocol::Status;

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

class StringWiper {
  public:
    explicit StringWiper(std::string& value) noexcept : value_(value) {}
    ~StringWiper() {
        if (!value_.empty()) OPENSSL_cleanse(value_.data(), value_.size());
    }

    StringWiper(const StringWiper&) = delete;
    StringWiper& operator=(const StringWiper&) = delete;

  private:
    std::string& value_;
};

class FrameWiper {
  public:
    explicit FrameWiper(Frame& frame) noexcept : frame_(frame) {}
    ~FrameWiper() {
        if (!frame_.empty()) OPENSSL_cleanse(frame_.data(), frame_.size());
    }

    FrameWiper(const FrameWiper&) = delete;
    FrameWiper& operator=(const FrameWiper&) = delete;

  private:
    Frame& frame_;
};

class StopOnExit {
  public:
    explicit StopOnExit(WirelessControl& control) noexcept : control_(control) {}
    ~StopOnExit() { control_.stop(); }

    StopOnExit(const StopOnExit&) = delete;
    StopOnExit& operator=(const StopOnExit&) = delete;

  private:
    WirelessControl& control_;
};

bool generate_pairing_code(std::string& code) {
    constexpr std::uint64_t kRange = std::uint64_t{1} << 32;
    constexpr std::uint32_t kCodeSpace = 1000000;
    constexpr std::uint64_t kAcceptedRange = kRange - (kRange % kCodeSpace);

    for (int attempt = 0; attempt < 128; ++attempt) {
        std::uint32_t random_value = 0;
        if (RAND_bytes(reinterpret_cast<unsigned char*>(&random_value), sizeof(random_value)) != 1) {
            OPENSSL_cleanse(&random_value, sizeof(random_value));
            return false;
        }
        const std::uint64_t sample = random_value;
        OPENSSL_cleanse(&random_value, sizeof(random_value));
        if (sample >= kAcceptedRange) continue;

        std::uint32_t number = static_cast<std::uint32_t>(sample % kCodeSpace);
        code.assign(6, '0');
        for (std::size_t index = code.size(); index > 0; --index) {
            code[index - 1] = static_cast<char>('0' + number % 10);
            number /= 10;
        }
        return true;
    }
    return false;
}

void copy_status(const WirelessControl::Status& source, Status& destination) {
    destination.enabled = source.enabled;
    destination.bind_address = source.bind_address;
    destination.pairing_port = source.pairing_port;
    destination.connect_port = source.connect_port;
    destination.pairing_listener_open = source.pairing_listener_open;
    destination.connect_listener_open = source.connect_listener_open;
    destination.pairing_mdns_published = source.pairing_mdns_published;
    destination.connect_mdns_published = source.connect_mdns_published;
    destination.paired_peer_ids = source.paired_peer_ids;
}

void handle_request(const Request& request, WirelessControl& control, Response& response) {
    response.command = request.command;
    switch (request.command) {
        case Command::kStatus:
            copy_status(control.status(), response.status);
            response.success = true;
            return;
        case Command::kPairStart: {
            std::string pairing_code;
            StringWiper pairing_code_wiper(pairing_code);
            if (!wireless_protocol::is_numeric_ip_address(request.value) ||
                !generate_pairing_code(pairing_code)) {
                return;
            }

            WirelessControl::Options options;
            options.bind_address = request.value;
            options.pairing_code = pairing_code;
            StringWiper options_code_wiper(options.pairing_code);
            if (!control.start(options, WirelessControl::Clock::now())) return;

            const WirelessControl::Status status = control.status();
            if (status.pairing_port == 0 || status.connect_port == 0 ||
                status.pairing_port == status.connect_port) {
                control.stop();
                return;
            }
            response.pairing_port = status.pairing_port;
            response.connect_port = status.connect_port;
            response.pairing_code = pairing_code;
            response.success = true;
            return;
        }
        case Command::kStop:
            control.stop();
            response.success = true;
            return;
        case Command::kRevoke:
            if (!wireless_protocol::is_safe_peer_id(request.value)) return;
            response.success = control.revoke_peer(request.value);
            return;
    }
}

void serve_client(int fd, WirelessControl& control) {
    ScopedFd client(fd);
    struct ucred credentials {};
    socklen_t credentials_size = sizeof(credentials);
    if (::getsockopt(client.get(), SOL_SOCKET, SO_PEERCRED, &credentials, &credentials_size) != 0 ||
        credentials_size != sizeof(credentials) || credentials.uid != 0) {
        return;
    }

    Frame request_frame;
    if (!wireless_protocol::read_frame(client.get(), request_frame)) return;
    Request request;
    if (!wireless_protocol::decode_request(request_frame.data(), request_frame.size(), request)) return;

    Response response;
    StringWiper response_code_wiper(response.pairing_code);
    dispatch_wireless_control_request(request, control, response);
    Frame response_frame;
    FrameWiper response_frame_wiper(response_frame);
    if (!wireless_protocol::encode_response(response, response_frame)) return;
    wireless_protocol::write_frame(client.get(), response_frame);
}

}  // namespace

void dispatch_wireless_control_request(const wireless_protocol::Request& request,
                                       WirelessControl& control,
                                       wireless_protocol::Response& response) {
    if (!response.pairing_code.empty()) {
        OPENSSL_cleanse(response.pairing_code.data(), response.pairing_code.size());
    }
    response = wireless_protocol::Response{};
    handle_request(request, control, response);
}

void run_wireless_control_server(int listening_fd, WirelessControl& control) {
    StopOnExit stop_on_exit(control);
    if (listening_fd < 0) return;
    for (;;) {
        control.advance_time(WirelessControl::Clock::now());
        pollfd listener{};
        listener.fd = listening_fd;
        listener.events = POLLIN;
        const int ready = ::poll(&listener, 1, 1000);
        if (ready < 0) {
            if (errno == EINTR) continue;
            return;
        }
        if (ready == 0) continue;
        if ((listener.revents & POLLIN) == 0) return;

        const int client_fd = ::accept4(listening_fd, nullptr, nullptr, SOCK_CLOEXEC);
        if (client_fd < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            return;
        }
        serve_client(client_fd, control);
    }
}
}  // namespace mtpadb::mtpadbd
