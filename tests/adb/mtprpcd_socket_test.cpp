#include "mtprpcd/service.h"

#include "mtprpc_protocol.h"
#include "mtprpc_session.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <iostream>
#include <optional>
#include <poll.h>
#include <stdexcept>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

using mtpadb::mtprpcd::serve_connection;
using mtpadb::protocol::Frame;
using mtpadb::protocol::FrameHeader;
using mtpadb::protocol::FrameType;
using mtpadb::protocol::kDefaultDataChunkBytes;
using mtpadb::protocol::kFrameHeaderBytes;
using mtpadb::protocol::kMaxPayloadBytes;
using mtpadb::protocol::crypto::Bytes;
using mtpadb::protocol::crypto::DeviceHello;
using mtpadb::protocol::crypto::DeviceId;
using mtpadb::protocol::crypto::HostHandshake;
using mtpadb::protocol::crypto::Session;
using Clock = std::chrono::steady_clock;
using Deadline = Clock::time_point;

constexpr auto kIoTimeout = std::chrono::seconds(10);
constexpr auto kRejectTimeout = std::chrono::seconds(2);
constexpr auto kHandshakeTestTimeout = std::chrono::milliseconds(175);
constexpr auto kHandshakeDeadlineSlack = std::chrono::milliseconds(75);
constexpr auto kPartialHelloInterval = std::chrono::milliseconds(40);
constexpr std::uint8_t kGenericError = 0x01;
constexpr std::uint8_t kSuccess = 0x00;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

Bytes test_psk() {
    return Bytes(32, 0x5a);
}

DeviceId test_device_id() {
    DeviceId id{};
    for (std::size_t i = 0; i < id.size(); ++i) {
        id[i] = static_cast<std::uint8_t>(0x30U + i);
    }
    return id;
}

int remaining_milliseconds(Deadline deadline) {
    const auto remaining = deadline - Clock::now();
    if (remaining <= Clock::duration::zero()) return 0;
    auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count();
    if (milliseconds == 0) milliseconds = 1;
    if (milliseconds > 2147483647) milliseconds = 2147483647;
    return static_cast<int>(milliseconds);
}

int wait_for(int fd, short events, Deadline deadline) {
    pollfd descriptor{fd, events, 0};
    for (;;) {
        const int timeout = remaining_milliseconds(deadline);
        if (timeout == 0) return 0;
        const int result = poll(&descriptor, 1, timeout);
        if (result > 0) return descriptor.revents;
        if (result == 0) return 0;
        if (errno != EINTR) throw std::runtime_error("poll failed on socketpair");
    }
}

enum class IoResult { kComplete, kClosed, kTimedOut };

enum class ReadResult { kFrame, kClosed, kTimedOut };

IoResult write_all_until(int fd, const std::uint8_t* bytes, std::size_t size,
                         Deadline deadline) {
    std::size_t written = 0;
    while (written < size) {
        if (Clock::now() >= deadline) return IoResult::kTimedOut;
        const ssize_t count = send(fd, bytes + written, size - written, MSG_NOSIGNAL);
        if (count > 0) {
            written += static_cast<std::size_t>(count);
            continue;
        }
        if (count == 0) return IoResult::kClosed;
        if (errno == EINTR) continue;
        if (errno == EPIPE || errno == ECONNRESET || errno == ENOTCONN) {
            return IoResult::kClosed;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            throw std::runtime_error("send failed on socketpair");
        }

        const int events = wait_for(fd, POLLOUT, deadline);
        if (events == 0) return IoResult::kTimedOut;
        if ((events & (POLLERR | POLLHUP | POLLNVAL)) != 0) return IoResult::kClosed;
    }
    return IoResult::kComplete;
}

void send_bytes(int fd, const std::uint8_t* bytes, std::size_t size,
                std::chrono::milliseconds timeout = kIoTimeout) {
    const IoResult result = write_all_until(fd, bytes, size, Clock::now() + timeout);
    require(result == IoResult::kComplete, "timed out or disconnected while sending socket frame");
}

