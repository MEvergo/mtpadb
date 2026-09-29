#include "service.h"

#include "mtprpc_protocol.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <fcntl.h>
#include <optional>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace mtpadb::mtprpcd {
namespace {

using protocol::Frame;
using protocol::FrameDecoder;
using protocol::FrameHeader;
using protocol::FrameType;
using protocol::kDefaultDataChunkBytes;
using protocol::kFrameHeaderBytes;
using protocol::kMaxPayloadBytes;
using protocol::kMaxQueueBytes;
using protocol::kMaxStreams;
using crypto::Bytes;
using crypto::DeviceHandshake;
using crypto::DeviceHello;
using crypto::EncryptedFrame;
using crypto::HostHello;
using crypto::Session;

constexpr std::uint8_t kGenericError = 0x01;
constexpr std::uint8_t kSuccess = 0x00;
constexpr std::size_t kAeadTagBytes = 16;
constexpr std::size_t kReadBufferBytes = 64U * 1024U;

class OwnedFd {
public:
    explicit OwnedFd(int fd) noexcept : fd_(fd) {}
    ~OwnedFd() {
        if (fd_ >= 0) close(fd_);
    }

    OwnedFd(const OwnedFd&) = delete;
    OwnedFd& operator=(const OwnedFd&) = delete;

private:
    int fd_;
};

bool wait_for(int fd, short events) noexcept {
    pollfd descriptor{fd, events, 0};
    for (;;) {
        const int result = poll(&descriptor, 1, -1);
        if (result > 0) {
            return (descriptor.revents & POLLNVAL) == 0 &&
                   (descriptor.revents & events) != 0;
        }
        if (result == 0) return false;
        if (errno != EINTR) return false;
    }
}

bool receive_exact(int fd, std::uint8_t* bytes, std::size_t size) noexcept {
    std::size_t received = 0;
    while (received < size) {
        const ssize_t count = recv(fd, bytes + received, size - received, 0);
        if (count > 0) {
            received += static_cast<std::size_t>(count);
            continue;
        }
        if (count == 0) return false;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (!wait_for(fd, POLLIN)) return false;
            continue;
        }
        return false;
    }
    return true;
}

bool send_all(int fd, const std::uint8_t* bytes, std::size_t size) noexcept {
    std::size_t sent = 0;
    while (sent < size) {
        const ssize_t count = send(fd, bytes + sent, size - sent, MSG_NOSIGNAL);
        if (count > 0) {
            sent += static_cast<std::size_t>(count);
            continue;
        }
        if (count == 0) return false;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (!wait_for(fd, POLLOUT)) return false;
            continue;
        }
        return false;
    }
    return true;
}

bool read_frame(int fd, Frame& frame) {
    std::array<std::uint8_t, kFrameHeaderBytes> wire_header{};
    if (!receive_exact(fd, wire_header.data(), wire_header.size())) return false;

    const FrameHeader header = protocol::decode_header(wire_header.data(), wire_header.size());
    frame.type = header.type;
    frame.flags = header.flags;
    frame.stream_id = header.stream_id;
    frame.sequence = header.sequence;
    frame.reserved = header.reserved;
    frame.payload.resize(header.payload_length);
    if (!frame.payload.empty() &&
        !receive_exact(fd, frame.payload.data(), frame.payload.size())) {
        return false;
    }
    return true;
}

bool send_plain_frame(int fd, const Frame& frame) {
    const Bytes bytes = protocol::encode_frame(frame);
    return send_all(fd, bytes.data(), bytes.size());
}

bool send_plain_error(int fd) {
    Frame frame;
    frame.type = FrameType::ERROR;
    frame.payload = Bytes{kGenericError};
    return send_plain_frame(fd, frame);
}

bool is_handshake_control(const Frame& frame, FrameType type) noexcept {
    return frame.type == type && frame.flags == 0 && frame.stream_id == 0 &&
           frame.sequence == 0 && frame.reserved == 0;
}

