#include "mtprpc_protocol.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {

using mtpadb::protocol::Frame;
using mtpadb::protocol::FrameDecoder;
using mtpadb::protocol::FrameType;

constexpr std::size_t kWireHeaderSize = 32;

void require(bool condition, const char* message) {
    if (condition) return;
    std::cerr << message << '\n';
    std::exit(1);
}

Frame make_frame(FrameType type, std::uint32_t flags, std::uint32_t stream_id,
                 std::uint64_t sequence, std::vector<std::uint8_t> payload) {
    Frame frame{};
    frame.type = type;
    frame.flags = flags;
    frame.stream_id = stream_id;
    frame.sequence = sequence;
    frame.reserved = 0;
    frame.payload = std::move(payload);
    return frame;
}

Frame golden_tx_frame() {
    return make_frame(FrameType::TX, 0x01020304, 0x11223344,
                      0x0102030405060708, {0xaa, 0xbb});
}

bool rejects_header(const std::vector<std::uint8_t>& bytes) {
    try {
        (void)mtpadb::protocol::decode_header(bytes.data(), bytes.size());
        return false;
    } catch (const std::exception&) {
        return true;
    }
}

bool decoder_rejects_header(const std::vector<std::uint8_t>& bytes) {
    FrameDecoder decoder;
    try {
        (void)decoder.feed(bytes.data(), bytes.size());
        return false;
    } catch (const std::exception&) {
        return true;
    }
}

std::vector<std::uint8_t> encoded_golden_header() {
    auto bytes = mtpadb::protocol::encode_frame(golden_tx_frame());
    bytes.resize(kWireHeaderSize);
    return bytes;
}

void test_golden_tx_wire_bytes() {
    const std::vector<std::uint8_t> expected{
        0x4d, 0x54, 0x50, 0x58, 0x00, 0x01, 0x00, 0x05,
        0x01, 0x02, 0x03, 0x04, 0x11, 0x22, 0x33, 0x44,
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
        0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00,
        0xaa, 0xbb,
    };
    require(mtpadb::protocol::encode_frame(golden_tx_frame()) == expected,
            "TX frame did not match the golden network-order bytes");
}

void test_exact_payload_roundtrip() {
    const auto frame = make_frame(FrameType::OPEN, 0x87654321, 7, 42,
                                  {0x00, 0xff, 0x10, 0x00, 0x80});
    const auto wire = mtpadb::protocol::encode_frame(frame);
    FrameDecoder decoder;
    const auto decoded = decoder.feed(wire.data(), wire.size());

    require(decoded.size() == 1, "one complete frame was not decoded");
    require(decoded[0].payload == frame.payload, "binary payload did not roundtrip exactly");
    require(decoded[0].type == frame.type && decoded[0].flags == frame.flags &&
                decoded[0].stream_id == frame.stream_id &&
                decoded[0].sequence == frame.sequence && decoded[0].reserved == frame.reserved,
            "frame metadata did not roundtrip exactly");
}

void test_split_header_and_payload_feeds() {
    const auto frame = make_frame(FrameType::RX, 3, 9, 0x1020304050607080,
                                  {0x31, 0x00, 0x32});
    const auto wire = mtpadb::protocol::encode_frame(frame);
    FrameDecoder decoder;

    const auto partial_header = decoder.feed(wire.data(), 11);
    require(partial_header.empty(), "partial header produced a frame");
    const auto complete_header = decoder.feed(wire.data() + 11, kWireHeaderSize - 11);
    require(complete_header.empty(), "header without its payload produced a frame");
    const auto partial_payload = decoder.feed(wire.data() + kWireHeaderSize, 2);
    require(partial_payload.empty(), "partial payload produced a frame");
    const auto complete_payload = decoder.feed(wire.data() + kWireHeaderSize + 2, 1);
    require(complete_payload.size() == 1 && complete_payload[0].payload == frame.payload,
            "split header/payload feeds did not produce the complete frame");
}

void test_two_coalesced_frames() {
    const auto first = make_frame(FrameType::PING, 1, 12, 100, {0x00, 0x01});
    const auto second = make_frame(FrameType::STATUS, 2, 13, 101, {0xfe, 0x00, 0xfd});
    const auto first_wire = mtpadb::protocol::encode_frame(first);
    const auto second_wire = mtpadb::protocol::encode_frame(second);
    auto coalesced = first_wire;
    coalesced.insert(coalesced.end(), second_wire.begin(), second_wire.end());

    FrameDecoder decoder;
    const auto decoded = decoder.feed(coalesced.data(), coalesced.size());
    require(decoded.size() == 2, "two coalesced frames were not both decoded");
    require(decoded[0].type == first.type && decoded[0].stream_id == first.stream_id &&
                decoded[0].sequence == first.sequence && decoded[0].payload == first.payload,
            "first coalesced frame changed or lost its boundary");
    require(decoded[1].type == second.type && decoded[1].stream_id == second.stream_id &&
                decoded[1].sequence == second.sequence && decoded[1].payload == second.payload,
            "second coalesced frame changed or lost its boundary");
}

void test_invalid_headers_are_rejected() {
    const auto valid = encoded_golden_header();

    auto invalid = valid;
    invalid[0] = 0x00;
    require(rejects_header(invalid) && decoder_rejects_header(invalid),
            "invalid magic was accepted");

    invalid = valid;
    invalid[5] = 0x02;
    require(rejects_header(invalid) && decoder_rejects_header(invalid),
            "unsupported protocol version was accepted");

    invalid = valid;
    invalid[6] = 0x00;
    invalid[7] = 0x00;
    require(rejects_header(invalid) && decoder_rejects_header(invalid),
            "unknown frame type was accepted");

    invalid = valid;
    invalid[31] = 0x01;
    require(rejects_header(invalid) && decoder_rejects_header(invalid),
            "nonzero reserved field was accepted");
}

void test_oversized_payload_rejected_from_header_alone() {
    auto header = encoded_golden_header();
    const auto oversized = static_cast<std::uint32_t>(
        mtpadb::protocol::kMaxPayloadBytes + 1);
    header[24] = static_cast<std::uint8_t>(oversized >> 24);
    header[25] = static_cast<std::uint8_t>(oversized >> 16);
    header[26] = static_cast<std::uint8_t>(oversized >> 8);
    header[27] = static_cast<std::uint8_t>(oversized);

    require(header.size() == kWireHeaderSize, "oversize test supplied payload bytes");
    require(rejects_header(header) && decoder_rejects_header(header),
            "payload length above kMaxPayloadBytes was accepted from header alone");
}

}  // namespace

int main() {
    test_golden_tx_wire_bytes();
    test_exact_payload_roundtrip();
    test_split_header_and_payload_feeds();
    test_two_coalesced_frames();
    test_invalid_headers_are_rejected();
    test_oversized_payload_rejected_from_header_alone();
}
