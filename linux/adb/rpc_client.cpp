#include "rpc_client.h"

#include "mtprpc_protocol.h"
#include "mtprpc_session.h"

#include <openssl/crypto.h>
#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <optional>
#include <poll.h>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace mtpadb::host {
namespace {

namespace protocol = mtpadb::protocol;
namespace crypto = mtpadb::protocol::crypto;

constexpr std::size_t kPskBytes = 32;
constexpr std::size_t kAeadTagBytes = 16;
constexpr std::size_t kDeviceHelloBytes = 16 + 32;
constexpr std::size_t kHostHelloBytes = 2 + 32;
constexpr std::uint8_t kGenericError = 0x01;
constexpr std::uint8_t kSuccess = 0x00;
constexpr int kDirectoryFlags = O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW;
constexpr int kKeyFlags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK;

class ScopedFd {
  public:
    explicit ScopedFd(int fd = -1) noexcept : fd_(fd) {}
    ~ScopedFd() {
        if (fd_ >= 0) ::close(fd_);
    }

    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;
    ScopedFd(ScopedFd&& other) noexcept : fd_(other.release()) {}
    ScopedFd& operator=(ScopedFd&& other) noexcept {
        if (this != &other) {
            if (fd_ >= 0) ::close(fd_);
            fd_ = other.release();
        }
        return *this;
    }

    int get() const noexcept { return fd_; }
    int release() noexcept {
        const int fd = fd_;
        fd_ = -1;
        return fd;
    }

  private:
    int fd_;
};

class BytesWiper {
  public:
    explicit BytesWiper(crypto::Bytes& bytes) noexcept : bytes_(bytes) {}
    ~BytesWiper() {
        if (!bytes_.empty()) OPENSSL_cleanse(bytes_.data(), bytes_.size());
    }

    BytesWiper(const BytesWiper&) = delete;
    BytesWiper& operator=(const BytesWiper&) = delete;

  private:
    crypto::Bytes& bytes_;
};

void set_error(std::string* error, const char* message) {
    if (error != nullptr) *error = message;
}

int open_key_directory(std::string_view path) noexcept {
    if (path.empty() || path.find('\0') != std::string_view::npos) return -1;
    try {
        const bool absolute = path.front() == '/';
        ScopedFd current(::open(absolute ? "/" : ".", kDirectoryFlags));
        if (current.get() < 0) return -1;

        bool saw_component = false;
        std::size_t offset = absolute ? 1 : 0;
        while (offset <= path.size()) {
            const std::size_t end = path.find('/', offset);
            const std::size_t length = (end == std::string_view::npos ? path.size() : end) - offset;
            const std::string_view component = path.substr(offset, length);
            if (!component.empty() && component != ".") {
                if (component == "..") return -1;
                const std::string name(component);
                const int next = ::openat(current.get(), name.c_str(), kDirectoryFlags);
                if (next < 0) return -1;
                current = ScopedFd(next);
                saw_component = true;
            }
            if (end == std::string_view::npos) break;
            offset = end + 1;
        }

        struct stat status {};
        if (!saw_component || ::fstat(current.get(), &status) != 0 ||
            !S_ISDIR(status.st_mode) ||
            (status.st_uid != ::geteuid() && status.st_uid != 0) ||
            (status.st_mode & 0022) != 0) {
            return -1;
        }
        return current.release();
    } catch (...) {
        return -1;
    }
}

std::string device_key_filename(const crypto::DeviceId& device_id) {
    constexpr char kHex[] = "0123456789abcdef";
    std::string filename;
    filename.reserve(device_id.size() * 2 + 4);
    for (const std::uint8_t byte : device_id) {
        filename.push_back(kHex[byte >> 4]);
        filename.push_back(kHex[byte & 0x0f]);
    }
    filename += ".key";
    return filename;
}

bool read_key_file(int directory_fd, const crypto::DeviceId& device_id, crypto::Bytes& psk) {
    const std::string filename = device_key_filename(device_id);
    ScopedFd file(::openat(directory_fd, filename.c_str(), kKeyFlags));
    if (file.get() < 0) return false;

    struct stat status {};
    if (::fstat(file.get(), &status) != 0 || !S_ISREG(status.st_mode) ||
        status.st_uid != ::geteuid() || (status.st_mode & 0077) != 0 ||
        status.st_size != static_cast<off_t>(kPskBytes)) {
        return false;
    }

    psk.resize(kPskBytes);
    std::size_t offset = 0;
    while (offset < psk.size()) {
        const ssize_t count = ::read(file.get(), psk.data() + offset, psk.size() - offset);
        if (count > 0) {
            offset += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR) continue;
        return false;
    }

    std::uint8_t extra_byte = 0;
    for (;;) {
        const ssize_t count = ::read(file.get(), &extra_byte, sizeof(extra_byte));
        if (count == 0) return true;
        if (count < 0 && errno == EINTR) continue;
        return false;
    }
}

bool load_device_key(std::string_view directory, const crypto::DeviceId& device_id,
                     crypto::Bytes& psk) noexcept {
    ScopedFd directory_fd(open_key_directory(directory));
    if (directory_fd.get() < 0) return false;
    try {
        return read_key_file(directory_fd.get(), device_id, psk);
    } catch (...) {
        return false;
    }
}

bool wait_for(int fd, short events) noexcept {
    pollfd descriptor{fd, events, 0};
    for (;;) {
        const int result = ::poll(&descriptor, 1, -1);
        if (result > 0) {
            return (descriptor.revents & (POLLNVAL | POLLERR)) == 0 &&
                   (descriptor.revents & events) != 0;
        }
        if (result < 0 && errno == EINTR) continue;
        return false;
    }
}

bool send_all(int fd, const std::uint8_t* bytes, std::size_t size) noexcept {
    std::size_t offset = 0;
    while (offset < size) {
        const ssize_t count = ::send(fd, bytes + offset, size - offset, MSG_NOSIGNAL);
        if (count > 0) {
            offset += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR) continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && wait_for(fd, POLLOUT)) {
            continue;
        }
        return false;
    }
    return true;
}

