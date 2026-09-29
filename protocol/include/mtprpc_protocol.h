#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace mtpadb::protocol {

inline constexpr std::size_t kFrameHeaderBytes = 32;
inline constexpr std::uint16_t kProtocolVersion = 1;
inline constexpr std::size_t kMaxPayloadBytes = 1024U * 1024U;
inline constexpr std::size_t kMaxStreams = 16;
inline constexpr std::size_t kMaxQueueBytes = 4U * 1024U * 1024U;
inline constexpr std::size_t kDefaultDataChunkBytes = 256U * 1024U;

enum class FrameType : std::uint16_t {
    HELLO = 1,
    AUTH = 2,
    OPEN = 3,
    CLOSE = 4,
    TX = 5,
    RX = 6,
    STATUS = 7,
    PING = 8,
    ERROR = 9,
};

struct FrameHeader {
    std::uint16_t version = kProtocolVersion;
    FrameType type{};
    std::uint32_t flags = 0;
    std::uint32_t stream_id = 0;
    std::uint64_t sequence = 0;
    std::uint32_t payload_length = 0;
    std::uint32_t reserved = 0;
};

struct Frame {
    FrameType type{};
    std::uint32_t flags = 0;
    std::uint32_t stream_id = 0;
    std::uint64_t sequence = 0;
    std::uint32_t reserved = 0;
    std::vector<std::uint8_t> payload;
};

std::vector<std::uint8_t> encode_frame(const Frame& frame);
FrameHeader decode_header(const std::uint8_t* bytes, std::size_t size);

class FrameDecoder {
public:
    std::vector<Frame> feed(const std::uint8_t* bytes, std::size_t size);

private:
    void reset() noexcept;

    std::array<std::uint8_t, kFrameHeaderBytes> header_bytes_{};
    std::size_t header_bytes_used_ = 0;
    bool has_header_ = false;
    FrameHeader pending_header_{};
    std::vector<std::uint8_t> payload_bytes_;
};

}  // namespace mtpadb::protocol