void send_frame(int fd, const Frame& frame,
                std::chrono::milliseconds timeout = kIoTimeout) {
    const auto wire = mtpadb::protocol::encode_frame(frame);
    send_bytes(fd, wire.data(), wire.size(), timeout);
}

IoResult read_exact_until(int fd, std::uint8_t* bytes, std::size_t size,
                          Deadline deadline, std::size_t& received) {
    received = 0;
    while (received < size) {
        const int events = wait_for(fd, POLLIN, deadline);
        if (events == 0) return IoResult::kTimedOut;
        const ssize_t count = recv(fd, bytes + received, size - received, 0);
        if (count > 0) {
            received += static_cast<std::size_t>(count);
            continue;
        }
        if (count == 0) return IoResult::kClosed;
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
        if (errno == ECONNRESET || errno == ENOTCONN) return IoResult::kClosed;
        throw std::runtime_error("recv failed on socketpair");
    }
    return IoResult::kComplete;
}

struct FrameRead {
    ReadResult result = ReadResult::kTimedOut;
    Frame frame{};
};

FrameRead read_frame_until(int fd, Deadline deadline) {
    std::array<std::uint8_t, kFrameHeaderBytes> wire_header{};
    std::size_t received = 0;
    const IoResult header_result =
        read_exact_until(fd, wire_header.data(), wire_header.size(), deadline, received);
    if (header_result != IoResult::kComplete) {
        if (received != 0) throw std::runtime_error("connection ended during frame header");
        return {header_result == IoResult::kClosed ? ReadResult::kClosed : ReadResult::kTimedOut,
                {}};
    }

    const auto header = mtpadb::protocol::decode_header(wire_header.data(), wire_header.size());
    require(header.payload_length <= kMaxPayloadBytes,
            "service response declared an oversized frame payload");
    Bytes payload(header.payload_length);
    if (!payload.empty()) {
        received = 0;
        const IoResult payload_result =
            read_exact_until(fd, payload.data(), payload.size(), deadline, received);
        if (payload_result != IoResult::kComplete) {
            throw std::runtime_error("connection ended during frame payload");
        }
    }

    Frame frame;
    frame.type = header.type;
    frame.flags = header.flags;
    frame.stream_id = header.stream_id;
    frame.sequence = header.sequence;
    frame.reserved = header.reserved;
    frame.payload = std::move(payload);
    return {ReadResult::kFrame, std::move(frame)};
}

Frame require_frame(int fd, std::chrono::milliseconds timeout = kIoTimeout) {
    FrameRead result = read_frame_until(fd, Clock::now() + timeout);
    require(result.result == ReadResult::kFrame, "expected a frame before the socket deadline");
    return std::move(result.frame);
}