bool set_nonblocking(int fd) noexcept {
    const int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

class ActiveStreams {
public:
    bool contains(std::uint32_t id) const noexcept {
        return id != 0 && std::find(ids_.begin(), ids_.end(), id) != ids_.end();
    }

    std::uint32_t allocate() noexcept {
        if (size_ == ids_.size()) return 0;
        for (std::size_t attempt = 0; attempt <= ids_.size(); ++attempt) {
            const std::uint32_t candidate = next_id_++;
            if (candidate != 0 && !contains(candidate)) {
                const auto free_slot = std::find(ids_.begin(), ids_.end(), 0U);
                *free_slot = candidate;
                ++size_;
                return candidate;
            }
        }
        return 0;
    }

    bool release(std::uint32_t id) noexcept {
        const auto found = std::find(ids_.begin(), ids_.end(), id);
        if (id == 0 || found == ids_.end()) return false;
        *found = 0;
        --size_;
        return true;
    }

private:
    std::array<std::uint32_t, kMaxStreams> ids_{};
    std::size_t size_ = 0;
    std::uint32_t next_id_ = 1;
};

struct PendingWrite {
    Bytes bytes;
    std::size_t offset = 0;
};

class AuthenticatedConnection {
public:
    AuthenticatedConnection(int fd, Session session)
        : fd_(fd), session_(std::move(session)) {}

    void run() {
        if (!set_nonblocking(fd_)) return;
        std::array<std::uint8_t, kReadBufferBytes> input{};
        for (;;) {
            if (close_after_flush_ && output_.empty()) return;
            short events = close_after_flush_ ? 0 : POLLIN;
            if (!output_.empty()) events = static_cast<short>(events | POLLOUT);
            if (events == 0) return;

            pollfd descriptor{fd_, events, 0};
            int result;
            do {
                result = poll(&descriptor, 1, -1);
            } while (result < 0 && errno == EINTR);
            if (result <= 0 || (descriptor.revents & (POLLERR | POLLNVAL)) != 0) return;

            if (!close_after_flush_ && (descriptor.revents & POLLIN) != 0) {
                const ssize_t count = recv(fd_, input.data(), input.size(), 0);
                if (count == 0) return;
                if (count < 0) {
                    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
                    return;
                }
                if (!process_input(input.data(), static_cast<std::size_t>(count))) return;
            }

            if ((descriptor.revents & POLLOUT) != 0 && !flush_output()) return;
            if ((descriptor.revents & POLLHUP) != 0) return;
        }
    }

private:
    bool process_input(const std::uint8_t* bytes, std::size_t size) {
        std::vector<Frame> frames = decoder_.feed(bytes, size);
        for (Frame& frame : frames) {
            if (!process_frame(frame)) return false;
            if (close_after_flush_) break;
        }
        return true;
    }

    bool process_frame(Frame& frame) {
        if (frame.flags != 0 || frame.payload.size() < kAeadTagBytes) {
            return reject_and_close();
        }

        EncryptedFrame encrypted;
        encrypted.header.type = frame.type;
        encrypted.header.flags = frame.flags;
        encrypted.header.stream_id = frame.stream_id;
        encrypted.header.sequence = frame.sequence;
        encrypted.header.payload_length = static_cast<std::uint32_t>(frame.payload.size());
        encrypted.header.reserved = frame.reserved;
        std::copy(frame.payload.end() - kAeadTagBytes, frame.payload.end(), encrypted.tag.begin());
        encrypted.ciphertext = std::move(frame.payload);
        encrypted.ciphertext.resize(encrypted.ciphertext.size() - kAeadTagBytes);

        std::optional<Bytes> plaintext = session_.open(encrypted);
        if (!plaintext) return reject_and_close();
        return dispatch(encrypted.header, *plaintext);
    }

    bool dispatch(const FrameHeader& request, const Bytes& payload) {
        switch (request.type) {
            case FrameType::PING:
                if (request.stream_id != 0 || !payload.empty()) return reject_and_close();
                return queue_encrypted(FrameType::STATUS, 0, Bytes{'P', 'O', 'N', 'G'});

            case FrameType::OPEN: {
                if (request.stream_id != 0 || payload != Bytes{'e', 'c', 'h', 'o'}) {
                    return reject_and_close();
                }
                const std::uint32_t stream_id = streams_.allocate();
                if (stream_id == 0) {
                    return queue_limit_error();
                }
                if (!queue_encrypted(FrameType::STATUS, stream_id, Bytes{kSuccess})) return false;
                return true;
            }

            case FrameType::TX:
                if (!streams_.contains(request.stream_id) ||
                    payload.size() > kDefaultDataChunkBytes) {
                    return reject_and_close();
                }
                return queue_encrypted(FrameType::RX, request.stream_id, payload);

            case FrameType::CLOSE:
                if (!payload.empty() || !streams_.release(request.stream_id)) {
                    return reject_and_close();
                }
                return queue_encrypted(FrameType::STATUS, request.stream_id, Bytes{kSuccess});

            case FrameType::HELLO:
            case FrameType::AUTH:
            case FrameType::RX:
            case FrameType::STATUS:
            case FrameType::ERROR:
                return reject_and_close();
        }
        return reject_and_close();
    }

    bool reject_and_close() {
        if (!queue_encrypted(FrameType::ERROR, 0, Bytes{kGenericError})) return false;
        close_after_flush_ = true;
        return true;
    }

    bool queue_limit_error() {
        return queue_encrypted(FrameType::ERROR, 0, Bytes{kGenericError});
    }

    bool queue_encrypted(FrameType type, std::uint32_t stream_id, const Bytes& plaintext) {
        if (plaintext.size() > kMaxPayloadBytes - kAeadTagBytes) return false;
        const std::size_t wire_size = kFrameHeaderBytes + plaintext.size() + kAeadTagBytes;
        if (wire_size > kMaxQueueBytes - queued_bytes_) return false;

        FrameHeader header;
        header.type = type;
        header.stream_id = stream_id;
        EncryptedFrame encrypted = session_.seal(header, plaintext);
        const auto wire_header = protocol::serialize_header(encrypted.header);
        Bytes wire(wire_size);
        std::copy(wire_header.begin(), wire_header.end(), wire.begin());
        auto output = wire.begin() + kFrameHeaderBytes;
        output = std::copy(encrypted.ciphertext.begin(), encrypted.ciphertext.end(), output);
        std::copy(encrypted.tag.begin(), encrypted.tag.end(), output);
        output_.push_back(PendingWrite{std::move(wire), 0});
        queued_bytes_ += wire_size;
        return true;
    }

    bool flush_output() noexcept {
        while (!output_.empty()) {
            PendingWrite& pending = output_.front();
            const std::size_t remaining = pending.bytes.size() - pending.offset;
            const ssize_t count = send(fd_, pending.bytes.data() + pending.offset, remaining,
                                       MSG_NOSIGNAL);
            if (count > 0) {
                pending.offset += static_cast<std::size_t>(count);
                if (pending.offset == pending.bytes.size()) {
                    queued_bytes_ -= pending.bytes.size();
                    output_.pop_front();
                }
                continue;
            }
            if (count == 0) return false;
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
            return false;
        }
        return true;
    }

    int fd_;
    Session session_;
    FrameDecoder decoder_;
    ActiveStreams streams_;
    std::deque<PendingWrite> output_;
    std::size_t queued_bytes_ = 0;
    bool close_after_flush_ = false;
};

void serve_authenticated(int fd, Session session) {
    AuthenticatedConnection(fd, std::move(session)).run();
}

void serve(int fd, const crypto::DeviceId& device_id, const Bytes& psk) {
    Frame hello_frame;

    if (!read_frame(fd, hello_frame) ||
        !is_handshake_control(hello_frame, FrameType::HELLO) ||
        hello_frame.payload.size() != 34) {
        return;
    }

    HostHello host_hello;
    host_hello.version = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(hello_frame.payload[0]) << 8U) |
        static_cast<std::uint16_t>(hello_frame.payload[1]));
    std::copy_n(hello_frame.payload.begin() + 2, host_hello.nonce.size(), host_hello.nonce.begin());

    DeviceHandshake handshake(psk, device_id);
    const DeviceHello device_hello = handshake.hello(host_hello);
    Frame response;
    response.type = FrameType::HELLO;
    response.payload.reserve(device_hello.device_id.size() + device_hello.nonce.size());
    response.payload.insert(response.payload.end(), device_hello.device_id.begin(),
                            device_hello.device_id.end());
    response.payload.insert(response.payload.end(), device_hello.nonce.begin(),
                            device_hello.nonce.end());
    if (!send_plain_frame(fd, response)) return;

    Frame auth_frame;
    if (!read_frame(fd, auth_frame) || !is_handshake_control(auth_frame, FrameType::AUTH)) return;
    std::optional<Session> session = handshake.authenticate(host_hello, auth_frame.payload);
    if (!session) {
        (void)send_plain_error(fd);
        return;
    }
    serve_authenticated(fd, std::move(*session));
}

}  // namespace

void serve_connection(int connected_fd, const crypto::DeviceId& device_id,
                      const crypto::Bytes& psk) {
    OwnedFd owned_fd(connected_fd);
    if (connected_fd < 0) return;
    try {
        serve(connected_fd, device_id, psk);
    } catch (...) {
        // The connection is intentionally closed without exposing protocol or secret details.
    }
}

}  // namespace mtpadb::mtprpcd
