#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "mtpadbd/control_server.h"
#include "mtpadbd/wireless_control.h"
#include "mtpadbd/wireless_control_protocol.h"

#include <csignal>
#include <cstddef>

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using mtpadb::mtpadbd::WirelessControl;
using mtpadb::mtpadbd::WirelessRuntime;
using namespace mtpadb::mtpadbd::wireless_protocol;

void check(bool condition, const char* expression) {
    if (!condition) throw std::runtime_error(std::string("check failed: ") + expression);
}

#define CHECK(expression) check(static_cast<bool>(expression), #expression)
#define CHECK_EQ(lhs, rhs) check((lhs) == (rhs), #lhs " == " #rhs)
#define CHECK_FALSE(expression) check(!(expression), "! (" #expression ")")

class TemporaryDirectory {
  public:
    TemporaryDirectory() {
        std::string pattern =
                (std::filesystem::temp_directory_path() / "mtpadbd-control-XXXXXX").string();
        std::vector<char> buffer(pattern.begin(), pattern.end());
        buffer.push_back('\0');
        if (char* directory = ::mkdtemp(buffer.data()); directory != nullptr) path_ = directory;
    }

    ~TemporaryDirectory() {
        if (!path_.empty()) {
            std::error_code ignored;
            std::filesystem::remove_all(path_, ignored);
        }
    }

    const std::filesystem::path& path() const { return path_; }

  private:
    std::filesystem::path path_;
};

class FakeRuntime final : public WirelessRuntime {
  public:
    std::optional<std::uint16_t> open_pairing_listener(std::string_view,
                                                         std::uint16_t,
                                                         std::string_view code) override {
        if (code.size() != 6 ||
            !std::all_of(code.begin(), code.end(),
                         [](char digit) { return digit >= '0' && digit <= '9'; })) {
            return std::nullopt;
        }
        pairing_open = true;
        return 31001;
    }

    std::optional<std::uint16_t> open_connect_listener(std::string_view,
                                                       std::uint16_t) override {
        connect_open = true;
        return 31002;
    }

    void close_pairing_listener() noexcept override { pairing_open = false; }
    void close_connect_listener() noexcept override { connect_open = false; }

    bool publish_pairing_mdns(std::string_view, std::uint16_t) override {
        pairing_mdns = true;
        return true;
    }

    bool publish_connect_mdns(std::string_view, std::uint16_t) override {
        connect_mdns = true;
        return true;
    }

    void remove_pairing_mdns() noexcept override { pairing_mdns = false; }
    void remove_connect_mdns() noexcept override { connect_mdns = false; }

    bool pairing_open = false;
    bool connect_open = false;
    bool pairing_mdns = false;
    bool connect_mdns = false;
};

class ChildServer {
  public:
    ChildServer(int listener_fd, const std::filesystem::path& peer_store) {
        pid_ = ::fork();
        if (pid_ < 0) throw std::runtime_error("fork failed");
        if (pid_ == 0) {
            FakeRuntime runtime;
            WirelessControl control(runtime, peer_store);
            mtpadb::mtpadbd::run_wireless_control_server(listener_fd, control);
            ::_exit(0);
        }
    }

    ~ChildServer() {
        if (pid_ > 0) {
            ::kill(pid_, SIGTERM);
            int status = 0;
            while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
            }
        }
    }

  private:
    pid_t pid_ = -1;
};

int create_listener(const std::filesystem::path& socket_path) {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const std::string path = socket_path.string();
    if (path.size() >= sizeof(address.sun_path)) {
        ::close(fd);
        return -1;
    }
    std::copy(path.begin(), path.end(), address.sun_path);
    address.sun_path[path.size()] = '\0';
    const socklen_t length = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + path.size() + 1);
    if (::bind(fd, reinterpret_cast<const sockaddr*>(&address), length) != 0 || ::listen(fd, 8) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

int connect_client(const std::filesystem::path& socket_path) {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const std::string path = socket_path.string();
    std::copy(path.begin(), path.end(), address.sun_path);
    address.sun_path[path.size()] = '\0';
    const socklen_t length = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + path.size() + 1);
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&address), length) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

Response exchange(const std::filesystem::path& socket_path, const Request& request,
                  std::string* raw_response = nullptr) {
    const int fd = connect_client(socket_path);
    CHECK(fd >= 0);
    Frame request_frame;
    CHECK(encode_request(request, request_frame));
    CHECK(write_frame(fd, request_frame));
    Frame response_frame;
    CHECK(read_frame(fd, response_frame));
    if (raw_response != nullptr) {
        raw_response->assign(response_frame.begin(), response_frame.end());
    }
    Response response;
    CHECK(decode_response(response_frame.data(), response_frame.size(), response));
    CHECK_EQ(response.command, request.command);
    ::close(fd);
    return response;
}

void create_peer_file(const std::filesystem::path& directory, std::string_view id,
                      std::string_view key) {
    const std::filesystem::path file_path = directory / std::string(id);
    std::ofstream file(file_path, std::ios::binary);
    CHECK(file.good());
    file.write(key.data(), static_cast<std::streamsize>(key.size()));
    file.close();
    CHECK(file.good());
    CHECK(::chmod(file_path.c_str(), 0600) == 0);
}

