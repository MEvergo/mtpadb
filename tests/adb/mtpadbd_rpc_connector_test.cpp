#include "mtpadbd/rpc_service.h"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <thread>

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <unistd.h>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

bool send_all(int fd, const std::uint8_t* bytes, std::size_t size) {
    std::size_t sent = 0;
    while (sent < size) {
        const ssize_t count = send(fd, bytes + sent, size - sent, MSG_NOSIGNAL);
        if (count > 0) {
            sent += static_cast<std::size_t>(count);
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

bool receive_exact(int fd, std::uint8_t* bytes, std::size_t size) {
    std::size_t received = 0;
    while (received < size) {
        const ssize_t count = recv(fd, bytes + received, size - received, 0);
        if (count > 0) {
            received += static_cast<std::size_t>(count);
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

class TemporarySocketPath {
public:
    TemporarySocketPath() {
        char directory[] = "/tmp/mtpadbd-rpc-connector-XXXXXX";
        char* created = mkdtemp(directory);
        if (created == nullptr) throw std::runtime_error("mkdtemp failed");
        directory_ = created;
        path_ = directory_ + "/listener.sock";
    }

    ~TemporarySocketPath() {
        unlink(path_.c_str());
        rmdir(directory_.c_str());
    }

    const std::string& path() const noexcept { return path_; }

private:
    std::string directory_;
    std::string path_;
};

void test_connector_transfers_bytes_and_propagates_close() {
    TemporarySocketPath socket_path;
    const int listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    require(listener >= 0, "creating AF_UNIX listener failed");

    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    require(socket_path.path().size() < sizeof(address.sun_path), "temporary socket path too long");
    std::memcpy(address.sun_path, socket_path.path().c_str(), socket_path.path().size() + 1);
    const socklen_t address_size = static_cast<socklen_t>(
        offsetof(sockaddr_un, sun_path) + socket_path.path().size() + 1);
    if (bind(listener, reinterpret_cast<const sockaddr*>(&address), address_size) != 0 ||
        listen(listener, 1) != 0) {
        close(listener);
        throw std::runtime_error("binding AF_UNIX listener failed");
    }

    constexpr std::array<std::uint8_t, 5> request{{'m', 't', 'p', 'x', '!'}};
    constexpr std::array<std::uint8_t, 6> response{{'r', 'e', 'p', 'l', 'y', '!'}};
    std::string server_error;
    bool server_observed_eof = false;
    std::thread server([&] {
        pollfd listener_poll{listener, POLLIN, 0};
        int poll_result;
        do {
            poll_result = poll(&listener_poll, 1, 5000);
        } while (poll_result < 0 && errno == EINTR);
        if (poll_result <= 0 || (listener_poll.revents & POLLIN) == 0) {
            server_error = "listener did not receive a connection";
            return;
        }

        const int accepted = accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
        if (accepted < 0) {
            server_error = "accept failed";
            return;
        }

        std::array<std::uint8_t, request.size()> received{};
        if (!receive_exact(accepted, received.data(), received.size()) || received != request) {
            server_error = "listener did not receive the caller's request bytes";
            close(accepted);
            return;
        }
        if (!send_all(accepted, response.data(), response.size())) {
            server_error = "listener could not send response bytes";
            close(accepted);
            return;
        }

        std::uint8_t extra = 0;
        for (;;) {
            const ssize_t count = recv(accepted, &extra, sizeof(extra), 0);
            if (count == 0) {
                server_observed_eof = true;
                break;
            }
            if (count < 0 && errno == EINTR) continue;
            if (count < 0) server_error = "reading caller close failed";
            else server_error = "caller sent unexpected trailing bytes";
            break;
        }
        close(accepted);
    });

    const int connected = mtpadb::mtpadbd::connect_mtprpcd_socket(socket_path.path().c_str());
    bool client_transfer_succeeded = false;
    if (connected >= 0) {
        timeval timeout{5, 0};
        setsockopt(connected, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(connected, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        std::array<std::uint8_t, response.size()> received{};
        client_transfer_succeeded =
            send_all(connected, request.data(), request.size()) &&
            receive_exact(connected, received.data(), received.size()) && received == response;
        close(connected);
    }

    server.join();
    close(listener);
    require(connected >= 0, "connect_mtprpcd_socket did not connect to a live listener");
    require(client_transfer_succeeded, "connector did not transfer bytes in both directions");
    if (!server_error.empty()) throw std::runtime_error(server_error);
    require(server_observed_eof, "closing the returned fd did not deliver EOF to the peer");
}

}  // namespace

int main() {
    try {
        test_connector_transfers_bytes_and_propagates_close();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
