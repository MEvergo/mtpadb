#include "mtpadbd/wireless_control.h"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using mtpadb::mtpadbd::WirelessControl;
using mtpadb::mtpadbd::WirelessRuntime;
using Clock = WirelessControl::Clock;
using Options = WirelessControl::Options;

Options test_options(std::string_view bind_address = "127.0.0.1", std::uint16_t pairing_port = 0,
                    std::uint16_t connect_port = 0) {
    return Options{std::string(bind_address), pairing_port, connect_port, "123456"};
}

void check(bool condition, const char* expression) {
    if (!condition) throw std::runtime_error(std::string("check failed: ") + expression);
}

#define CHECK(expression) check(static_cast<bool>(expression), #expression)
#define CHECK_FALSE(expression) check(!(expression), "! (" #expression ")")
#define CHECK_EQ(lhs, rhs) check((lhs) == (rhs), #lhs " == " #rhs)
#define CHECK_NE(lhs, rhs) check((lhs) != (rhs), #lhs " != " #rhs)

class TemporaryPeerStore {
  public:
    TemporaryPeerStore() {
        std::string pattern = (std::filesystem::temp_directory_path() / "mtpadbd-wireless-XXXXXX").string();
        std::vector<char> buffer(pattern.begin(), pattern.end());
        buffer.push_back('\0');
        if (char* directory = ::mkdtemp(buffer.data()); directory != nullptr) {
            path_ = directory;
        }
    }

    ~TemporaryPeerStore() {
        if (!path_.empty()) {
            std::error_code ignored;
            std::filesystem::remove_all(path_, ignored);
        }
    }

    TemporaryPeerStore(const TemporaryPeerStore&) = delete;
    TemporaryPeerStore& operator=(const TemporaryPeerStore&) = delete;

    const std::filesystem::path& path() const { return path_; }

  private:
    std::filesystem::path path_;
};

class SocketRuntime final : public WirelessRuntime {
  public:
    ~SocketRuntime() override {
        close_pairing_listener();
        close_connect_listener();
    }

    std::optional<std::uint16_t> open_pairing_listener(std::string_view bind_address,
                                                        std::uint16_t requested_port,
                                                        std::string_view) override {
        const auto port = open_listener(pairing_fd_, bind_address, requested_port);
        if (port) last_pairing_port_ = *port;
        return port;
    }

    std::optional<std::uint16_t> open_connect_listener(std::string_view bind_address,
                                                       std::uint16_t requested_port) override {
        connect_attempted_with_pairing_listener_open_ = pairing_fd_ >= 0;
        return open_listener(connect_fd_, bind_address, requested_port);
    }

    void close_pairing_listener() noexcept override { close_fd(pairing_fd_); }
    void close_connect_listener() noexcept override { close_fd(connect_fd_); }

    bool publish_pairing_mdns(std::string_view, std::uint16_t) override {
        pairing_mdns_was_published_ = true;
        if (pairing_mdns_fails_) return false;
        pairing_mdns_active_ = true;
        return true;
    }
    bool publish_connect_mdns(std::string_view, std::uint16_t) override {
        connect_mdns_was_published_ = true;
        if (connect_mdns_fails_) return false;
        connect_mdns_active_ = true;
        return true;
    }
    void remove_pairing_mdns() noexcept override { pairing_mdns_active_ = false; }
    void remove_connect_mdns() noexcept override { connect_mdns_active_ = false; }

    bool pairing_listener_open() const { return pairing_fd_ >= 0; }
    bool connect_listener_open() const { return connect_fd_ >= 0; }
    bool pairing_mdns_active() const { return pairing_mdns_active_; }
    bool connect_mdns_active() const { return connect_mdns_active_; }
    bool pairing_mdns_was_published() const { return pairing_mdns_was_published_; }
    bool connect_mdns_was_published() const { return connect_mdns_was_published_; }
    bool pairing_listener_is_loopback() const { return bound_to_loopback(pairing_fd_); }
    bool connect_listener_is_loopback() const { return bound_to_loopback(connect_fd_); }
    bool connect_attempted_with_pairing_listener_open() const {
        return connect_attempted_with_pairing_listener_open_;
    }
    std::uint16_t last_pairing_port() const { return last_pairing_port_; }
    void fail_pairing_mdns() { pairing_mdns_fails_ = true; }
    void fail_connect_mdns() { connect_mdns_fails_ = true; }

