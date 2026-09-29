#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace mtpadb::protocol::crypto {

using Bytes = std::vector<std::uint8_t>;
using Key32 = std::array<std::uint8_t, 32>;
using Nonce12 = std::array<std::uint8_t, 12>;
using Nonce32 = std::array<std::uint8_t, 32>;
using Tag16 = std::array<std::uint8_t, 16>;

struct AeadCiphertext {
    Bytes ciphertext;
    Tag16 tag{};
};

Bytes hmac_sha256(const Bytes& key, const Bytes& message);
Bytes hkdf_sha256(const Bytes& input_key_material, const Bytes& salt,
                  const Bytes& info, std::size_t output_length);
AeadCiphertext chacha20_poly1305_encrypt(const Key32& key, const Nonce12& nonce,
                                         const Bytes& associated_data,
                                         const Bytes& plaintext);
AeadCiphertext chacha20_poly1305_encrypt(const Key32& key, const Nonce12& nonce,
                                         const std::uint8_t* associated_data,
                                         std::size_t associated_data_size,
                                         const Bytes& plaintext);
std::optional<Bytes> chacha20_poly1305_decrypt(const Key32& key, const Nonce12& nonce,
                                               const Bytes& associated_data,
                                               const Bytes& ciphertext,
                                               const Tag16& tag);
std::optional<Bytes> chacha20_poly1305_decrypt(const Key32& key, const Nonce12& nonce,
                                               const std::uint8_t* associated_data,
                                               std::size_t associated_data_size,
                                               const Bytes& ciphertext,
                                               const Tag16& tag);

}  // namespace mtpadb::protocol::crypto

// Keep the handshake/session API available to consumers of the crypto interface.
#include "mtprpc_session.h"