void test_socketpair_frame_round_trip() {
    int sockets[2] = {-1, -1};
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);
    Request original;
    original.command = Command::kStatus;
    Frame encoded;
    CHECK(encode_request(original, encoded));
    CHECK(write_frame(sockets[0], encoded));
    Frame received;
    CHECK(read_frame(sockets[1], received));
    Request decoded;
    CHECK(decode_request(received.data(), received.size(), decoded));
    CHECK_EQ(decoded.command, Command::kStatus);
    ::close(sockets[0]);
    ::close(sockets[1]);
}

Response dispatch_request(WirelessControl& control, const Request& request) {
    Frame request_frame;
    CHECK(encode_request(request, request_frame));
    Request validated_request;
    CHECK(decode_request(request_frame.data(), request_frame.size(), validated_request));
    Response response;
    mtpadb::mtpadbd::dispatch_wireless_control_request(validated_request, control, response);
    return response;
}

void test_dispatcher_commands_and_secret_boundaries() {
    TemporaryDirectory temporary;
    CHECK(!temporary.path().empty());
    const std::filesystem::path peer_store = temporary.path() / "peers";
    std::filesystem::create_directory(peer_store);
    CHECK(::chmod(peer_store.c_str(), 0700) == 0);
    constexpr std::string_view first_key = "first-public-key-material";
    constexpr std::string_view second_key = "second-public-key-material";
    create_peer_file(peer_store, "peer-one", first_key);
    create_peer_file(peer_store, "peer-two", second_key);

    FakeRuntime runtime;
    WirelessControl control(runtime, peer_store);
    Request request;
    request.command = Command::kStatus;
    Response response = dispatch_request(control, request);
    CHECK(response.success);
    CHECK(response.pairing_code.empty());
    CHECK(!response.status.enabled);
    CHECK_EQ(response.status.paired_peer_ids.size(), 2u);

    Request pair_request;
    pair_request.command = Command::kPairStart;
    pair_request.value = "127.0.0.1";
    response = dispatch_request(control, pair_request);
    CHECK(response.success);
    CHECK(response.pairing_port == 31001);
    CHECK(response.connect_port == 31002);
    CHECK(response.pairing_port != response.connect_port);
    CHECK(response.pairing_code.size() == 6);
    CHECK(std::all_of(response.pairing_code.begin(), response.pairing_code.end(),
                      [](char digit) { return digit >= '0' && digit <= '9'; }));
    const std::string pairing_code = response.pairing_code;

    response = dispatch_request(control, pair_request);
    CHECK(!response.success);
    CHECK(response.pairing_code.empty());

    request.command = Command::kStatus;
    response = dispatch_request(control, request);
    CHECK(response.success);
    CHECK(response.pairing_code.empty());
    CHECK(response.status.enabled);
    CHECK_EQ(response.status.bind_address, "127.0.0.1");
    CHECK(response.status.pairing_listener_open);
    CHECK(response.status.connect_listener_open);
    CHECK(response.status.pairing_mdns_published);
    CHECK(response.status.connect_mdns_published);
    Frame status_frame;
    CHECK(encode_response(response, status_frame));
    const std::string status_wire(status_frame.begin(), status_frame.end());
    CHECK(status_wire.find(pairing_code) == std::string::npos);
    CHECK(status_wire.find(first_key.data()) == std::string::npos);
    CHECK(status_wire.find(second_key.data()) == std::string::npos);

    Request revoke_request;
    revoke_request.command = Command::kRevoke;
    revoke_request.value = "peer-one";
    response = dispatch_request(control, revoke_request);
    CHECK(response.success);
    CHECK(response.pairing_code.empty());

    request.command = Command::kStatus;
    response = dispatch_request(control, request);
    CHECK(response.success);
    CHECK_EQ(response.status.paired_peer_ids.size(), 1u);
    CHECK_EQ(response.status.paired_peer_ids[0], "peer-two");

    request.command = Command::kStop;
    response = dispatch_request(control, request);
    CHECK(response.success);
    CHECK(response.pairing_code.empty());
    request.command = Command::kStatus;
    response = dispatch_request(control, request);
    CHECK(response.success);
    CHECK(!response.status.enabled);
    CHECK(!response.status.pairing_listener_open);
    CHECK(!response.status.connect_listener_open);
    CHECK(!response.status.pairing_mdns_published);
    CHECK(!response.status.connect_mdns_published);
}

void test_listener_exit_stops_wireless_control() {
    TemporaryDirectory temporary;
    CHECK(!temporary.path().empty());
    FakeRuntime runtime;
    WirelessControl control(runtime, temporary.path() / "peers");
    Request pair_request;
    pair_request.command = Command::kPairStart;
    pair_request.value = "127.0.0.1";
    Response response = dispatch_request(control, pair_request);
    CHECK(response.success);
    CHECK(runtime.pairing_open);
    CHECK(runtime.connect_open);

    const int non_socket_listener = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
    CHECK(non_socket_listener >= 0);
    mtpadb::mtpadbd::run_wireless_control_server(non_socket_listener, control);
    CHECK(::close(non_socket_listener) == 0);

    CHECK(!control.enabled());
    CHECK(!runtime.pairing_open);
    CHECK(!runtime.connect_open);
    CHECK(!runtime.pairing_mdns);
    CHECK(!runtime.connect_mdns);
}

