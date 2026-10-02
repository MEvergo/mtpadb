#include "rpc_client.h"
#include "mtprpcd/service.h"

#include "mtprpc_crypto.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace {

using mtpadb::protocol::crypto::Bytes;
using mtpadb::protocol::crypto::DeviceId;
using mtpadb::mtprpcd::serve_connection;
using mtpadb::host::RpcOperation;
using mtpadb::host::run_rpc;

constexpr std::size_t kPskBytes = 32;
constexpr std::size_t kOneMiB = 1024U * 1024U;
constexpr std::string_view kRpcFailure = "MTPADB RPC failed";
constexpr std::string_view kInvalidKeyFailure = "MTPADB device key is unavailable or invalid";

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

#define CHECK(expression) require(static_cast<bool>(expression), #expression)

DeviceId test_device_id() {
    DeviceId id{};
    constexpr std::array<std::uint8_t, 16> kId = {
        0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
        0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
    };
    id = kId;
    return id;
}

Bytes test_psk(std::uint8_t start) {
    Bytes psk(kPskBytes);
    for (std::size_t i = 0; i < psk.size(); ++i) {
        psk[i] = static_cast<std::uint8_t>(start + i * 7U);
    }
    return psk;
}

std::string lowercase_hex(const std::uint8_t* bytes, std::size_t size) {
    constexpr char kHex[] = "0123456789abcdef";
    std::string result;
    result.reserve(size * 2U);
    for (std::size_t i = 0; i < size; ++i) {
        result.push_back(kHex[bytes[i] >> 4U]);
        result.push_back(kHex[bytes[i] & 0x0fU]);
    }
    return result;
}

std::string device_key_name(const DeviceId& id) {
    return lowercase_hex(id.data(), id.size()) + ".key";
}

std::string raw_key_string(const Bytes& key) {
    return std::string(reinterpret_cast<const char*>(key.data()), key.size());
}

void check_error_hides_key(const std::string& error, const Bytes& key) {
    CHECK(error.find(raw_key_string(key)) == std::string::npos);
    CHECK(error.find(lowercase_hex(key.data(), key.size())) == std::string::npos);
}

void write_all(int fd, const std::uint8_t* bytes, std::size_t size) {
    std::size_t written = 0;
    while (written < size) {
        const ssize_t count = ::write(fd, bytes + written, size - written);
        if (count > 0) {
            written += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR) continue;
        throw std::runtime_error("writing temporary test data failed");
    }
}

class TemporaryDirectory {
  public:
    TemporaryDirectory() {
        std::string pattern =
                (std::filesystem::temp_directory_path() / "mtpadb-rpc-client-XXXXXX").string();
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');
        char* created = ::mkdtemp(writable.data());
        if (created == nullptr) throw std::runtime_error("creating temporary directory failed");
        path_ = created;
    }

    ~TemporaryDirectory() {
        if (!path_.empty()) {
            std::error_code ignored;
            std::filesystem::remove_all(path_, ignored);
        }
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    const std::filesystem::path& path() const { return path_; }

  private:
    std::filesystem::path path_;
};

class DeviceKeyDirectory {
  public:
    DeviceKeyDirectory() = default;

    std::filesystem::path write_key(const DeviceId& id, const Bytes& key) const {
        const std::filesystem::path file = directory_.path() / device_key_name(id);
        const int fd = ::open(file.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd < 0) throw std::runtime_error("creating test device key failed");
        try {
            if (::fchmod(fd, 0600) != 0) {
                throw std::runtime_error("setting test device key permissions failed");
            }
            struct stat status {};
            if (::fstat(fd, &status) != 0 || status.st_uid != ::geteuid() ||
                (status.st_mode & 0777) != 0600) {
                throw std::runtime_error("test device key does not have secure ownership and mode");
            }
            write_all(fd, key.data(), key.size());
        } catch (...) {
            ::close(fd);
            throw;
        }
        if (::close(fd) != 0) throw std::runtime_error("closing test device key failed");
        return file;
    }

    std::string path_string() const { return directory_.path().string(); }
    const std::filesystem::path& path() const { return directory_.path(); }

