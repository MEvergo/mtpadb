#pragma once

#include "mtprpc_crypto.h"
#include "mtprpc_protocol.h"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace mtpadb::protocol::crypto {

using DeviceId = std::array<std::uint8_t, 16>;

struct HostHello {
    std::uint16_t version = kProtocolVersion;
    Nonce32 nonce{};
};

struct DeviceHello {
    DeviceId device_id{};
    Nonce32 nonce{};
};

struct EncryptedFrame {
    FrameHeader header{};
    Bytes ciphertext;
    Tag16 tag{};
};

class Session {
public:
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    Session(Session&& other) noexcept;
    Session& operator=(Session&& other) noexcept;

    EncryptedFrame seal(const FrameHeader& header, const Bytes& plaintext);
    std::optional<Bytes> open(const EncryptedFrame& frame);

private:
    friend class HostHandshake;
    friend class DeviceHandshake;

    Session(const Key32& send_key, const Key32& receive_key,
            std::uint32_t send_direction, std::uint32_t receive_direction);
    void clear() noexcept;

    Key32 send_key_{};
    Key32 receive_key_{};
    std::uint32_t send_direction_ = 0;
    std::uint32_t receive_direction_ = 0;
    std::uint64_t send_sequence_ = 0;
    std::uint64_t receive_sequence_ = 0;
    bool send_exhausted_ = false;
    bool receive_exhausted_ = false;
    bool valid_ = true;
};


class HostHandshake {
public:
    explicit HostHandshake(Bytes psk);
    HostHandshake(Bytes psk, const Nonce32& host_nonce);
    ~HostHandshake();
    HostHandshake(const HostHandshake&) = delete;
    HostHandshake& operator=(const HostHandshake&) = delete;
    HostHandshake(HostHandshake&&) = delete;
    HostHandshake& operator=(HostHandshake&&) = delete;

    HostHello hello();
    Bytes make_auth(const DeviceHello& device_hello);
    Session establish(const DeviceHello& device_hello);

private:
    enum class State { kInitial, kHelloSent, kAuthCreated, kEstablished };

    Bytes psk_;
    Nonce32 host_nonce_{};
    DeviceHello device_hello_{};
    State state_ = State::kInitial;
};


class DeviceHandshake {
public:
    DeviceHandshake(Bytes psk, const DeviceId& device_id);
    DeviceHandshake(Bytes psk, const DeviceId& device_id, const Nonce32& device_nonce);
    ~DeviceHandshake();
    DeviceHandshake(const DeviceHandshake&) = delete;
    DeviceHandshake& operator=(const DeviceHandshake&) = delete;
    DeviceHandshake(DeviceHandshake&&) = delete;
    DeviceHandshake& operator=(DeviceHandshake&&) = delete;

    DeviceHello hello(const HostHello& host_hello);
    std::optional<Session> authenticate(const HostHello& host_hello, const Bytes& auth);

private:
    enum class State { kInitial, kHelloSent, kAuthenticationAttempted };

    Bytes psk_;
    DeviceId device_id_{};
    Nonce32 device_nonce_{};
    Nonce32 host_nonce_{};
    std::uint16_t host_version_ = 0;
    State state_ = State::kInitial;
};

}  // namespace mtpadb::protocol::crypto