bool write_all(int fd, const std::uint8_t* bytes, std::size_t size) noexcept {
    std::size_t offset = 0;
    while (offset < size) {
        const ssize_t count = ::write(fd, bytes + offset, size - offset);
        if (count > 0) {
            offset += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR) continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && wait_for(fd, POLLOUT)) {
            continue;
        }
        return false;
    }
    return true;
}

bool receive_exact(int fd, std::uint8_t* bytes, std::size_t size) noexcept {
    std::size_t offset = 0;
    while (offset < size) {
        const ssize_t count = ::recv(fd, bytes + offset, size - offset, 0);
        if (count > 0) {
            offset += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR) continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && wait_for(fd, POLLIN)) {
            continue;
        }
        return false;
    }
    return true;
}

bool send_plain_frame(int fd, const protocol::Frame& frame) {
    try {
        const crypto::Bytes wire = protocol::encode_frame(frame);
        return send_all(fd, wire.data(), wire.size());
    } catch (...) {
        return false;
    }
}

bool receive_frame(int fd, protocol::Frame& frame, protocol::FrameHeader& header) {
    std::array<std::uint8_t, protocol::kFrameHeaderBytes> wire_header{};
    if (!receive_exact(fd, wire_header.data(), wire_header.size())) return false;
    try {
        header = protocol::decode_header(wire_header.data(), wire_header.size());
    } catch (...) {
        return false;
    }

    frame.type = header.type;
    frame.flags = header.flags;
    frame.stream_id = header.stream_id;
    frame.sequence = header.sequence;
    frame.reserved = header.reserved;
    frame.payload.resize(header.payload_length);
    return frame.payload.empty() || receive_exact(fd, frame.payload.data(), frame.payload.size());
}

bool send_auth_frame(int fd, crypto::Bytes auth) {
    BytesWiper auth_wiper(auth);
    protocol::Frame frame;
    frame.type = protocol::FrameType::AUTH;
    frame.payload = std::move(auth);
    BytesWiper frame_wiper(frame.payload);

    crypto::Bytes wire;
    try {
        wire = protocol::encode_frame(frame);
    } catch (...) {
        return false;
    }
    BytesWiper wire_wiper(wire);
    return send_all(fd, wire.data(), wire.size());
}

bool send_encrypted_frame(int fd, crypto::Session& session, const protocol::FrameHeader& header,
                          const crypto::Bytes& plaintext) {
    try {
        crypto::EncryptedFrame encrypted = session.seal(header, plaintext);
        const auto wire_header = protocol::serialize_header(encrypted.header);
        return send_all(fd, wire_header.data(), wire_header.size()) &&
               send_all(fd, encrypted.ciphertext.data(), encrypted.ciphertext.size()) &&
               send_all(fd, encrypted.tag.data(), encrypted.tag.size());
    } catch (...) {
        return false;
    }
}

