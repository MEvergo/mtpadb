#include "mtprpc_crypto.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#if defined(OPENSSL_IS_BORINGSSL)
#include <openssl/aead.h>
#endif

#include <algorithm>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>

namespace mtpadb::protocol::crypto {
namespace {

constexpr std::size_t kSha256Bytes = 32;
constexpr std::size_t kPoly1305TagBytes = 16;

void cleanse(Bytes& bytes) noexcept {
    if (!bytes.empty()) OPENSSL_cleanse(bytes.data(), bytes.size());
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

void require_hmac_key_size(std::size_t size) {
    if (size > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::length_error("HMAC key is too large");
    }
}

void require_openssl_input_size(std::size_t size) {
    if (size > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::length_error("AEAD input is too large");
    }
}

void require_aead_output_size(std::size_t size) {
    if (size > Bytes{}.max_size() - kPoly1305TagBytes) {
        throw std::length_error("AEAD input is too large");
    }
}

#if !defined(OPENSSL_IS_BORINGSSL)
struct CipherContextDeleter {
    void operator()(EVP_CIPHER_CTX* context) const noexcept { EVP_CIPHER_CTX_free(context); }
};
using CipherContext = std::unique_ptr<EVP_CIPHER_CTX, CipherContextDeleter>;
#endif

}  // namespace

Bytes hmac_sha256(const Bytes& key, const Bytes& message) {
    require_hmac_key_size(key.size());
    Bytes digest(kSha256Bytes);
    unsigned int digest_length = 0;
    static constexpr unsigned char kEmpty = 0;
    const unsigned char* key_data = key.empty() ? &kEmpty : key.data();
    const unsigned char* message_data = message.empty() ? &kEmpty : message.data();
    if (HMAC(EVP_sha256(), key_data, static_cast<int>(key.size()), message_data,
             message.size(), digest.data(), &digest_length) == nullptr ||
        digest_length != digest.size()) {
        cleanse(digest);
        throw std::runtime_error("HMAC-SHA256 failed");
    }
    return digest;
}

Bytes hkdf_sha256(const Bytes& input_key_material, const Bytes& salt,
                  const Bytes& info, std::size_t output_length) {
    constexpr std::size_t kMaximumOutput = 255U * kSha256Bytes;
    if (output_length > kMaximumOutput) {
        throw std::length_error("HKDF-SHA256 output exceeds RFC 5869 limit");
    }
    if (output_length == 0) return {};

    Bytes prk = hmac_sha256(salt, input_key_material);
    CleanseOnExit<Bytes> wipe_prk(prk);
    Bytes previous;
    CleanseOnExit<Bytes> wipe_previous(previous);
    Bytes output;
    output.reserve(output_length);

    for (std::uint16_t counter = 1; output.size() < output_length; ++counter) {
        if (info.size() > Bytes{}.max_size() - previous.size() - 1U) {
            throw std::length_error("HKDF-SHA256 info is too large");
        }
        Bytes block_input;
        block_input.reserve(previous.size() + info.size() + 1U);
        block_input.insert(block_input.end(), previous.begin(), previous.end());
        block_input.insert(block_input.end(), info.begin(), info.end());
        block_input.push_back(static_cast<std::uint8_t>(counter));
        CleanseOnExit<Bytes> wipe_block_input(block_input);

        Bytes block = hmac_sha256(prk, block_input);
        const std::size_t copied = std::min(block.size(), output_length - output.size());
        output.insert(output.end(), block.begin(), block.begin() + copied);
        cleanse(previous);
        previous = std::move(block);
    }
    return output;
}

AeadCiphertext chacha20_poly1305_encrypt(const Key32& key, const Nonce12& nonce,
                                         const Bytes& associated_data,
                                         const Bytes& plaintext) {
    return chacha20_poly1305_encrypt(key, nonce, associated_data.data(),
                                     associated_data.size(), plaintext);
}

AeadCiphertext chacha20_poly1305_encrypt(const Key32& key, const Nonce12& nonce,
                                         const std::uint8_t* associated_data,
                                         std::size_t associated_data_size,
                                         const Bytes& plaintext) {
    if (associated_data == nullptr && associated_data_size != 0) {
        throw std::invalid_argument("null AEAD associated data");
    }
    require_aead_output_size(plaintext.size());
#if !defined(OPENSSL_IS_BORINGSSL)
    require_openssl_input_size(associated_data_size);
    require_openssl_input_size(plaintext.size());
#endif
#if !defined(OPENSSL_IS_BORINGSSL)
    if (plaintext.size() > Bytes{}.max_size() - EVP_MAX_BLOCK_LENGTH) {
        throw std::length_error("AEAD output is too large");
    }
#endif

    AeadCiphertext result;
#if defined(OPENSSL_IS_BORINGSSL)
    result.ciphertext.resize(plaintext.size() + kPoly1305TagBytes);
    std::unique_ptr<EVP_AEAD_CTX, decltype(&EVP_AEAD_CTX_free)> context(
        EVP_AEAD_CTX_new(EVP_aead_chacha20_poly1305(), key.data(), key.size(),
                         EVP_AEAD_DEFAULT_TAG_LENGTH),
        EVP_AEAD_CTX_free);
    if (!context) throw std::runtime_error("ChaCha20-Poly1305 initialization failed");

    std::size_t sealed_length = 0;
    if (EVP_AEAD_CTX_seal(context.get(), result.ciphertext.data(), &sealed_length,
                          result.ciphertext.size(), nonce.data(), nonce.size(),
                          plaintext.data(), plaintext.size(), associated_data,
                          associated_data_size) != 1 ||
        sealed_length != plaintext.size() + kPoly1305TagBytes) {
        cleanse(result.ciphertext);
        throw std::runtime_error("ChaCha20-Poly1305 encryption failed");
    }
    std::copy_n(result.ciphertext.data() + plaintext.size(), result.tag.size(),
                result.tag.begin());
    result.ciphertext.resize(plaintext.size());
#else
    result.ciphertext.resize(plaintext.size() + EVP_MAX_BLOCK_LENGTH);
    CipherContext context(EVP_CIPHER_CTX_new());
    if (!context || EVP_EncryptInit_ex(context.get(), EVP_chacha20_poly1305(), nullptr,
                                       nullptr, nullptr) != 1 ||
        EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_AEAD_SET_IVLEN,
                            static_cast<int>(nonce.size()), nullptr) != 1 ||
        EVP_EncryptInit_ex(context.get(), nullptr, nullptr, key.data(), nonce.data()) != 1) {
        cleanse(result.ciphertext);
        throw std::runtime_error("ChaCha20-Poly1305 initialization failed");
    }

