#include "mtprpc_crypto.h"
#include "mtprpc_protocol.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace crypto = mtpadb::protocol::crypto;
using mtpadb::protocol::Frame;
using mtpadb::protocol::FrameHeader;
using mtpadb::protocol::FrameType;
using mtpadb::protocol::kFrameHeaderBytes;
using mtpadb::protocol::encode_frame;
using crypto::AeadCiphertext;
using crypto::DeviceHandshake;
using crypto::DeviceHello;
using crypto::EncryptedFrame;
using crypto::HostHandshake;
using crypto::HostHello;
using crypto::Session;

using Bytes = std::vector<std::uint8_t>;
using Key32 = std::array<std::uint8_t, 32>;
using Nonce12 = std::array<std::uint8_t, 12>;
using Nonce32 = std::array<std::uint8_t, 32>;
using DeviceId = std::array<std::uint8_t, 16>;
using Tag16 = std::array<std::uint8_t, 16>;

void require(bool condition, const char* message) {
    if (condition) return;
    std::cerr << message << '\n';
    std::exit(1);
}

std::uint8_t hex_nibble(char ch) {
    if (ch >= '0' && ch <= '9') return static_cast<std::uint8_t>(ch - '0');
    if (ch >= 'a' && ch <= 'f') return static_cast<std::uint8_t>(ch - 'a' + 10);
    if (ch >= 'A' && ch <= 'F') return static_cast<std::uint8_t>(ch - 'A' + 10);
    require(false, "invalid hex digit in test vector");
    return 0;
}

Bytes from_hex(std::string_view hex) {
    require(hex.size() % 2 == 0, "odd-length hex test vector");
    Bytes result;
    result.reserve(hex.size() / 2);
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        result.push_back(static_cast<std::uint8_t>((hex_nibble(hex[i]) << 4) |
                                                   hex_nibble(hex[i + 1])));
    }
    return result;
}

template <std::size_t Size>
std::array<std::uint8_t, Size> fixed_bytes(std::string_view hex) {
    const Bytes bytes = from_hex(hex);
    require(bytes.size() == Size, "test vector has an unexpected byte count");
    std::array<std::uint8_t, Size> result{};
    for (std::size_t i = 0; i < Size; ++i) result[i] = bytes[i];
    return result;
}

Bytes bytes_from_string(std::string_view value) {
    return Bytes(value.begin(), value.end());
}

void append(Bytes& destination, const std::uint8_t* data, std::size_t size) {
    destination.insert(destination.end(), data, data + size);
}

template <std::size_t Size>
void append(Bytes& destination, const std::array<std::uint8_t, Size>& data) {
    append(destination, data.data(), data.size());
}

FrameHeader data_header() {
    FrameHeader header{};
    header.type = FrameType::TX;
    header.flags = 3;
    header.stream_id = 7;
    return header;
}

std::pair<Session, Session> establish_test_sessions() {
    const Bytes psk(32, 0x42);
    const Nonce32 host_nonce = fixed_bytes<32>(
        "000102030405060708090a0b0c0d0e0f"
        "101112131415161718191a1b1c1d1e1f");
    const DeviceId device_id = fixed_bytes<16>("00112233445566778899aabbccddeeff");
    const Nonce32 device_nonce = fixed_bytes<32>(
        "a0a1a2a3a4a5a6a7a8a9aaabacadaeaf"
        "b0b1b2b3b4b5b6b7b8b9babbbcbdbebf");

    HostHandshake host(psk, host_nonce);
    DeviceHandshake device(psk, device_id, device_nonce);
    const HostHello host_hello = host.hello();
    const DeviceHello device_hello = device.hello(host_hello);
    const Bytes auth = host.make_auth(device_hello);
    std::optional<Session> device_session = device.authenticate(host_hello, auth);
    require(device_session.has_value(), "valid deterministic handshake was rejected");
    Session host_session = host.establish(device_hello);
    return {std::move(host_session), std::move(*device_session)};
}