class SocketService {
public:
    explicit SocketService(
        const Bytes& psk, int service_send_buffer = 0, int client_receive_buffer = 0,
        int service_receive_buffer = 0, int client_send_buffer = 0,
        std::optional<std::chrono::milliseconds> handshake_timeout = std::nullopt)
        : psk_(psk), device_id_(test_device_id()) {
        int sockets[2] = {-1, -1};
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
            throw std::runtime_error("socketpair creation failed");
        }
        client_fd_ = sockets[0];
        const int service_fd = sockets[1];
        try {
            if (service_send_buffer > 0 &&
                setsockopt(service_fd, SOL_SOCKET, SO_SNDBUF, &service_send_buffer,
                           sizeof(service_send_buffer)) != 0) {
                throw std::runtime_error("could not constrain service socket send buffer");
            }
            if (client_receive_buffer > 0 &&
                setsockopt(client_fd_, SOL_SOCKET, SO_RCVBUF, &client_receive_buffer,
                           sizeof(client_receive_buffer)) != 0) {
                throw std::runtime_error("could not constrain client socket receive buffer");
            }
            if (service_receive_buffer > 0 &&
                setsockopt(service_fd, SOL_SOCKET, SO_RCVBUF, &service_receive_buffer,
                           sizeof(service_receive_buffer)) != 0) {
                throw std::runtime_error("could not constrain service socket receive buffer");
            }
            if (client_send_buffer > 0 &&
                setsockopt(client_fd_, SOL_SOCKET, SO_SNDBUF, &client_send_buffer,
                           sizeof(client_send_buffer)) != 0) {
                throw std::runtime_error("could not constrain client socket send buffer");
            }
            const int flags = fcntl(client_fd_, F_GETFL, 0);
            if (flags < 0 || fcntl(client_fd_, F_SETFL, flags | O_NONBLOCK) != 0) {
                throw std::runtime_error("could not make test socket nonblocking");
            }
            worker_ = std::thread([this, service_fd, handshake_timeout] {
                try {
                    if (handshake_timeout) {
                        serve_connection(service_fd, device_id_, psk_, *handshake_timeout);
                    } else {
                        serve_connection(service_fd, device_id_, psk_);
                    }
                } catch (...) {
                    shutdown(service_fd, SHUT_RDWR);
                    close(service_fd);
                }
            });
        } catch (...) {
            close(client_fd_);
            client_fd_ = -1;
            close(service_fd);
            throw;
        }
    }

    SocketService(const SocketService&) = delete;
    SocketService& operator=(const SocketService&) = delete;

    ~SocketService() {
        if (client_fd_ >= 0) {
            shutdown(client_fd_, SHUT_RDWR);
            close(client_fd_);
        }
        if (worker_.joinable()) worker_.join();
    }

    int fd() const noexcept { return client_fd_; }

private:
    Bytes psk_;
    DeviceId device_id_{};
    int client_fd_ = -1;
    std::thread worker_;
};

Session authenticate(SocketService& service, const Bytes& host_psk) {
    mtpadb::protocol::crypto::Nonce32 host_nonce{};
    for (std::size_t i = 0; i < host_nonce.size(); ++i) {
        host_nonce[i] = static_cast<std::uint8_t>(0xa0U + i);
    }
    HostHandshake handshake(host_psk, host_nonce);
    const auto host_hello = handshake.hello();

    Bytes hello_payload;
    hello_payload.push_back(static_cast<std::uint8_t>(host_hello.version >> 8U));
    hello_payload.push_back(static_cast<std::uint8_t>(host_hello.version & 0xffU));
    hello_payload.insert(hello_payload.end(), host_hello.nonce.begin(), host_hello.nonce.end());
    send_frame(service.fd(), Frame{FrameType::HELLO, 0, 0, 0, 0, std::move(hello_payload)});

    Frame device_frame = require_frame(service.fd());
    require(device_frame.type == FrameType::HELLO, "device did not answer with HELLO");
    require(device_frame.stream_id == 0, "device HELLO used a nonzero stream");
    require(device_frame.payload.size() == 48, "device HELLO payload had the wrong size");
    DeviceHello device_hello{};
    std::copy_n(device_frame.payload.begin(), device_hello.device_id.size(),
                device_hello.device_id.begin());
    std::copy_n(device_frame.payload.begin() + device_hello.device_id.size(),
                device_hello.nonce.size(), device_hello.nonce.begin());
    require(device_hello.device_id == test_device_id(),
            "device HELLO returned an unexpected device ID");

    Bytes auth = handshake.make_auth(device_hello);
    send_frame(service.fd(), Frame{FrameType::AUTH, 0, 0, 0, 0, std::move(auth)});
    return handshake.establish(device_hello);
}

Frame encrypted_request(Session& session, FrameType type, std::uint32_t stream_id,
                        const Bytes& plaintext) {
    FrameHeader header{};
    header.type = type;
    header.stream_id = stream_id;
    auto encrypted = session.seal(header, plaintext);

    Frame frame;
    frame.type = encrypted.header.type;
    frame.flags = encrypted.header.flags;
    frame.stream_id = encrypted.header.stream_id;
    frame.sequence = encrypted.header.sequence;
    frame.reserved = encrypted.header.reserved;
    frame.payload = std::move(encrypted.ciphertext);
    frame.payload.insert(frame.payload.end(), encrypted.tag.begin(), encrypted.tag.end());
    return frame;
}