    int written = 0;
    int total = 0;
    if (associated_data_size != 0 &&
        EVP_EncryptUpdate(context.get(), nullptr, &written, associated_data,
                          static_cast<int>(associated_data_size)) != 1) {
        cleanse(result.ciphertext);
        throw std::runtime_error("ChaCha20-Poly1305 encryption failed");
    }
    if (!plaintext.empty()) {
        if (EVP_EncryptUpdate(context.get(), result.ciphertext.data(), &written,
                              plaintext.data(), static_cast<int>(plaintext.size())) != 1) {
            cleanse(result.ciphertext);
            throw std::runtime_error("ChaCha20-Poly1305 encryption failed");
        }
        total = written;
    }
    if (EVP_EncryptFinal_ex(context.get(), result.ciphertext.data() + total, &written) != 1) {
        cleanse(result.ciphertext);
        throw std::runtime_error("ChaCha20-Poly1305 encryption failed");
    }
    total += written;
    if (total != static_cast<int>(plaintext.size()) ||
        EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_AEAD_GET_TAG,
                            static_cast<int>(result.tag.size()), result.tag.data()) != 1) {
        cleanse(result.ciphertext);
        OPENSSL_cleanse(result.tag.data(), result.tag.size());
        throw std::runtime_error("ChaCha20-Poly1305 encryption failed");
    }
    result.ciphertext.resize(static_cast<std::size_t>(total));
#endif
    return result;
}

std::optional<Bytes> chacha20_poly1305_decrypt(const Key32& key, const Nonce12& nonce,
                                               const Bytes& associated_data,
                                               const Bytes& ciphertext,
                                               const Tag16& tag) {
    return chacha20_poly1305_decrypt(key, nonce, associated_data.data(),
                                     associated_data.size(), ciphertext, tag);
}