void test_rfc4231_hmac_sha256_case1() {
    const Bytes key(20, 0x0b);
    const Bytes message = bytes_from_string("Hi There");
    const Bytes expected = from_hex(
        "b0344c61d8db38535ca8afceaf0bf12b"
        "881dc200c9833da726e9376c2e32cff7");
    require(crypto::hmac_sha256(key, message) == expected,
            "RFC 4231 HMAC-SHA256 Test Case 1 mismatch");
}

void test_rfc5869_hkdf_sha256_case1() {
    const Bytes ikm(22, 0x0b);
    const Bytes salt = from_hex("000102030405060708090a0b0c");
    const Bytes info = from_hex("f0f1f2f3f4f5f6f7f8f9");
    const Bytes expected = from_hex(
        "3cb25f25faacd57a90434f64d0362f2a"
        "2d2d0a90cf1a5a4c5db02d56ecc4c5bf"
        "34007208d5b887185865");
    require(crypto::hkdf_sha256(ikm, salt, info, 42) == expected,
            "RFC 5869 HKDF-SHA256 Test Case 1 mismatch");
}

void test_rfc8439_chacha20_poly1305_section_2_8_2() {
    const Key32 key = fixed_bytes<32>(
        "808182838485868788898a8b8c8d8e8f"
        "909192939495969798999a9b9c9d9e9f");
    const Nonce12 nonce = fixed_bytes<12>("070000004041424344454647");
    const Bytes aad = from_hex("50515253c0c1c2c3c4c5c6c7");
    const Bytes plaintext = bytes_from_string(
        "Ladies and Gentlemen of the class of '99: If I could offer you only "
        "one tip for the future, sunscreen would be it.");
    const Bytes expected_ciphertext = from_hex(
        "d31a8d34648e60db7b86afbc53ef7ec2"
        "a4aded51296e08fea9e2b5a736ee62d6"
        "3dbea45e8ca9671282fafb69da92728b"
        "1a71de0a9e060b2905d6a5b67ecd3b36"
        "92ddbd7f2d778b8c9803aee328091b58"
        "fab324e4fad675945585808b4831d7bc"
        "3ff4def08e4b7a9de576d26586cec64b"
        "6116");
    const Tag16 expected_tag = fixed_bytes<16>("1ae10b594f09e26a7e902ecbd0600691");

    const AeadCiphertext sealed = crypto::chacha20_poly1305_encrypt(key, nonce, aad, plaintext);
    require(sealed.ciphertext == expected_ciphertext,
            "RFC 8439 ChaCha20-Poly1305 ciphertext mismatch");
    require(sealed.tag == expected_tag, "RFC 8439 ChaCha20-Poly1305 tag mismatch");
    const std::optional<Bytes> opened =
        crypto::chacha20_poly1305_decrypt(key, nonce, aad, sealed.ciphertext, sealed.tag);
    require(opened.has_value() && *opened == plaintext,
            "RFC 8439 ChaCha20-Poly1305 decryption mismatch");

    Tag16 altered_tag = sealed.tag;
    altered_tag[0] ^= 0x01;
    require(!crypto::chacha20_poly1305_decrypt(key, nonce, aad, sealed.ciphertext,
                                                altered_tag).has_value(),
            "ChaCha20-Poly1305 accepted an altered authentication tag");
}

void test_fixed_transcript_and_valid_deterministic_handshake() {
    const Bytes psk(32, 0x42);
    const Nonce32 host_nonce = fixed_bytes<32>(
        "000102030405060708090a0b0c0d0e0f"
        "101112131415161718191a1b1c1d1e1f");
    const DeviceId device_id = fixed_bytes<16>("00112233445566778899aabbccddeeff");
    const Nonce32 device_nonce = fixed_bytes<32>(
        "a0a1a2a3a4a5a6a7a8a9aaabacadaeaf"
        "b0b1b2b3b4b5b6b7b8b9babbbcbdbebf");

    HostHandshake host(psk, host_nonce);
    DeviceHandshake device(psk, device_id, device_nonce);
    const HostHello host_hello = host.hello();
    require(host_hello.version == 1 && host_hello.nonce == host_nonce,
            "host HELLO does not contain version 1 and its supplied nonce");

    const DeviceHello device_hello = device.hello(host_hello);
    require(device_hello.device_id == device_id && device_hello.nonce == device_nonce,
            "device HELLO does not contain its persistent ID and supplied nonce");

    Bytes auth_transcript{0x00, 0x01};
    append(auth_transcript, host_nonce);
    append(auth_transcript, device_nonce);
    append(auth_transcript, device_id);
    const Bytes expected_auth = crypto::hmac_sha256(psk, auth_transcript);
    const Bytes auth = host.make_auth(device_hello);
    require(auth == expected_auth, "AUTH does not match the fixed handshake transcript");
    require(device.authenticate(host_hello, auth).has_value(),
            "device rejected AUTH for the matching PSK");
}

