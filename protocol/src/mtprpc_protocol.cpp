#include "mtprpc_protocol.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace mtpadb::protocol {
namespace {

constexpr std::array<std::uint8_t, 4> kMagic{{'M', 'T', 'P', 'X'}};

bool is_valid_frame_type(FrameType type) noexcept {
    switch (type) {
        case FrameType::HELLO:
        case FrameType::AUTH:
        case FrameType::OPEN:
        case FrameType::CLOSE:
        case FrameType::TX:
        case FrameType::RX:
        case FrameType::STATUS:
        case FrameType::PING:
        case FrameType::ERROR:
            return true;
    }
    return false;
}

std::uint16_t read_u16(const std::uint8_t* bytes) noexcept {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[0]) << 8) |
                                      static_cast<std::uint16_t>(bytes[1]));
}

std::uint32_t read_u32(const std::uint8_t* bytes) noexcept {
    return (static_cast<std::uint32_t>(bytes[0]) << 24) |
           (static_cast<std::uint32_t>(bytes[1]) << 16) |
           (static_cast<std::uint32_t>(bytes[2]) << 8) |
           static_cast<std::uint32_t>(bytes[3]);
}

std::uint64_t read_u64(const std::uint8_t* bytes) noexcept {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < 8; ++index) {
        value = (value << 8) | static_cast<std::uint64_t>(bytes[index]);
    }
    return value;
}

void write_u16(std::uint8_t* bytes, std::uint16_t value) noexcept {
    bytes[0] = static_cast<std::uint8_t>(value >> 8);
    bytes[1] = static_cast<std::uint8_t>(value);
}

void write_u32(std::uint8_t* bytes, std::uint32_t value) noexcept {
    bytes[0] = static_cast<std::uint8_t>(value >> 24);
    bytes[1] = static_cast<std::uint8_t>(value >> 16);
    bytes[2] = static_cast<std::uint8_t>(value >> 8);
    bytes[3] = static_cast<std::uint8_t>(value);
}

void write_u64(std::uint8_t* bytes, std::uint64_t value) noexcept {
    for (std::size_t index = 0; index < 8; ++index) {
        bytes[7 - index] = static_cast<std::uint8_t>(value);
        value >>= 8;
    }
}

}  // namespace

std::vector<std::uint8_t> encode_frame(const Frame& frame) {
    if (!is_valid_frame_type(frame.type)) {
        throw std::invalid_argument("unknown MTPX frame type");
    }
    if (frame.reserved != 0) {
        throw std::invalid_argument("MTPX reserved field must be zero");
    }
    if (frame.payload.size() > kMaxPayloadBytes) {
        throw std::length_error("MTPX payload exceeds the configured maximum");
    }

    std::vector<std::uint8_t> bytes(kFrameHeaderBytes + frame.payload.size());
    std::copy(kMagic.begin(), kMagic.end(), bytes.begin());
    write_u16(bytes.data() + 4, kProtocolVersion);
    write_u16(bytes.data() + 6, static_cast<std::uint16_t>(frame.type));
    write_u32(bytes.data() + 8, frame.flags);
    write_u32(bytes.data() + 12, frame.stream_id);
    write_u64(bytes.data() + 16, frame.sequence);
    write_u32(bytes.data() + 24, static_cast<std::uint32_t>(frame.payload.size()));
    write_u32(bytes.data() + 28, frame.reserved);
    std::copy(frame.payload.begin(), frame.payload.end(), bytes.begin() + kFrameHeaderBytes);
    return bytes;
}

FrameHeader decode_header(const std::uint8_t* bytes, std::size_t size) {
    if (bytes == nullptr || size < kFrameHeaderBytes) {
        throw std::invalid_argument("incomplete MTPX frame header");
    }
    if (!std::equal(kMagic.begin(), kMagic.end(), bytes)) {
        throw std::invalid_argument("invalid MTPX magic");
    }

    FrameHeader header;
    header.version = read_u16(bytes + 4);
    if (header.version != kProtocolVersion) {
        throw std::invalid_argument("unsupported MTPX protocol version");
    }

    const auto type = static_cast<FrameType>(read_u16(bytes + 6));
    if (!is_valid_frame_type(type)) {
        throw std::invalid_argument("unknown MTPX frame type");
    }
    header.type = type;
    header.flags = read_u32(bytes + 8);
    header.stream_id = read_u32(bytes + 12);
    header.sequence = read_u64(bytes + 16);
    header.payload_length = read_u32(bytes + 24);
    header.reserved = read_u32(bytes + 28);

    if (header.reserved != 0) {
        throw std::invalid_argument("MTPX reserved field must be zero");
    }
    if (header.payload_length > kMaxPayloadBytes) {
        throw std::length_error("MTPX payload exceeds the configured maximum");
    }
    return header;
}

std::vector<Frame> FrameDecoder::feed(const std::uint8_t* bytes, std::size_t size) {
    if (bytes == nullptr && size != 0) {
        throw std::invalid_argument("null input with nonzero MTPX input size");
    }

    std::vector<Frame> frames;
    const std::uint8_t* current = bytes;
    std::size_t remaining = size;

    for (;;) {
        if (!has_header_) {
            if (remaining == 0) break;

            const std::size_t needed = kFrameHeaderBytes - header_bytes_used_;
            const std::size_t count = std::min(needed, remaining);
            std::memcpy(header_bytes_.data() + header_bytes_used_, current, count);
            header_bytes_used_ += count;
            current += count;
            remaining -= count;
            if (header_bytes_used_ != kFrameHeaderBytes) break;

            try {
                pending_header_ = decode_header(header_bytes_.data(), header_bytes_.size());
                if (pending_header_.payload_length != 0) {
                    payload_bytes_.reserve(pending_header_.payload_length);
                }
                has_header_ = true;
            } catch (...) {
                reset();
                throw;
            }
        }

        if (payload_bytes_.size() == pending_header_.payload_length) {
            Frame frame;
            frame.type = pending_header_.type;
            frame.flags = pending_header_.flags;
            frame.stream_id = pending_header_.stream_id;
            frame.sequence = pending_header_.sequence;
            frame.reserved = pending_header_.reserved;
            frame.payload = std::move(payload_bytes_);
            frames.push_back(std::move(frame));
            reset();
            continue;
        }

        if (remaining == 0) break;
        const std::size_t needed = pending_header_.payload_length - payload_bytes_.size();
        const std::size_t count = std::min(needed, remaining);
        payload_bytes_.insert(payload_bytes_.end(), current, current + count);
        current += count;
        remaining -= count;
    }

    return frames;
}

void FrameDecoder::reset() noexcept {
    header_bytes_used_ = 0;
    has_header_ = false;
    pending_header_ = FrameHeader{};
    payload_bytes_.clear();
}

}  // namespace mtpadb::protocol