  private:
    TemporaryDirectory directory_;
};

class TemporaryFile {
  public:
    explicit TemporaryFile(const std::filesystem::path& directory) {
        std::string pattern = (directory / "rpc-data-XXXXXX").string();
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');
        fd_ = ::mkstemp(writable.data());
        if (fd_ < 0) throw std::runtime_error("creating temporary data file failed");
        if (::unlink(writable.data()) != 0) {
            ::close(fd_);
            fd_ = -1;
            throw std::runtime_error("unlinking temporary data file failed");
        }
        const int flags = ::fcntl(fd_, F_GETFD);
        if (flags < 0 || ::fcntl(fd_, F_SETFD, flags | FD_CLOEXEC) != 0) {
            ::close(fd_);
            fd_ = -1;
            throw std::runtime_error("setting temporary data file close-on-exec failed");
        }
    }

    ~TemporaryFile() {
        if (fd_ >= 0) ::close(fd_);
    }

    TemporaryFile(const TemporaryFile&) = delete;
    TemporaryFile& operator=(const TemporaryFile&) = delete;

    int fd() const { return fd_; }

    void set_contents(const Bytes& contents) {
        if (::lseek(fd_, 0, SEEK_SET) < 0 || ::ftruncate(fd_, 0) != 0) {
            throw std::runtime_error("resetting temporary data file failed");
        }
        if (!contents.empty()) write_all(fd_, contents.data(), contents.size());
        if (::lseek(fd_, 0, SEEK_SET) < 0) {
            throw std::runtime_error("rewinding temporary input file failed");
        }
    }

    Bytes read_exactly(std::size_t expected_size) const {
        struct stat status {};
        if (::fstat(fd_, &status) != 0 || status.st_size < 0 ||
            static_cast<std::uintmax_t>(status.st_size) != expected_size) {
            throw std::runtime_error("RPC output length did not match input length");
        }

        Bytes contents(expected_size);
        std::size_t received = 0;
        while (received < contents.size()) {
            const ssize_t count = ::pread(fd_, contents.data() + received,
                                          contents.size() - received,
                                          static_cast<off_t>(received));
            if (count > 0) {
                received += static_cast<std::size_t>(count);
                continue;
            }
            if (count < 0 && errno == EINTR) continue;
            throw std::runtime_error("reading temporary RPC output failed");
        }
        return contents;
    }

  private:
    int fd_ = -1;
};

class RpcServer {
  public:
    explicit RpcServer(const Bytes& psk,
                       std::chrono::milliseconds handshake_timeout = std::chrono::seconds(5)) {
        int sockets[2] = {-1, -1};
        if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) != 0) {
            throw std::runtime_error("creating RPC test socketpair failed");
        }
        client_fd_ = sockets[0];
        const DeviceId id = test_device_id();
        try {
            worker_ = std::thread([server_fd = sockets[1], id, psk, handshake_timeout] {
                serve_connection(server_fd, id, psk, handshake_timeout);
            });
        } catch (...) {
            ::close(sockets[1]);
            ::close(client_fd_);
            client_fd_ = -1;
            throw;
        }
    }

    ~RpcServer() { finish(); }

    RpcServer(const RpcServer&) = delete;
    RpcServer& operator=(const RpcServer&) = delete;

    int client_fd() const { return client_fd_; }

    void finish() {
        if (client_fd_ >= 0) {
            ::close(client_fd_);
            client_fd_ = -1;
        }
        if (worker_.joinable()) worker_.join();
    }

  private:
    int client_fd_ = -1;
    std::thread worker_;
};

void test_successful_ping_authenticates_with_device_key() {
    const DeviceId id = test_device_id();
    const Bytes psk = test_psk(0x13);
    DeviceKeyDirectory keys;
    keys.write_key(id, psk);
    RpcServer server(psk);

    std::string error;
    const bool succeeded = run_rpc(server.client_fd(), RpcOperation::kPing, -1, -1,
                                   keys.path_string(), &error);
    server.finish();

    CHECK(succeeded);
}