void test_wrong_psk_is_rejected() {
    const Bytes host_psk(32, 0x42);
    const Bytes wrong_psk(32, 0x24);
    const Nonce32 host_nonce = fixed_bytes<32>(
        "000102030405060708090a0b0c0d0e0f"
        "101112131415161718191a1b1c1d1e1f");
    const DeviceId device_id = fixed_bytes<16>("00112233445566778899aabbccddeeff");
    const Nonce32 device_nonce = fixed_bytes<32>(
        "a0a1a2a3a4a5a6a7a8a9aaabacadaeaf"
        "b0b1b2b3b4b5b6b7b8b9babbbcbdbebf");

    HostHandshake host(host_psk, host_nonce);
    DeviceHandshake device(wrong_psk, device_id, device_nonce);
    const HostHello host_hello = host.hello();
    const DeviceHello device_hello = device.hello(host_hello);
    const Bytes auth = host.make_auth(device_hello);
    require(!device.authenticate(host_hello, auth).has_value(),
            "device accepted AUTH generated with a different PSK");
}

void test_session_derivation_and_header_associated_data() {
    auto sessions = establish_test_sessions();
    Session& host = sessions.first;
    Session& device = sessions.second;
    const Bytes plaintext = from_hex("01020304050607");
    const EncryptedFrame frame = host.seal(data_header(), plaintext);
    require(frame.header.sequence == 0, "first host-to-device sequence was not zero");
    require(frame.header.payload_length == plaintext.size() + 16,
            "encrypted frame payload length omitted the Poly1305 tag");

    const Bytes psk(32, 0x42);
    const Nonce32 host_nonce = fixed_bytes<32>(
        "000102030405060708090a0b0c0d0e0f"
        "101112131415161718191a1b1c1d1e1f");
    const DeviceId device_id = fixed_bytes<16>("00112233445566778899aabbccddeeff");
    const Nonce32 device_nonce = fixed_bytes<32>(
        "a0a1a2a3a4a5a6a7a8a9aaabacadaeaf"
        "b0b1b2b3b4b5b6b7b8b9babbbcbdbebf");
    Bytes salt;
    append(salt, host_nonce);
    append(salt, device_nonce);
    Bytes info = bytes_from_string("MTPADB-RPC-v1");
    append(info, device_id);
    const Bytes key_material = crypto::hkdf_sha256(psk, salt, info, 64);
    Key32 host_to_device_key{};
    for (std::size_t i = 0; i < host_to_device_key.size(); ++i) {
        host_to_device_key[i] = key_material[i];
    }

    Nonce12 nonce{};
    nonce[3] = 0;  // Host-to-device direction prefix, uint32_be(0).
    Frame header_frame{};
    header_frame.type = frame.header.type;
    header_frame.flags = frame.header.flags;
    header_frame.stream_id = frame.header.stream_id;
    header_frame.sequence = frame.header.sequence;
    header_frame.reserved = frame.header.reserved;
    header_frame.payload.assign(frame.header.payload_length, 0);
    const Bytes encoded_header_and_body = encode_frame(header_frame);
    const Bytes serialized_header(encoded_header_and_body.begin(),
                                  encoded_header_and_body.begin() + kFrameHeaderBytes);
    const AeadCiphertext expected = crypto::chacha20_poly1305_encrypt(
        host_to_device_key, nonce, serialized_header, plaintext);
    require(frame.ciphertext == expected.ciphertext && frame.tag == expected.tag,
            "session did not use the fixed HKDF key, nonce, and serialized header AAD");
    const std::optional<Bytes> opened = device.open(frame);
    require(opened.has_value() && *opened == plaintext,
            "device could not open a valid host-to-device session frame");
}

