#include "mtprpc_session.h"

#include <openssl/crypto.h>
#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace mtpadb::protocol::crypto {
namespace {

constexpr std::size_t kPskBytes = 32;
constexpr std::size_t kAuthBytes = 32;
constexpr std::size_t kAeadTagBytes = 16;
constexpr std::size_t kAuthTranscriptBytes = 2 + 32 + 32 + 16;
constexpr std::string_view kKeyInfo = "MTPADB-RPC-v1";

void wipe(Bytes& bytes) noexcept {
    if (!bytes.empty()) OPENSSL_cleanse(bytes.data(), bytes.size());
    bytes.clear();
}

template <typename Container>
class CleanseOnExit {
public:
    explicit CleanseOnExit(Container& value) noexcept : value_(value) {}
    ~CleanseOnExit() {
        if (!value_.empty()) OPENSSL_cleanse(value_.data(), value_.size() * sizeof(value_[0]));
    }
    CleanseOnExit(const CleanseOnExit&) = delete;
    CleanseOnExit& operator=(const CleanseOnExit&) = delete;

private:
    Container& value_;
};

Nonce32 random_nonce() {
    Nonce32 nonce{};
    if (RAND_bytes(nonce.data(), static_cast<int>(nonce.size())) != 1) {
        OPENSSL_cleanse(nonce.data(), nonce.size());
        throw std::runtime_error("secure nonce generation failed");
    }
    return nonce;
}

void validate_psk(Bytes& psk) {
    if (psk.size() != kPskBytes) {
        wipe(psk);
        throw std::invalid_argument("provisioned PSK must be exactly 32 bytes");
    }
}

std::array<std::uint8_t, kAuthTranscriptBytes> make_auth_transcript(
    const Nonce32& host_nonce, const Nonce32& device_nonce, const DeviceId& device_id) {
    std::array<std::uint8_t, kAuthTranscriptBytes> transcript{};
    transcript[0] = static_cast<std::uint8_t>(kProtocolVersion >> 8);
    transcript[1] = static_cast<std::uint8_t>(kProtocolVersion);
    std::copy(host_nonce.begin(), host_nonce.end(), transcript.begin() + 2);
    std::copy(device_nonce.begin(), device_nonce.end(), transcript.begin() + 34);
    std::copy(device_id.begin(), device_id.end(), transcript.begin() + 66);
    return transcript;
}

void derive_keys(const Bytes& psk, const Nonce32& host_nonce, const Nonce32& device_nonce,
                 const DeviceId& device_id, Key32& host_to_device, Key32& device_to_host) {
    Bytes salt;
    salt.reserve(host_nonce.size() + device_nonce.size());
    salt.insert(salt.end(), host_nonce.begin(), host_nonce.end());
    salt.insert(salt.end(), device_nonce.begin(), device_nonce.end());

    Bytes info(kKeyInfo.begin(), kKeyInfo.end());
    info.insert(info.end(), device_id.begin(), device_id.end());
    Bytes key_material = hkdf_sha256(psk, salt, info, 64);
    CleanseOnExit<Bytes> wipe_key_material(key_material);
    std::copy_n(key_material.begin(), host_to_device.size(), host_to_device.begin());
    std::copy_n(key_material.begin() + host_to_device.size(), device_to_host.size(),
                device_to_host.begin());
}

Nonce12 make_aead_nonce(std::uint32_t direction, std::uint64_t sequence) noexcept {
    Nonce12 nonce{};
    for (std::size_t index = 0; index < 4; ++index) {
        nonce[3U - index] = static_cast<std::uint8_t>(direction);
        direction >>= 8;
    }
    for (std::size_t index = 0; index < 8; ++index) {
        nonce[11U - index] = static_cast<std::uint8_t>(sequence);
        sequence >>= 8;
    }
    return nonce;
}

void advance_sequence(std::uint64_t& sequence, bool& exhausted) noexcept {
    if (sequence == std::numeric_limits<std::uint64_t>::max()) {
        exhausted = true;
    } else {
        ++sequence;
    }
}


bool same_hello(const DeviceHello& left, const DeviceHello& right) noexcept {
    return left.device_id == right.device_id && left.nonce == right.nonce;
}

bool is_authenticated_frame_type(FrameType type) noexcept {
    switch (type) {
        case FrameType::OPEN:
        case FrameType::CLOSE:
        case FrameType::TX:
        case FrameType::RX:
        case FrameType::STATUS:
        case FrameType::PING:
        case FrameType::ERROR:
            return true;
        case FrameType::HELLO:
        case FrameType::AUTH:
            return false;
    }
    return false;
}

}  // namespace