  private:
    static std::optional<std::uint16_t> open_listener(int& fd, std::string_view address,
                                                       std::uint16_t requested_port) {
        if (fd >= 0) return std::nullopt;

        const int candidate = ::socket(AF_INET, SOCK_STREAM, 0);
        if (candidate < 0) return std::nullopt;

        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_port = htons(requested_port);
        const std::string address_string(address);
        if (::inet_pton(AF_INET, address_string.c_str(), &local.sin_addr) != 1 ||
            ::bind(candidate, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) < 0 ||
            ::listen(candidate, 16) < 0) {
            ::close(candidate);
            return std::nullopt;
        }

        socklen_t local_size = sizeof(local);
        if (::getsockname(candidate, reinterpret_cast<sockaddr*>(&local), &local_size) < 0) {
            ::close(candidate);
            return std::nullopt;
        }

        fd = candidate;
        return ntohs(local.sin_port);
    }

    static bool bound_to_loopback(int fd) {
        if (fd < 0) return false;
        sockaddr_in local{};
        socklen_t local_size = sizeof(local);
        return ::getsockname(fd, reinterpret_cast<sockaddr*>(&local), &local_size) == 0 &&
               local.sin_family == AF_INET && ntohl(local.sin_addr.s_addr) == INADDR_LOOPBACK;
    }

    static void close_fd(int& fd) noexcept {
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
    }

    int pairing_fd_ = -1;
    int connect_fd_ = -1;
    bool pairing_mdns_active_ = false;
    bool connect_mdns_active_ = false;
    bool pairing_mdns_was_published_ = false;
    bool connect_mdns_was_published_ = false;
    bool pairing_mdns_fails_ = false;
    bool connect_mdns_fails_ = false;
    bool connect_attempted_with_pairing_listener_open_ = false;
    std::uint16_t last_pairing_port_ = 0;
};

class ScopedFd {
  public:
    explicit ScopedFd(int fd = -1) : fd_(fd) {}
    ~ScopedFd() {
        if (fd_ >= 0) ::close(fd_);
    }
    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;
    int get() const { return fd_; }

  private:
    int fd_;
};

bool can_connect(std::uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;

    sockaddr_in remote{};
    remote.sin_family = AF_INET;
    remote.sin_port = htons(port);
    remote.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const bool connected = ::connect(fd, reinterpret_cast<const sockaddr*>(&remote), sizeof(remote)) == 0;
    ::close(fd);
    return connected;
}

std::uint16_t local_port(int fd) {
    sockaddr_in local{};
    socklen_t local_size = sizeof(local);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&local), &local_size) < 0) return 0;
    return ntohs(local.sin_port);
}

void starts_disabled_without_listeners_or_mdns() {
    TemporaryPeerStore peers;
    CHECK_FALSE(peers.path().empty());
    SocketRuntime runtime;
    WirelessControl control(runtime, peers.path());
    const auto status = control.status();
    CHECK_FALSE(status.enabled);
    CHECK_FALSE(status.pairing_listener_open);
    CHECK_FALSE(status.connect_listener_open);
    CHECK_FALSE(status.pairing_mdns_published);
    CHECK_FALSE(status.connect_mdns_published);
    CHECK(status.bind_address.empty());
    CHECK(status.paired_peer_ids.empty());

    CHECK_FALSE(control.enabled());
    CHECK_FALSE(runtime.pairing_listener_open());
    CHECK_FALSE(runtime.connect_listener_open());
    CHECK_FALSE(runtime.pairing_mdns_active());
    CHECK_FALSE(runtime.connect_mdns_active());
    CHECK_FALSE(runtime.pairing_mdns_was_published());
    CHECK_FALSE(runtime.connect_mdns_was_published());
}