Bytes open_response(Session& session, const Frame& frame) {
    require(frame.payload.size() >= 16, "encrypted response omitted its authentication tag");
    mtpadb::protocol::crypto::EncryptedFrame encrypted;
    encrypted.header.type = frame.type;
    encrypted.header.flags = frame.flags;
    encrypted.header.stream_id = frame.stream_id;
    encrypted.header.sequence = frame.sequence;
    encrypted.header.payload_length = static_cast<std::uint32_t>(frame.payload.size());
    encrypted.header.reserved = frame.reserved;
    const std::size_t ciphertext_size = frame.payload.size() - encrypted.tag.size();
    encrypted.ciphertext.assign(frame.payload.begin(), frame.payload.begin() + ciphertext_size);
    std::copy(frame.payload.begin() + ciphertext_size, frame.payload.end(), encrypted.tag.begin());
    auto plaintext = session.open(encrypted);
    require(plaintext.has_value(), "service response failed authenticated session verification");
    return std::move(*plaintext);
}

Bytes receive_response(SocketService& service, Session& session, FrameType expected_type,
                       std::uint32_t expected_stream, std::uint64_t& expected_sequence) {
    Frame frame = require_frame(service.fd());
    require(frame.type == expected_type, "service returned an unexpected frame type");
    require(frame.stream_id == expected_stream, "service changed the response stream ID");
    require(frame.sequence == expected_sequence, "device-to-host sequence was not monotonic");
    ++expected_sequence;
    return open_response(session, frame);
}

void expect_closed(SocketService& service,
                   std::chrono::milliseconds timeout = kRejectTimeout) {
    const FrameRead result = read_frame_until(service.fd(), Clock::now() + timeout);
    require(result.result == ReadResult::kClosed, "service did not close after rejection");
}

void expect_closed_by(SocketService& service, Deadline deadline) {
    const FrameRead result = read_frame_until(service.fd(), deadline);
    require(result.result == ReadResult::kClosed,
            "unauthenticated connection exceeded its absolute handshake deadline");
}

void test_idle_peer_is_closed_by_handshake_deadline() {
    const Bytes psk = test_psk();
    const Deadline deadline =
        Clock::now() + kHandshakeTestTimeout + kHandshakeDeadlineSlack;
    SocketService service(psk, 0, 0, 0, 0, kHandshakeTestTimeout);
    expect_closed_by(service, deadline);
}

void test_partial_hello_progress_does_not_reset_handshake_deadline() {
    const Bytes psk = test_psk();
    const Deadline handshake_deadline = Clock::now() + kHandshakeTestTimeout;
    const Deadline close_deadline = handshake_deadline + kHandshakeDeadlineSlack;
    SocketService service(psk, 0, 0, 0, 0, kHandshakeTestTimeout);

    Frame hello;
    hello.type = FrameType::HELLO;
    hello.payload = Bytes(34, 0);
    const Bytes wire = mtpadb::protocol::encode_frame(hello);
    constexpr std::size_t kPartialPayloadBytes = 4;
    send_bytes(service.fd(), wire.data(), kFrameHeaderBytes + 1);
    for (std::size_t received = 1; received < kPartialPayloadBytes; ++received) {
        std::this_thread::sleep_for(kPartialHelloInterval);
        require(Clock::now() < handshake_deadline,
                "test did not send partial HELLO progress before its deadline");
        send_bytes(service.fd(), wire.data() + kFrameHeaderBytes + received, 1);
    }

    expect_closed_by(service, close_deadline);
}

void test_authenticated_session_survives_handshake_deadline() {
    const Bytes psk = test_psk();
    SocketService service(psk, 0, 0, 0, 0, kHandshakeTestTimeout);
    Session session = authenticate(service, psk);
    std::this_thread::sleep_for(kHandshakeTestTimeout + std::chrono::milliseconds(25));

    send_frame(service.fd(), encrypted_request(session, FrameType::PING, 0, {}));
    std::uint64_t device_sequence = 0;
    const Bytes pong = receive_response(service, session, FrameType::STATUS, 0, device_sequence);
    require(pong == Bytes{'P', 'O', 'N', 'G'},
            "authenticated session was terminated by the handshake deadline");
}