Session::Session(const Key32& send_key, const Key32& receive_key,
                 std::uint32_t send_direction, std::uint32_t receive_direction)
    : send_key_(send_key),
      receive_key_(receive_key),
      send_direction_(send_direction),
      receive_direction_(receive_direction) {}

Session::~Session() { clear(); }

Session::Session(Session&& other) noexcept
    : send_key_(other.send_key_),
      receive_key_(other.receive_key_),
      send_direction_(other.send_direction_),
      receive_direction_(other.receive_direction_),
      send_sequence_(other.send_sequence_),
      receive_sequence_(other.receive_sequence_),
      send_exhausted_(other.send_exhausted_),
      receive_exhausted_(other.receive_exhausted_),
      valid_(other.valid_) {
    other.clear();
}

Session& Session::operator=(Session&& other) noexcept {
    if (this == &other) return *this;
    clear();
    send_key_ = other.send_key_;
    receive_key_ = other.receive_key_;
    send_direction_ = other.send_direction_;
    receive_direction_ = other.receive_direction_;
    send_sequence_ = other.send_sequence_;
    receive_sequence_ = other.receive_sequence_;
    send_exhausted_ = other.send_exhausted_;
    receive_exhausted_ = other.receive_exhausted_;
    valid_ = other.valid_;
    other.clear();
    return *this;
}

void Session::clear() noexcept {
    OPENSSL_cleanse(send_key_.data(), send_key_.size());
    OPENSSL_cleanse(receive_key_.data(), receive_key_.size());
    send_direction_ = 0;
    receive_direction_ = 0;
    send_sequence_ = 0;
    receive_sequence_ = 0;
    send_exhausted_ = true;
    receive_exhausted_ = true;
    valid_ = false;
}

EncryptedFrame Session::seal(const FrameHeader& header, const Bytes& plaintext) {
    if (!valid_ || send_exhausted_) {
        throw std::overflow_error("session send sequence is unavailable");
    }
    if (plaintext.size() > kMaxPayloadBytes - kAeadTagBytes) {
        throw std::length_error("encrypted payload exceeds the configured maximum");
    }
    if (!is_authenticated_frame_type(header.type)) {
        throw std::invalid_argument("handshake frame is not valid inside an established session");
    }

    EncryptedFrame frame;
    frame.header = header;
    frame.header.sequence = send_sequence_;
    frame.header.payload_length = static_cast<std::uint32_t>(plaintext.size() + kAeadTagBytes);
    const auto associated_data = serialize_header(frame.header);
    const Nonce12 nonce = make_aead_nonce(send_direction_, send_sequence_);
    AeadCiphertext encrypted = chacha20_poly1305_encrypt(
        send_key_, nonce, associated_data.data(), associated_data.size(), plaintext);
    frame.ciphertext = std::move(encrypted.ciphertext);
    frame.tag = encrypted.tag;
    advance_sequence(send_sequence_, send_exhausted_);
    return frame;
}

std::optional<Bytes> Session::open(const EncryptedFrame& frame) {
    if (!is_authenticated_frame_type(frame.header.type)) return std::nullopt;
    if (!valid_ || receive_exhausted_ || frame.header.sequence != receive_sequence_ ||
        frame.ciphertext.size() > kMaxPayloadBytes - kAeadTagBytes ||
        frame.header.payload_length != frame.ciphertext.size() + kAeadTagBytes) {
        return std::nullopt;
    }

    std::array<std::uint8_t, kFrameHeaderBytes> associated_data{};
    try {
        associated_data = serialize_header(frame.header);
    } catch (...) {
        return std::nullopt;
    }
    const Nonce12 nonce = make_aead_nonce(receive_direction_, receive_sequence_);
    std::optional<Bytes> plaintext = chacha20_poly1305_decrypt(
        receive_key_, nonce, associated_data.data(), associated_data.size(), frame.ciphertext,
        frame.tag);
    if (!plaintext) return std::nullopt;

    advance_sequence(receive_sequence_, receive_exhausted_);
    return plaintext;
}

HostHandshake::HostHandshake(Bytes psk) : HostHandshake(std::move(psk), random_nonce()) {}

HostHandshake::HostHandshake(Bytes psk, const Nonce32& host_nonce)
    : psk_(std::move(psk)), host_nonce_(host_nonce) {
    validate_psk(psk_);
}