void binds_both_listeners_to_explicit_local_address() {
    TemporaryPeerStore peers;
    CHECK_FALSE(peers.path().empty());
    SocketRuntime runtime;
    WirelessControl control(runtime, peers.path());
    const Clock::time_point started_at{};

    CHECK(control.start(test_options(), started_at));
    const auto status = control.status();
    CHECK(status.enabled);
    CHECK(status.pairing_listener_open);
    CHECK(status.connect_listener_open);
    CHECK(status.pairing_mdns_published);
    CHECK(status.connect_mdns_published);
    CHECK_EQ(status.bind_address, "127.0.0.1");
    CHECK_EQ(status.pairing_port, control.pairing_port());
    CHECK_EQ(status.connect_port, control.connect_port());
    CHECK_NE(control.pairing_port(), 0);
    CHECK_NE(control.connect_port(), 0);
    CHECK_NE(control.pairing_port(), control.connect_port());
    CHECK(runtime.pairing_listener_is_loopback());
    CHECK(runtime.connect_listener_is_loopback());
    CHECK(can_connect(control.pairing_port()));
    CHECK(can_connect(control.connect_port()));
    CHECK(runtime.pairing_mdns_active());
    CHECK(runtime.connect_mdns_active());
}
void manual_pairing_remains_available_when_mdns_registration_fails() {
    TemporaryPeerStore peers;
    CHECK_FALSE(peers.path().empty());
    SocketRuntime runtime;
    runtime.fail_pairing_mdns();
    runtime.fail_connect_mdns();
    WirelessControl control(runtime, peers.path());

    CHECK(control.start(test_options(), Clock::time_point{}));
    const auto status = control.status();
    CHECK(status.enabled);
    CHECK(status.pairing_listener_open);
    CHECK(status.connect_listener_open);
    CHECK_FALSE(status.pairing_mdns_published);
    CHECK_FALSE(status.connect_mdns_published);
    CHECK_NE(status.pairing_port, 0);
    CHECK_NE(status.connect_port, 0);
    CHECK(can_connect(status.pairing_port));
    CHECK(can_connect(status.connect_port));

    CHECK(control.pairing_succeeded("peer-a", "public-key-a"));
    CHECK_FALSE(runtime.pairing_listener_open());
    CHECK(runtime.connect_listener_open());
    CHECK(control.has_peer("peer-a"));
}

void rejects_nonlocal_and_wildcard_addresses() {
    const Clock::time_point started_at{};
    for (const std::string address : {"203.0.113.77", "0.0.0.0"}) {
        TemporaryPeerStore peers;
        CHECK_FALSE(peers.path().empty());
        SocketRuntime runtime;
        WirelessControl control(runtime, peers.path());

        CHECK_FALSE(control.start(test_options(address), started_at));
        CHECK_FALSE(control.enabled());
        CHECK_FALSE(runtime.pairing_listener_open());
        CHECK_FALSE(runtime.connect_listener_open());
        CHECK_FALSE(runtime.pairing_mdns_active());
        CHECK_FALSE(runtime.connect_mdns_active());
    }
}

void rejects_invalid_pairing_codes() {
    const Clock::time_point started_at{};
    for (const std::string code : {"", "12345", "12a456", "1234567"}) {
        TemporaryPeerStore peers;
        CHECK_FALSE(peers.path().empty());
        SocketRuntime runtime;
        WirelessControl control(runtime, peers.path());
        Options options = test_options();
        options.pairing_code = code;

        CHECK_FALSE(control.start(options, started_at));
        CHECK_FALSE(control.enabled());
        CHECK_FALSE(runtime.pairing_listener_open());
        CHECK_FALSE(runtime.connect_listener_open());
        CHECK_FALSE(runtime.pairing_mdns_active());
        CHECK_FALSE(runtime.connect_mdns_active());
    }
}

void rejects_an_occupied_port_without_partial_listeners() {
    ScopedFd occupied(::socket(AF_INET, SOCK_STREAM, 0));
    CHECK(occupied.get() >= 0);

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = 0;
    local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK_EQ(::bind(occupied.get(), reinterpret_cast<const sockaddr*>(&local), sizeof(local)), 0);
    CHECK_EQ(::listen(occupied.get(), 16), 0);
    const std::uint16_t occupied_port = local_port(occupied.get());
    CHECK_NE(occupied_port, 0);
    CHECK(can_connect(occupied_port));

    TemporaryPeerStore peers;
    CHECK_FALSE(peers.path().empty());
    SocketRuntime runtime;
    WirelessControl control(runtime, peers.path());
    CHECK_FALSE(control.start(test_options("127.0.0.1", occupied_port), Clock::time_point{}));
    CHECK_FALSE(control.enabled());
    CHECK_FALSE(runtime.pairing_listener_open());
    CHECK_FALSE(runtime.connect_listener_open());
    CHECK_FALSE(runtime.pairing_mdns_active());
    CHECK_FALSE(runtime.connect_mdns_active());
    CHECK(can_connect(occupied_port));
    CHECK_FALSE(runtime.pairing_mdns_was_published());
    CHECK_FALSE(runtime.connect_mdns_was_published());
}