std::optional<Bytes> chacha20_poly1305_decrypt(const Key32& key, const Nonce12& nonce,
                                               const std::uint8_t* associated_data,
                                               std::size_t associated_data_size,
                                               const Bytes& ciphertext,
                                               const Tag16& tag) {
    if (associated_data == nullptr && associated_data_size != 0) return std::nullopt;
    if (ciphertext.size() > Bytes{}.max_size() - kPoly1305TagBytes) return std::nullopt;
#if !defined(OPENSSL_IS_BORINGSSL)
    if (associated_data_size > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        ciphertext.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return std::nullopt;
    }
#endif
#if !defined(OPENSSL_IS_BORINGSSL)
    if (ciphertext.size() > Bytes{}.max_size() - EVP_MAX_BLOCK_LENGTH) {
        return std::nullopt;
    }
#endif

#if defined(OPENSSL_IS_BORINGSSL)
    Bytes sealed;
    sealed.reserve(ciphertext.size() + tag.size());
    sealed.insert(sealed.end(), ciphertext.begin(), ciphertext.end());
    sealed.insert(sealed.end(), tag.begin(), tag.end());
    CleanseOnExit<Bytes> wipe_sealed(sealed);
    Bytes plaintext(ciphertext.size() + 1U);
    std::unique_ptr<EVP_AEAD_CTX, decltype(&EVP_AEAD_CTX_free)> context(
        EVP_AEAD_CTX_new(EVP_aead_chacha20_poly1305(), key.data(), key.size(),
                         EVP_AEAD_DEFAULT_TAG_LENGTH),
        EVP_AEAD_CTX_free);
    if (!context) return std::nullopt;

    std::size_t plaintext_length = 0;
    if (EVP_AEAD_CTX_open(context.get(), plaintext.data(), &plaintext_length,
                          ciphertext.size(), nonce.data(), nonce.size(), sealed.data(),
                          sealed.size(), associated_data, associated_data_size) != 1 ||
        plaintext_length != ciphertext.size()) {
        cleanse(plaintext);
        return std::nullopt;
    }
    plaintext.resize(plaintext_length);
    return plaintext;
#else
    Bytes plaintext(ciphertext.size() + EVP_MAX_BLOCK_LENGTH);
    CipherContext context(EVP_CIPHER_CTX_new());
    if (!context || EVP_DecryptInit_ex(context.get(), EVP_chacha20_poly1305(), nullptr,
                                       nullptr, nullptr) != 1 ||
        EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_AEAD_SET_IVLEN,
                            static_cast<int>(nonce.size()), nullptr) != 1 ||
        EVP_DecryptInit_ex(context.get(), nullptr, nullptr, key.data(), nonce.data()) != 1) {
        cleanse(plaintext);
        return std::nullopt;
    }

    int written = 0;
    int total = 0;
    if (associated_data_size != 0 &&
        EVP_DecryptUpdate(context.get(), nullptr, &written, associated_data,
                          static_cast<int>(associated_data_size)) != 1) {
        cleanse(plaintext);
        return std::nullopt;
    }
    if (!ciphertext.empty()) {
        if (EVP_DecryptUpdate(context.get(), plaintext.data(), &written, ciphertext.data(),
                              static_cast<int>(ciphertext.size())) != 1) {
            cleanse(plaintext);
            return std::nullopt;
        }
        total = written;
    }
    Tag16 mutable_tag = tag;
    if (EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_AEAD_SET_TAG,
                            static_cast<int>(mutable_tag.size()), mutable_tag.data()) != 1) {
        OPENSSL_cleanse(mutable_tag.data(), mutable_tag.size());
        cleanse(plaintext);
        return std::nullopt;
    }
    OPENSSL_cleanse(mutable_tag.data(), mutable_tag.size());
    if (EVP_DecryptFinal_ex(context.get(), plaintext.data() + total, &written) != 1) {
        cleanse(plaintext);
        return std::nullopt;
    }
    total += written;
    if (total != static_cast<int>(ciphertext.size())) {
        cleanse(plaintext);
        return std::nullopt;
    }
    plaintext.resize(static_cast<std::size_t>(total));
    return plaintext;
#endif
}

}  // namespace mtpadb::protocol::crypto
