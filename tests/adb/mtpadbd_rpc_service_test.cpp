#include "adb.h"
#include "adb_unique_fd.h"
#include "transport.h"

#include "mtprpc_protocol.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cerrno>
#include <cstdint>
#include <optional>
#include <poll.h>
#include <type_traits>
#include <vector>

#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

using mtpadb::protocol::Frame;
using mtpadb::protocol::FrameHeader;
using mtpadb::protocol::FrameType;
using mtpadb::protocol::kFrameHeaderBytes;

// Android 11's transport is defaulted to a connection state; modular ADB's
// constructor also requires an explicit transport type.
template <typename Transport>
Transport make_transport() {
    if constexpr (std::is_constructible_v<Transport, TransportType>) {
        return Transport{kTransportAny};
    } else {
        return Transport{kCsOffline};
    }
}

bool wait_readable(int fd) {
    pollfd descriptor{fd, POLLIN, 0};
    for (;;) {
        const int result = poll(&descriptor, 1, 5000);
        if (result > 0) {
            return (descriptor.revents & (POLLIN | POLLHUP)) != 0 &&
                   (descriptor.revents & POLLNVAL) == 0;
        }
        if (result == 0) return false;
        if (errno != EINTR) return false;
    }
}

bool read_exact(int fd, std::uint8_t* bytes, std::size_t size) {
    std::size_t received = 0;
    while (received < size) {
        if (!wait_readable(fd)) return false;
        const ssize_t count = read(fd, bytes + received, size - received);
        if (count > 0) {
            received += static_cast<std::size_t>(count);
        } else if (count == 0) {
            return false;
        } else if (errno != EINTR) {
            return false;
        }
    }
    return true;
}

bool write_all(int fd, const std::uint8_t* bytes, std::size_t size) {
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

std::optional<Frame> read_frame(int fd) {
    std::array<std::uint8_t, kFrameHeaderBytes> wire_header{};
    if (!read_exact(fd, wire_header.data(), wire_header.size())) return std::nullopt;

    FrameHeader header;
    try {
        header = mtpadb::protocol::decode_header(wire_header.data(), wire_header.size());
    } catch (...) {
        return std::nullopt;
    }

    Frame frame;
    frame.type = header.type;
    frame.flags = header.flags;
    frame.stream_id = header.stream_id;
    frame.sequence = header.sequence;
    frame.reserved = header.reserved;
    frame.payload.resize(header.payload_length);
    if (!frame.payload.empty() && !read_exact(fd, frame.payload.data(), frame.payload.size())) {
        return std::nullopt;
    }
    return frame;
}

TEST(MtpadbdRpcServiceTest, DispatchesBidirectionalHelloThroughRunningMtprpcd) {
    auto transport = make_transport<atransport>();
    unique_fd service = daemon_service_to_fd("mtpadb:rpc", &transport);
    ASSERT_GE(service.get(), 0)
            << "requires the Android mtprpcd init service and its provisioned identity";

    Frame host_hello;
    host_hello.type = FrameType::HELLO;
    host_hello.payload.resize(34);
    host_hello.payload[0] = 0;
    host_hello.payload[1] = 1;
    for (std::size_t index = 0; index < 32; ++index) {
        host_hello.payload[index + 2] = static_cast<std::uint8_t>(index + 1);
    }
    const std::vector<std::uint8_t> wire_hello = mtpadb::protocol::encode_frame(host_hello);
    ASSERT_TRUE(write_all(service.get(), wire_hello.data(), wire_hello.size()))
            << "writing the host MTPX HELLO to the service fd failed";

    const std::optional<Frame> device_hello = read_frame(service.get());
    ASSERT_TRUE(device_hello)
            << "requires a running mtprpcd init service with provisioned device identity";
    EXPECT_EQ(device_hello->type, FrameType::HELLO);
    EXPECT_EQ(device_hello->flags, 0U);
    EXPECT_EQ(device_hello->stream_id, 0U);
    EXPECT_EQ(device_hello->sequence, 0U);
    EXPECT_EQ(device_hello->reserved, 0U);
    EXPECT_EQ(device_hello->payload.size(), 48U);
}

TEST(MtpadbdRpcServiceTest, KeepsUnrelatedStockDeviceServiceDispatch) {
    auto transport = make_transport<atransport>();
    unique_fd service = daemon_service_to_fd("dev:/dev/null", &transport);
    ASSERT_GE(service.get(), 0);

    constexpr std::uint8_t byte = 0x5a;
    EXPECT_EQ(write(service.get(), &byte, sizeof(byte)), static_cast<ssize_t>(sizeof(byte)));
    std::uint8_t discarded = 0;
    EXPECT_EQ(read(service.get(), &discarded, sizeof(discarded)), 0);
}

TEST(MtpadbdRpcServiceTest, RejectsMalformedAndUnknownMtpadbServiceNames) {
    auto transport = make_transport<atransport>();
    constexpr std::array<const char*, 4> invalid_names{{
        "mtpadb:",
        "mtpadb:unknown",
        "mtpadb:rpc-extra",
        "mtpadb:rpc:extra",
    }};

    for (const char* name : invalid_names) {
        SCOPED_TRACE(name);
        unique_fd service = daemon_service_to_fd(name, &transport);
        EXPECT_LT(service.get(), 0);
    }
}

}  // namespace