void test_listener_peer_credentials() {
    TemporaryDirectory temporary;
    CHECK(!temporary.path().empty());
    const std::filesystem::path socket_path = temporary.path() / "control.sock";
    const std::filesystem::path peer_store = temporary.path() / "peers";
    std::filesystem::create_directory(peer_store);
    CHECK(::chmod(peer_store.c_str(), 0700) == 0);
    constexpr std::string_view first_key = "first-public-key-material";
    constexpr std::string_view second_key = "second-public-key-material";
    create_peer_file(peer_store, "peer-one", first_key);
    create_peer_file(peer_store, "peer-two", second_key);

    const int listener_fd = create_listener(socket_path);
    CHECK(listener_fd >= 0);
    ChildServer server(listener_fd, peer_store);
    ::close(listener_fd);

    if (::geteuid() != 0) {
        const int fd = connect_client(socket_path);
        CHECK(fd >= 0);
        Request request;
        request.command = Command::kStatus;
        Frame request_frame;
        CHECK(encode_request(request, request_frame));
        CHECK(write_frame(fd, request_frame));
        Frame response_frame;
        CHECK_FALSE(read_frame(fd, response_frame));
        ::close(fd);
        return;
    }

    Request status_request;
    status_request.command = Command::kStatus;
    std::string raw_status;
    Response response = exchange(socket_path, status_request, &raw_status);
    CHECK(response.success);
    CHECK(response.pairing_code.empty());
    CHECK(!response.status.enabled);
    CHECK_EQ(response.status.paired_peer_ids.size(), 2u);
    CHECK(std::find(response.status.paired_peer_ids.begin(), response.status.paired_peer_ids.end(),
                    "peer-one") != response.status.paired_peer_ids.end());
    CHECK(std::find(response.status.paired_peer_ids.begin(), response.status.paired_peer_ids.end(),
                    "peer-two") != response.status.paired_peer_ids.end());
    CHECK(raw_status.find(first_key.data()) == std::string::npos);
    CHECK(raw_status.find(second_key.data()) == std::string::npos);

    Request pair_request;
    pair_request.command = Command::kPairStart;
    pair_request.value = "127.0.0.1";
    response = exchange(socket_path, pair_request);
    CHECK(response.success);
    CHECK(response.pairing_port == 31001);
    CHECK(response.connect_port == 31002);
    CHECK(response.pairing_port != response.connect_port);
    CHECK(response.pairing_code.size() == 6);
    CHECK(std::all_of(response.pairing_code.begin(), response.pairing_code.end(),
                      [](char digit) { return digit >= '0' && digit <= '9'; }));
    const std::string pairing_code = response.pairing_code;

    response = exchange(socket_path, pair_request);
    CHECK(!response.success);
    CHECK(response.pairing_code.empty());

    raw_status.clear();
    response = exchange(socket_path, status_request, &raw_status);
    CHECK(response.success);
    CHECK(response.pairing_code.empty());
    CHECK(response.status.enabled);
    CHECK_EQ(response.status.bind_address, "127.0.0.1");
    CHECK(response.status.pairing_listener_open);
    CHECK(response.status.connect_listener_open);
    CHECK(response.status.pairing_mdns_published);
    CHECK(response.status.connect_mdns_published);
    CHECK(raw_status.find(pairing_code) == std::string::npos);
    CHECK(raw_status.find(first_key.data()) == std::string::npos);
    CHECK(raw_status.find(second_key.data()) == std::string::npos);

    Request revoke_request;
    revoke_request.command = Command::kRevoke;
    revoke_request.value = "peer-one";
    response = exchange(socket_path, revoke_request);
    CHECK(response.success);
    CHECK(response.pairing_code.empty());

    response = exchange(socket_path, status_request);
    CHECK(response.success);
    CHECK(response.pairing_code.empty());
    CHECK_EQ(response.status.paired_peer_ids.size(), 1u);
    CHECK_EQ(response.status.paired_peer_ids[0], "peer-two");

    Request stop_request;
    stop_request.command = Command::kStop;
    response = exchange(socket_path, stop_request);
    CHECK(response.success);
    CHECK(response.pairing_code.empty());

    response = exchange(socket_path, status_request);
    CHECK(response.success);
    CHECK(response.pairing_code.empty());
    CHECK(!response.status.enabled);
    CHECK(!response.status.pairing_listener_open);
    CHECK(!response.status.connect_listener_open);
    CHECK(!response.status.pairing_mdns_published);
    CHECK(!response.status.connect_mdns_published);
}

}  // namespace

int main() {
    try {
        test_socketpair_frame_round_trip();
        test_dispatcher_commands_and_secret_boundaries();
        test_listener_exit_stops_wireless_control();
        test_listener_peer_credentials();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