void test_wrong_psk_returns_only_generic_failure() {
    const DeviceId id = test_device_id();
    const Bytes server_psk = test_psk(0x21);
    const Bytes client_psk = test_psk(0x61);
    DeviceKeyDirectory keys;
    keys.write_key(id, client_psk);
    RpcServer server(server_psk);

    std::string error;
    const bool succeeded = run_rpc(server.client_fd(), RpcOperation::kPing, -1, -1,
                                   keys.path_string(), &error);
    server.finish();

    CHECK(!succeeded);
    CHECK(error == kRpcFailure);
    check_error_hides_key(error, server_psk);
    check_error_hides_key(error, client_psk);
}

void test_echo_round_trips_binary_multiple_chunks_with_short_tail() {
    const DeviceId id = test_device_id();
    const Bytes psk = test_psk(0x35);
    DeviceKeyDirectory keys;
    keys.write_key(id, psk);

    Bytes input(kOneMiB + 13U);
    for (std::size_t i = 0; i < input.size(); ++i) {
        input[i] = static_cast<std::uint8_t>((i * 37U + i / 251U) & 0xffU);
    }
    input[0] = 0;
    input[1] = 0xff;
    input[257] = 0;
    input.back() = 0;

    TemporaryFile input_file(keys.path());
    TemporaryFile output_file(keys.path());
    input_file.set_contents(input);
    RpcServer server(psk);

    std::string error;
    const bool succeeded = run_rpc(server.client_fd(), RpcOperation::kEcho,
                                   input_file.fd(), output_file.fd(), keys.path_string(), &error);
    server.finish();

    CHECK(succeeded);
    CHECK(output_file.read_exactly(input.size()) == input);
}

void test_echo_round_trips_empty_input() {
    const DeviceId id = test_device_id();
    const Bytes psk = test_psk(0x39);
    DeviceKeyDirectory keys;
    keys.write_key(id, psk);

    TemporaryFile input_file(keys.path());
    TemporaryFile output_file(keys.path());
    RpcServer server(psk);

    std::string error;
    const bool succeeded = run_rpc(server.client_fd(), RpcOperation::kEcho,
                                   input_file.fd(), output_file.fd(), keys.path_string(), &error);
    server.finish();

    CHECK(succeeded);
    CHECK(output_file.read_exactly(0).empty());
}

void test_unsafe_key_file_metadata_is_rejected_without_disclosure() {
    const DeviceId id = test_device_id();
    const Bytes psk = test_psk(0x47);

    auto expect_key_rejection = [&](DeviceKeyDirectory& keys) {
        RpcServer server(psk, std::chrono::milliseconds(500));
        std::string error;
        const bool succeeded = run_rpc(server.client_fd(), RpcOperation::kPing, -1, -1,
                                       keys.path_string(), &error);
        server.finish();
        CHECK(!succeeded);
        CHECK(error == kInvalidKeyFailure);
        check_error_hides_key(error, psk);
    };

    {
        DeviceKeyDirectory keys;
        const std::filesystem::path key_path = keys.write_key(id, psk);
        CHECK(::chmod(key_path.c_str(), 0644) == 0);
        expect_key_rejection(keys);
    }

    {
        DeviceKeyDirectory keys;
        keys.write_key(id, psk);
        CHECK(::chmod(keys.path().c_str(), 0777) == 0);
        expect_key_rejection(keys);
    }

    if (::geteuid() == 0) {
        DeviceKeyDirectory keys;
        const std::filesystem::path key_path = keys.write_key(id, psk);
        const uid_t untrusted_owner = 1;
        if (::chown(key_path.c_str(), untrusted_owner, static_cast<gid_t>(-1)) == 0) {
            struct stat status {};
            CHECK(::stat(key_path.c_str(), &status) == 0);
            CHECK(status.st_uid == untrusted_owner);
            CHECK(::chmod(key_path.c_str(), 0600) == 0);
            expect_key_rejection(keys);
        }
    }
}

}  // namespace

int main() {
    try {
        test_successful_ping_authenticates_with_device_key();
        test_wrong_psk_returns_only_generic_failure();
        test_echo_round_trips_binary_multiple_chunks_with_short_tail();
        test_echo_round_trips_empty_input();
        test_unsafe_key_file_metadata_is_rejected_without_disclosure();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