void require_plaintext_rejection(SocketService& service) {
    const FrameRead result = read_frame_until(service.fd(), Clock::now() + kRejectTimeout);
    if (result.result == ReadResult::kClosed) return;
    require(result.result == ReadResult::kFrame, "authentication rejection was not bounded");
    require(result.frame.type == FrameType::ERROR, "wrong PSK did not produce generic ERROR");
    require(result.frame.payload == Bytes{kGenericError}, "wrong PSK error was not generic");
    expect_closed(service);
}

void require_authenticated_rejection(SocketService& service, Session& session,
                                     std::uint64_t expected_sequence) {
    const FrameRead result = read_frame_until(service.fd(), Clock::now() + kRejectTimeout);
    if (result.result == ReadResult::kClosed) return;
    require(result.result == ReadResult::kFrame, "invalid request rejection was not bounded");
    require(result.frame.type == FrameType::ERROR,
            "invalid request produced a data response instead of ERROR/close");
    require(result.frame.stream_id == 0, "generic ERROR used a nonzero stream");
    require(result.frame.sequence == expected_sequence,
            "generic ERROR did not preserve device-to-host sequence ordering");
    const Bytes payload = open_response(session, result.frame);
    require(payload == Bytes{kGenericError}, "authenticated ERROR was not generic");
    expect_closed(service);
}

void test_handshake_ping_echo_and_chunked_roundtrip() {
    const Bytes psk = test_psk();
    SocketService service(psk);
    Session session = authenticate(service, psk);
    std::uint64_t device_sequence = 0;

    Frame ping = encrypted_request(session, FrameType::PING, 0, {});
    send_frame(service.fd(), ping);
    const Bytes pong = receive_response(service, session, FrameType::STATUS, 0, device_sequence);
    require(pong == Bytes{'P', 'O', 'N', 'G'}, "PING did not return the exact PONG payload");

    const Bytes echo_name{'e', 'c', 'h', 'o'};
    send_frame(service.fd(), encrypted_request(session, FrameType::OPEN, 0, echo_name));
    Frame open_response_frame = require_frame(service.fd());
    require(open_response_frame.type == FrameType::STATUS,
            "OPEN did not return STATUS");
    require(open_response_frame.stream_id != 0, "OPEN did not allocate a nonzero stream");
    require(open_response_frame.sequence == device_sequence,
            "OPEN response sequence was not monotonic");
    const std::uint32_t echo_stream = open_response_frame.stream_id;
    ++device_sequence;
    require(open_response(session, open_response_frame) == Bytes{kSuccess},
            "OPEN did not report success");

    const Bytes binary_echo{0x00, 0x41, 0x00, 0xff, 0x12, 0x00, 0x7f};
    send_frame(service.fd(), encrypted_request(session, FrameType::TX, echo_stream, binary_echo));
    require(receive_response(service, session, FrameType::RX, echo_stream, device_sequence) ==
                binary_echo,
            "echo stream changed binary payload bytes");

    Bytes one_megabyte(1024U * 1024U);
    for (std::size_t i = 0; i < one_megabyte.size(); ++i) {
        one_megabyte[i] = static_cast<std::uint8_t>((i * 37U + i / 251U) & 0xffU);
    }
    one_megabyte[0] = 0;
    one_megabyte[1] = 0xff;
    one_megabyte[257] = 0;
    one_megabyte.back() = 0;
    for (std::size_t offset = 0; offset < one_megabyte.size();
         offset += kDefaultDataChunkBytes) {
        const Bytes chunk(one_megabyte.begin() + offset,
                          one_megabyte.begin() + offset + kDefaultDataChunkBytes);
        send_frame(service.fd(), encrypted_request(session, FrameType::TX, echo_stream, chunk));
        const Bytes echoed =
            receive_response(service, session, FrameType::RX, echo_stream, device_sequence);
        require(echoed == chunk, "256 KiB echo chunk did not roundtrip exactly");
    }

    send_frame(service.fd(), encrypted_request(session, FrameType::CLOSE, echo_stream, {}));
    require(receive_response(service, session, FrameType::STATUS, echo_stream, device_sequence) ==
                Bytes{kSuccess},
            "CLOSE did not report success");
}