bool is_generic_plain_error(const protocol::Frame& frame,
                            const protocol::FrameHeader& header) noexcept {
    return frame.type == protocol::FrameType::ERROR && header.flags == 0 &&
           header.stream_id == 0 && header.sequence == 0 && header.reserved == 0 &&
           frame.payload.size() == 1 && frame.payload[0] == kGenericError;
}

bool receive_encrypted_frame(int fd, crypto::Session& session, protocol::FrameHeader& header,
                             crypto::Bytes& plaintext) {
    protocol::Frame frame;
    if (!receive_frame(fd, frame, header)) return false;
    if (is_generic_plain_error(frame, header) || frame.payload.size() < kAeadTagBytes) return false;

    crypto::EncryptedFrame encrypted;
    encrypted.header = header;
    std::copy(frame.payload.end() - static_cast<std::ptrdiff_t>(kAeadTagBytes), frame.payload.end(),
              encrypted.tag.begin());
    frame.payload.resize(frame.payload.size() - kAeadTagBytes);
    encrypted.ciphertext = std::move(frame.payload);
    std::optional<crypto::Bytes> decoded = session.open(encrypted);
    if (!decoded) return false;
    plaintext = std::move(*decoded);
    return true;
}

bool send_host_hello(int fd, const crypto::Nonce32& host_nonce) {
    protocol::Frame hello;
    hello.type = protocol::FrameType::HELLO;
    hello.payload.reserve(kHostHelloBytes);
    hello.payload.push_back(static_cast<std::uint8_t>(protocol::kProtocolVersion >> 8));
    hello.payload.push_back(static_cast<std::uint8_t>(protocol::kProtocolVersion));
    hello.payload.insert(hello.payload.end(), host_nonce.begin(), host_nonce.end());
    return send_plain_frame(fd, hello);
}

bool read_device_hello(int fd, crypto::DeviceHello& device_hello) {
    protocol::Frame frame;
    protocol::FrameHeader header;
    if (!receive_frame(fd, frame, header) || frame.type != protocol::FrameType::HELLO ||
        header.flags != 0 || header.stream_id != 0 || header.sequence != 0 ||
        header.reserved != 0 || frame.payload.size() != kDeviceHelloBytes) {
        return false;
    }
    std::copy_n(frame.payload.begin(), device_hello.device_id.size(), device_hello.device_id.begin());
    std::copy_n(frame.payload.begin() + static_cast<std::ptrdiff_t>(device_hello.device_id.size()),
                device_hello.nonce.size(), device_hello.nonce.begin());
    return true;
}

bool authenticate(int fd, std::string_view key_directory, const crypto::Nonce32& host_nonce,
                  const crypto::DeviceHello& device_hello,
                  std::optional<crypto::Session>& session, std::string* error) {
    crypto::Bytes psk;
    BytesWiper psk_wiper(psk);
    if (!load_device_key(key_directory, device_hello.device_id, psk)) {
        set_error(error, "MTPADB device key is unavailable or invalid");
        return false;
    }

    crypto::HostHandshake handshake(std::move(psk), host_nonce);
    const crypto::HostHello started = handshake.hello();
    if (started.version != protocol::kProtocolVersion || started.nonce != host_nonce) return false;

    crypto::Bytes auth = handshake.make_auth(device_hello);
    BytesWiper auth_wiper(auth);
    crypto::Session established = handshake.establish(device_hello);
    if (!send_auth_frame(fd, std::move(auth))) return false;
    session = std::move(established);
    return true;
}

bool run_ping(int fd, crypto::Session& session) {
    protocol::FrameHeader request{};
    request.type = protocol::FrameType::PING;
    if (!send_encrypted_frame(fd, session, request, {})) return false;

    protocol::FrameHeader response{};
    crypto::Bytes payload;
    return receive_encrypted_frame(fd, session, response, payload) &&
           response.type == protocol::FrameType::STATUS && response.flags == 0 &&
           response.stream_id == 0 && response.reserved == 0 &&
           payload == crypto::Bytes{'P', 'O', 'N', 'G'};
}