void rolls_back_pairing_listener_when_connect_port_is_occupied() {
    ScopedFd occupied(::socket(AF_INET, SOCK_STREAM, 0));
    CHECK(occupied.get() >= 0);

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = 0;
    local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK_EQ(::bind(occupied.get(), reinterpret_cast<const sockaddr*>(&local), sizeof(local)), 0);
    CHECK_EQ(::listen(occupied.get(), 16), 0);
    const std::uint16_t occupied_port = local_port(occupied.get());
    CHECK_NE(occupied_port, 0);
    CHECK(can_connect(occupied_port));

    TemporaryPeerStore peers;
    CHECK_FALSE(peers.path().empty());
    SocketRuntime runtime;
    WirelessControl control(runtime, peers.path());
    CHECK_FALSE(control.start(test_options("127.0.0.1", 0, occupied_port), Clock::time_point{}));
    CHECK(runtime.connect_attempted_with_pairing_listener_open());
    CHECK_NE(runtime.last_pairing_port(), 0);
    CHECK_FALSE(runtime.pairing_listener_open());
    CHECK_FALSE(runtime.connect_listener_open());
    CHECK_FALSE(can_connect(runtime.last_pairing_port()));
    CHECK_FALSE(runtime.pairing_mdns_active());
    CHECK_FALSE(runtime.pairing_mdns_was_published());
    CHECK_FALSE(runtime.connect_mdns_was_published());
    CHECK_FALSE(runtime.connect_mdns_active());
    CHECK(can_connect(occupied_port));
}


void pairing_window_expires_at_exactly_120_seconds() {
    TemporaryPeerStore peers;
    CHECK_FALSE(peers.path().empty());
    SocketRuntime runtime;
    WirelessControl control(runtime, peers.path());
    const Clock::time_point started_at{};

    CHECK(control.start(test_options(), started_at));
    const std::uint16_t pairing_port = control.pairing_port();
    const std::uint16_t connect_port = control.connect_port();
    CHECK(can_connect(pairing_port));
    CHECK(can_connect(connect_port));

    control.advance_time(started_at + std::chrono::seconds(119));
    CHECK(runtime.pairing_listener_open());
    CHECK(can_connect(pairing_port));

    control.advance_time(started_at + std::chrono::seconds(120));
    CHECK_FALSE(runtime.pairing_listener_open());
    CHECK_FALSE(can_connect(pairing_port));
    CHECK(runtime.connect_listener_open());
    CHECK(can_connect(connect_port));
    CHECK_FALSE(runtime.pairing_mdns_active());
    CHECK(runtime.connect_mdns_active());
    const auto status = control.status();
    CHECK(status.enabled);
    CHECK_FALSE(status.pairing_listener_open);
    CHECK(status.connect_listener_open);
    CHECK_FALSE(status.pairing_mdns_published);
    CHECK(status.connect_mdns_published);
    CHECK_EQ(status.pairing_port, 0);
    CHECK_EQ(status.connect_port, connect_port);
}

void successful_pairing_closes_only_pairing_listener() {
    TemporaryPeerStore peers;
    CHECK_FALSE(peers.path().empty());
    SocketRuntime runtime;
    WirelessControl control(runtime, peers.path());
    CHECK(control.start(test_options(), Clock::time_point{}));
    const std::uint16_t pairing_port = control.pairing_port();
    const std::uint16_t connect_port = control.connect_port();

    CHECK(control.pairing_succeeded("peer-a", "public-key-a"));
    CHECK_FALSE(runtime.pairing_listener_open());
    CHECK_FALSE(can_connect(pairing_port));
    CHECK(runtime.connect_listener_open());
    CHECK(can_connect(connect_port));
    CHECK(control.enabled());
    CHECK_FALSE(runtime.pairing_mdns_active());
    CHECK(runtime.connect_mdns_active());
    CHECK(control.has_peer("peer-a"));
    const auto status = control.status();
    CHECK(status.enabled);
    CHECK_FALSE(status.pairing_listener_open);
    CHECK(status.connect_listener_open);
    CHECK_FALSE(status.pairing_mdns_published);
    CHECK(status.connect_mdns_published);
    CHECK_EQ(status.paired_peer_ids.size(), 1U);
    CHECK_EQ(status.paired_peer_ids.front(), "peer-a");
    CHECK_EQ(status.pairing_port, 0);
    CHECK_EQ(status.connect_port, connect_port);
    CHECK_FALSE(std::filesystem::is_empty(peers.path()));
}