std::uint32_t open_echo_stream(SocketService& service, Session& session,
                               std::uint64_t& device_sequence) {
    send_frame(service.fd(), encrypted_request(session, FrameType::OPEN, 0,
                                               Bytes{'e', 'c', 'h', 'o'}));
    Frame response = require_frame(service.fd());
    require(response.type == FrameType::STATUS, "OPEN did not return STATUS");
    require(response.stream_id != 0, "OPEN did not allocate a nonzero stream");
    require(response.sequence == device_sequence, "OPEN response sequence was not monotonic");
    ++device_sequence;
    require(open_response(session, response) == Bytes{kSuccess}, "OPEN did not report success");
    return response.stream_id;
}

void close_echo_stream(SocketService& service, Session& session, std::uint32_t stream_id,
                       std::uint64_t& device_sequence) {
    send_frame(service.fd(), encrypted_request(session, FrameType::CLOSE, stream_id, {}));
    require(receive_response(service, session, FrameType::STATUS, stream_id, device_sequence) ==
                Bytes{kSuccess},
            "CLOSE did not report success");
}

void test_stream_limit_and_slot_release() {
    const Bytes psk = test_psk();
    SocketService service(psk);
    Session session = authenticate(service, psk);
    std::uint64_t device_sequence = 0;
    std::vector<std::uint32_t> streams;
    streams.reserve(mtpadb::protocol::kMaxStreams);

    for (std::size_t i = 0; i < mtpadb::protocol::kMaxStreams; ++i) {
        const std::uint32_t stream_id = open_echo_stream(service, session, device_sequence);
        require(std::find(streams.begin(), streams.end(), stream_id) == streams.end(),
                "OPEN reused an ID belonging to an active stream");
        streams.push_back(stream_id);
    }

    send_frame(service.fd(), encrypted_request(session, FrameType::OPEN, 0,
                                               Bytes{'e', 'c', 'h', 'o'}));
    require(receive_response(service, session, FrameType::ERROR, 0, device_sequence) ==
                Bytes{kGenericError},
            "17th OPEN did not return a generic control-stream ERROR");

    close_echo_stream(service, session, streams.front(), device_sequence);
    const std::uint32_t replacement = open_echo_stream(service, session, device_sequence);
    require(std::find(streams.begin() + 1, streams.end(), replacement) == streams.end(),
            "replacement OPEN reused an ID belonging to another active stream");

    close_echo_stream(service, session, replacement, device_sequence);
    for (std::size_t i = 1; i < streams.size(); ++i) {
        close_echo_stream(service, session, streams[i], device_sequence);
    }
}

void test_wrong_psk_is_rejected_generically() {
    const Bytes psk = test_psk();
    SocketService service(psk);
    Bytes wrong_psk = psk;
    wrong_psk[0] ^= 0x80;
    (void)authenticate(service, wrong_psk);
    require_plaintext_rejection(service);
}

void test_replayed_request_has_no_second_data_response() {
    const Bytes psk = test_psk();
    SocketService service(psk);
    Session session = authenticate(service, psk);
    std::uint64_t device_sequence = 0;

    const auto ping = encrypted_request(session, FrameType::PING, 0, {});
    const auto replayed_wire = mtpadb::protocol::encode_frame(ping);
    send_bytes(service.fd(), replayed_wire.data(), replayed_wire.size());
    require(receive_response(service, session, FrameType::STATUS, 0, device_sequence) ==
                Bytes{'P', 'O', 'N', 'G'},
            "initial PING did not return PONG");

    send_bytes(service.fd(), replayed_wire.data(), replayed_wire.size());
    require_authenticated_rejection(service, session, device_sequence);
}