HostHandshake::~HostHandshake() {
    wipe(psk_);
    OPENSSL_cleanse(host_nonce_.data(), host_nonce_.size());
}

HostHello HostHandshake::hello() {
    if (state_ != State::kInitial) throw std::logic_error("invalid host handshake state");
    state_ = State::kHelloSent;
    return HostHello{kProtocolVersion, host_nonce_};
}

Bytes HostHandshake::make_auth(const DeviceHello& device_hello) {
    if (state_ != State::kHelloSent) throw std::logic_error("invalid host handshake state");
    const auto transcript = make_auth_transcript(host_nonce_, device_hello.nonce,
                                                 device_hello.device_id);
    Bytes transcript_bytes(transcript.begin(), transcript.end());
    Bytes auth = hmac_sha256(psk_, transcript_bytes);
    wipe(transcript_bytes);
    device_hello_ = device_hello;
    state_ = State::kAuthCreated;
    return auth;
}

Session HostHandshake::establish(const DeviceHello& device_hello) {
    if (state_ != State::kAuthCreated || !same_hello(device_hello_, device_hello)) {
        throw std::logic_error("invalid host handshake state");
    }
    std::pair<Key32, Key32> keys{};
    derive_keys(psk_, host_nonce_, device_hello.nonce, device_hello.device_id,
                keys.first, keys.second);
    CleanseOnExit<Key32> wipe_host_key(keys.first);
    CleanseOnExit<Key32> wipe_device_key(keys.second);
    Session session(keys.first, keys.second, 0, 1);
    state_ = State::kEstablished;
    wipe(psk_);
    return session;
}

DeviceHandshake::DeviceHandshake(Bytes psk, const DeviceId& device_id)
    : DeviceHandshake(std::move(psk), device_id, random_nonce()) {}

DeviceHandshake::DeviceHandshake(Bytes psk, const DeviceId& device_id,
                                 const Nonce32& device_nonce)
    : psk_(std::move(psk)), device_id_(device_id), device_nonce_(device_nonce) {
    validate_psk(psk_);
}

DeviceHandshake::~DeviceHandshake() {
    wipe(psk_);
    OPENSSL_cleanse(device_nonce_.data(), device_nonce_.size());
    OPENSSL_cleanse(host_nonce_.data(), host_nonce_.size());
}

DeviceHello DeviceHandshake::hello(const HostHello& host_hello) {
    if (state_ != State::kInitial || host_hello.version != kProtocolVersion) {
        throw std::invalid_argument("invalid host HELLO");
    }
    host_nonce_ = host_hello.nonce;
    host_version_ = host_hello.version;
    state_ = State::kHelloSent;
    return DeviceHello{device_id_, device_nonce_};
}

std::optional<Session> DeviceHandshake::authenticate(const HostHello& host_hello,
                                                    const Bytes& auth) {
    if (state_ != State::kHelloSent) {
        wipe(psk_);
        state_ = State::kAuthenticationAttempted;
        return std::nullopt;
    }
    state_ = State::kAuthenticationAttempted;
    CleanseOnExit<Bytes> wipe_psk_on_exit(psk_);
    if (host_hello.version != host_version_ || host_hello.version != kProtocolVersion ||
        CRYPTO_memcmp(host_hello.nonce.data(), host_nonce_.data(), host_nonce_.size()) != 0 ||
        auth.size() != kAuthBytes) {
        wipe(psk_);
        return std::nullopt;
    }

    const auto transcript = make_auth_transcript(host_nonce_, device_nonce_, device_id_);
    Bytes transcript_bytes(transcript.begin(), transcript.end());
    Bytes expected = hmac_sha256(psk_, transcript_bytes);
    wipe(transcript_bytes);
    const bool authenticated =
        expected.size() == auth.size() && CRYPTO_memcmp(expected.data(), auth.data(), kAuthBytes) == 0;
    wipe(expected);
    if (!authenticated) {
        wipe(psk_);
        return std::nullopt;
    }

    std::pair<Key32, Key32> keys{};
    derive_keys(psk_, host_nonce_, device_nonce_, device_id_, keys.first, keys.second);
    CleanseOnExit<Key32> wipe_host_key(keys.first);
    CleanseOnExit<Key32> wipe_device_key(keys.second);
    Session session(keys.second, keys.first, 1, 0);
    wipe(psk_);
    return std::optional<Session>(std::move(session));
}

}  // namespace mtpadb::protocol::crypto