bool run_echo(int fd, crypto::Session& session, int input_fd, int output_fd) {
    protocol::FrameHeader open_request{};
    open_request.type = protocol::FrameType::OPEN;
    const crypto::Bytes echo_service{'e', 'c', 'h', 'o'};
    if (!send_encrypted_frame(fd, session, open_request, echo_service)) return false;

    protocol::FrameHeader open_response{};
    crypto::Bytes open_payload;
    if (!receive_encrypted_frame(fd, session, open_response, open_payload) ||
        open_response.type != protocol::FrameType::STATUS || open_response.flags != 0 ||
        open_response.stream_id == 0 || open_response.reserved != 0 ||
        open_payload != crypto::Bytes{kSuccess}) {
        return false;
    }
    const std::uint32_t stream_id = open_response.stream_id;

    crypto::Bytes chunk(protocol::kDefaultDataChunkBytes);
    for (;;) {
        ssize_t count;
        do {
            count = ::read(input_fd, chunk.data(), chunk.size());
        } while (count < 0 && errno == EINTR);
        if (count < 0) return false;
        if (count == 0) break;
        chunk.resize(static_cast<std::size_t>(count));

        protocol::FrameHeader tx{};
        tx.type = protocol::FrameType::TX;
        tx.stream_id = stream_id;
        if (!send_encrypted_frame(fd, session, tx, chunk)) return false;

        protocol::FrameHeader rx{};
        crypto::Bytes echoed;
        if (!receive_encrypted_frame(fd, session, rx, echoed) ||
            rx.type != protocol::FrameType::RX || rx.flags != 0 || rx.stream_id != stream_id ||
            rx.reserved != 0 || echoed != chunk ||
            !write_all(output_fd, echoed.data(), echoed.size())) {
            return false;
        }
        chunk.resize(protocol::kDefaultDataChunkBytes);
    }

    protocol::FrameHeader close_request{};
    close_request.type = protocol::FrameType::CLOSE;
    close_request.stream_id = stream_id;
    if (!send_encrypted_frame(fd, session, close_request, {})) return false;

    protocol::FrameHeader close_response{};
    crypto::Bytes close_payload;
    return receive_encrypted_frame(fd, session, close_response, close_payload) &&
           close_response.type == protocol::FrameType::STATUS && close_response.flags == 0 &&
           close_response.stream_id == stream_id && close_response.reserved == 0 &&
           close_payload == crypto::Bytes{kSuccess};
}

bool run_rpc_impl(int fd, RpcOperation operation, int input_fd, int output_fd,
                  std::string_view key_directory, std::string* error) {
    if (fd < 0 || key_directory.empty() ||
        (operation == RpcOperation::kPing && (input_fd != -1 || output_fd != -1)) ||
        (operation == RpcOperation::kEcho && (input_fd < 0 || output_fd < 0)) ||
        (operation != RpcOperation::kPing && operation != RpcOperation::kEcho)) {
        set_error(error, "MTPADB RPC failed");
        return false;
    }

    crypto::Nonce32 host_nonce{};
    if (RAND_bytes(host_nonce.data(), static_cast<int>(host_nonce.size())) != 1 ||
        !send_host_hello(fd, host_nonce)) {
        set_error(error, "MTPADB RPC failed");
        return false;
    }

    crypto::DeviceHello device_hello;
    if (!read_device_hello(fd, device_hello)) {
        set_error(error, "MTPADB RPC failed");
        return false;
    }

    std::optional<crypto::Session> session;
    if (!authenticate(fd, key_directory, host_nonce, device_hello, session, error)) {
        if (error == nullptr || *error != "MTPADB device key is unavailable or invalid") {
            set_error(error, "MTPADB RPC failed");
        }
        return false;
    }

    const bool success = operation == RpcOperation::kPing
                             ? run_ping(fd, *session)
                             : run_echo(fd, *session, input_fd, output_fd);
    if (!success) set_error(error, "MTPADB RPC failed");
    return success;
}

}  // namespace

bool run_rpc(int adb_service_fd, RpcOperation operation, int input_fd, int output_fd,
             std::string_view device_keys_directory, std::string* error) {
    if (error != nullptr) error->clear();
    try {
        return run_rpc_impl(adb_service_fd, operation, input_fd, output_fd,
                            device_keys_directory, error);
    } catch (...) {
        set_error(error, "MTPADB RPC failed");
        return false;
    }
}

}  // namespace mtpadb::host