std::array<std::uint8_t, kFrameHeaderBytes> oversized_header(Session& session) {
    const Frame sealed_ping = encrypted_request(session, FrameType::PING, 0, {});
    const auto wire = mtpadb::protocol::encode_frame(sealed_ping);
    std::array<std::uint8_t, kFrameHeaderBytes> bytes{};
    std::copy_n(wire.begin(), bytes.size(), bytes.begin());
    const std::uint32_t oversized = static_cast<std::uint32_t>(kMaxPayloadBytes + 1U);
    bytes[24] = static_cast<std::uint8_t>(oversized >> 24U);
    bytes[25] = static_cast<std::uint8_t>(oversized >> 16U);
    bytes[26] = static_cast<std::uint8_t>(oversized >> 8U);
    bytes[27] = static_cast<std::uint8_t>(oversized);
    return bytes;
}

void test_oversized_header_is_rejected_without_payload() {
    const Bytes psk = test_psk();
    SocketService service(psk);
    Session session = authenticate(service, psk);
    const auto header = oversized_header(session);
    send_bytes(service.fd(), header.data(), header.size());
    require_authenticated_rejection(service, session, 0);
}

void test_stalled_reader_is_backpressured_by_session_queue_limit() {
    const Bytes psk = test_psk();
    SocketService service(psk, 4096, 4096, 4096, 4096);
    Session session = authenticate(service, psk);

    send_frame(service.fd(), encrypted_request(session, FrameType::OPEN, 0,
                                               Bytes{'e', 'c', 'h', 'o'}));
    Frame opened = require_frame(service.fd());
    require(opened.type == FrameType::STATUS && opened.stream_id != 0,
            "stalled-reader setup did not open an echo stream");
    require(opened.sequence == 0, "stalled-reader OPEN response had the wrong sequence");
    require(open_response(session, opened) == Bytes{kSuccess},
            "stalled-reader stream did not report success");
    const std::uint32_t stream_id = opened.stream_id;

    constexpr std::size_t kQueuedFrames = 32;
    constexpr auto kBackpressureDeadline = std::chrono::seconds(5);
    const Deadline deadline = Clock::now() + kBackpressureDeadline;
    const Bytes chunk(kDefaultDataChunkBytes, 0x6d);
    std::size_t completed_frames = 0;
    IoResult final_result = IoResult::kComplete;
    for (; completed_frames < kQueuedFrames; ++completed_frames) {
        const auto wire = mtpadb::protocol::encode_frame(
            encrypted_request(session, FrameType::TX, stream_id, chunk));
        final_result = write_all_until(service.fd(), wire.data(), wire.size(), deadline);
        if (final_result != IoResult::kComplete) break;
    }

    const std::size_t completed_payload_bytes = completed_frames * kDefaultDataChunkBytes;
    require(completed_payload_bytes >=
                mtpadb::protocol::kMaxQueueBytes - kDefaultDataChunkBytes,
            "stalled reader caused backpressure before the 4 MiB output budget was approached");
    require(completed_payload_bytes <=
                mtpadb::protocol::kMaxQueueBytes + kDefaultDataChunkBytes,
            "stalled reader exceeded the 4 MiB budget by more than one in-flight chunk");
    require(final_result == IoResult::kClosed || final_result == IoResult::kTimedOut,
            "service accepted an unbounded echo pipeline while the reader was stalled");
}

}  // namespace

int main() {
    try {
        test_handshake_ping_echo_and_chunked_roundtrip();
        test_stream_limit_and_slot_release();
        test_wrong_psk_is_rejected_generically();
        test_replayed_request_has_no_second_data_response();
        test_oversized_header_is_rejected_without_payload();
        test_idle_peer_is_closed_by_handshake_deadline();
        test_partial_hello_progress_does_not_reset_handshake_deadline();
        test_authenticated_session_survives_handshake_deadline();
        test_stalled_reader_is_backpressured_by_session_queue_limit();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