void test_altered_header_ciphertext_and_tag_are_rejected() {
    const Bytes plaintext = from_hex("102030405060");

    {
        auto sessions = establish_test_sessions();
        const EncryptedFrame frame = sessions.first.seal(data_header(), plaintext);
        EncryptedFrame altered = frame;
        altered.header.stream_id ^= 1U;
        require(!sessions.second.open(altered).has_value(),
                "session accepted a frame with an altered associated header");
        const std::optional<Bytes> original = sessions.second.open(frame);
        require(original.has_value() && *original == plaintext,
                "rejected header alteration consumed the receive sequence");
    }
    {
        auto sessions = establish_test_sessions();
        const EncryptedFrame frame = sessions.first.seal(data_header(), plaintext);
        EncryptedFrame altered = frame;
        altered.ciphertext[0] ^= 0x80;
        require(!sessions.second.open(altered).has_value(),
                "session accepted altered ciphertext");
        const std::optional<Bytes> original = sessions.second.open(frame);
        require(original.has_value() && *original == plaintext,
                "rejected ciphertext alteration consumed the receive sequence");
    }
    {
        auto sessions = establish_test_sessions();
        const EncryptedFrame frame = sessions.first.seal(data_header(), plaintext);
        EncryptedFrame altered = frame;
        altered.tag[0] ^= 0x01;
        require(!sessions.second.open(altered).has_value(),
                "session accepted an altered authentication tag");
        const std::optional<Bytes> original = sessions.second.open(frame);
        require(original.has_value() && *original == plaintext,
                "rejected tag alteration consumed the receive sequence");
    }
}

void test_duplicate_and_rolled_back_sequences_are_rejected() {
    auto sessions = establish_test_sessions();
    Session& host = sessions.first;
    Session& device = sessions.second;
    const Bytes first_payload = from_hex("0101");
    const Bytes second_payload = from_hex("020202");
    const EncryptedFrame first = host.seal(data_header(), first_payload);
    const EncryptedFrame second = host.seal(data_header(), second_payload);

    const std::optional<Bytes> first_opened = device.open(first);
    require(first_opened.has_value() && *first_opened == first_payload,
            "device failed to accept sequence zero");
    require(!device.open(first).has_value(),
            "device accepted a duplicate sequence before dispatch");
    const std::optional<Bytes> second_opened = device.open(second);
    require(second_opened.has_value() && *second_opened == second_payload,
            "device failed to accept the next sequence");
    require(!device.open(first).has_value(),
            "device accepted a rolled-back sequence before dispatch");
}

void test_host_to_device_frame_is_not_accepted_as_device_to_host() {
    auto sessions = establish_test_sessions();
    Session& host = sessions.first;
    Session& device = sessions.second;
    const Bytes payload = from_hex("cafebabefeed");
    const EncryptedFrame host_to_device = host.seal(data_header(), payload);

    require(!host.open(host_to_device).has_value(),
            "host accepted its host-to-device frame as device-to-host data");
    const std::optional<Bytes> device_opened = device.open(host_to_device);
    require(device_opened.has_value() && *device_opened == payload,
            "correct device receiver rejected host-to-device data");
}

}  // namespace

int main() {
    test_rfc4231_hmac_sha256_case1();
    test_rfc5869_hkdf_sha256_case1();
    test_rfc8439_chacha20_poly1305_section_2_8_2();
    test_fixed_transcript_and_valid_deterministic_handshake();
    test_wrong_psk_is_rejected();
    test_session_derivation_and_header_associated_data();
    test_altered_header_ciphertext_and_tag_are_rejected();
    test_duplicate_and_rolled_back_sequences_are_rejected();
    test_host_to_device_frame_is_not_accepted_as_device_to_host();
}