void stop_closes_both_listeners_removes_mdns_and_preserves_peers() {
    TemporaryPeerStore peers;
    CHECK_FALSE(peers.path().empty());
    SocketRuntime runtime;
    WirelessControl control(runtime, peers.path());
    CHECK(control.start(test_options(), Clock::time_point{}));
    const std::uint16_t pairing_port = control.pairing_port();
    const std::uint16_t connect_port = control.connect_port();
    CHECK(control.pairing_succeeded("peer-a", "public-key-a"));

    control.stop();
    CHECK_FALSE(control.enabled());
    CHECK_FALSE(runtime.pairing_listener_open());
    CHECK_FALSE(runtime.connect_listener_open());
    CHECK_FALSE(runtime.pairing_mdns_active());
    CHECK_FALSE(runtime.connect_mdns_active());
    CHECK_FALSE(can_connect(pairing_port));
    CHECK_FALSE(can_connect(connect_port));
    CHECK(control.has_peer("peer-a"));
    CHECK_FALSE(std::filesystem::is_empty(peers.path()));
    const auto status = control.status();
    CHECK_FALSE(status.enabled);
    CHECK_FALSE(status.pairing_listener_open);
    CHECK_FALSE(status.connect_listener_open);
    CHECK_FALSE(status.pairing_mdns_published);
    CHECK_FALSE(status.connect_mdns_published);
    CHECK(status.bind_address.empty());
    CHECK_EQ(status.paired_peer_ids.size(), 1U);
}

void revoking_one_peer_preserves_other_stored_peers() {
    TemporaryPeerStore peers;
    CHECK_FALSE(peers.path().empty());
    {
        SocketRuntime runtime;
        WirelessControl control(runtime, peers.path());
        CHECK(control.start(test_options(), Clock::time_point{}));
        CHECK(control.pairing_succeeded("peer-a", "public-key-a"));
        control.stop();
        CHECK(control.has_peer("peer-a"));
    }

    {
        SocketRuntime runtime;
        WirelessControl control(runtime, peers.path());
        CHECK(control.has_peer("peer-a"));
        CHECK(control.start(test_options(), Clock::time_point{}));
        CHECK(control.pairing_succeeded("peer-b", "public-key-b"));
        control.stop();
        CHECK(control.has_peer("peer-a"));
        CHECK(control.has_peer("peer-b"));

        CHECK(control.revoke_peer("peer-a"));
        CHECK_FALSE(control.has_peer("peer-a"));
        CHECK(control.has_peer("peer-b"));
        CHECK_FALSE(std::filesystem::is_empty(peers.path()));
    }
}

void paired_peer_iteration_stops_when_callback_returns_false() {
    TemporaryPeerStore peers;
    CHECK_FALSE(peers.path().empty());
    {
        SocketRuntime runtime;
        WirelessControl control(runtime, peers.path());
        CHECK(control.start(test_options(), Clock::time_point{}));
        CHECK(control.pairing_succeeded("peer-a", "public-key-a"));
        control.stop();
    }
    {
        SocketRuntime runtime;
        WirelessControl control(runtime, peers.path());
        CHECK(control.start(test_options(), Clock::time_point{}));
        CHECK(control.pairing_succeeded("peer-b", "public-key-b"));
        control.stop();

        int visits = 0;
        control.for_each_paired_peer([&](std::string_view, std::string_view) {
            ++visits;
            return false;
        });
        CHECK_EQ(visits, 1);
    }
}

}  // namespace

int main() {
    try {
        starts_disabled_without_listeners_or_mdns();
        binds_both_listeners_to_explicit_local_address();
        manual_pairing_remains_available_when_mdns_registration_fails();
        rejects_nonlocal_and_wildcard_addresses();
        rejects_invalid_pairing_codes();
        rejects_an_occupied_port_without_partial_listeners();
        rolls_back_pairing_listener_when_connect_port_is_occupied();
        pairing_window_expires_at_exactly_120_seconds();
        successful_pairing_closes_only_pairing_listener();
        stop_closes_both_listeners_removes_mdns_and_preserves_peers();
        revoking_one_peer_preserves_other_stored_peers();
        paired_peer_iteration_stops_when_callback_returns_false();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
