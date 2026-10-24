#pragma once
// 加密原语接口。实现位于 pal/<平台>，当前提供 Windows CNG 实现。
// Linux/Android 需实现同一组函数（OpenSSL/BoringSSL），上层代码零改动。
#include "core/common/types.h"

#include <memory>
#include <string>

namespace lb::crypto {

constexpr size_t kSha256Len = 32;
constexpr size_t kAeadKeyLen = 32;
constexpr size_t kAeadNonceLen = 12;
constexpr size_t kAeadTagLen = 16;

ByteBuf random_bytes(size_t n);

ByteBuf sha256(const void* data, size_t n);
ByteBuf sha256(const ByteBuf& b);

// 流式 SHA-256（大文件不整体入内存）。update 返回 false 表示底层失败，
// 一旦失败 finish 将返回空。
class Sha256Hasher {
public:
    Sha256Hasher();
    ~Sha256Hasher();
    Sha256Hasher(const Sha256Hasher&) = delete;
    Sha256Hasher& operator=(const Sha256Hasher&) = delete;

    bool update(const void* data, size_t n);
    bool update(const ByteBuf& b) { return update(b.data(), b.size()); }
    ByteBuf finish();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

ByteBuf hmac_sha256(const ByteBuf& key, const void* data, size_t n);
ByteBuf hmac_sha256(const ByteBuf& key, const ByteBuf& data);
inline ByteBuf hmac_sha256(const ByteBuf& key, const std::string& data) {
    return hmac_sha256(key, data.data(), data.size());
}

ByteBuf hkdf_sha256(const ByteBuf& ikm, const ByteBuf& salt, const std::string& info, size_t out_len);

// 返回 密文||16字节tag
ByteBuf aead_encrypt(const ByteBuf& key, const ByteBuf& nonce, const ByteBuf& plaintext,
                     const ByteBuf& aad);
Expected<ByteBuf> aead_decrypt(const ByteBuf& key, const ByteBuf& nonce, const ByteBuf& ciphertext,
                               const ByteBuf& aad);

// ---- ECDSA P-256（长期身份密钥，用于签名认证）----
// 私钥以 CNG ECCPRIVATE_BLOB 形式返回（上层仅当作不透明字节）
Expected<ByteBuf> ecdsa_generate_private();
Expected<ByteBuf> ecdsa_public_raw(const ByteBuf& priv_blob);  // 65 字节 0x04||X||Y
ByteBuf ecdsa_sign(const ByteBuf& priv_blob, const ByteBuf& message); // 内部 SHA-256，输出 64 字节 r||s
bool ecdsa_verify(const ByteBuf& pub_raw, const ByteBuf& message, const ByteBuf& sig);

// ---- ECDH P-256（每次会话临时密钥，提供前向保密）----
Expected<ByteBuf> ecdh_generate_private();
Expected<ByteBuf> ecdh_public_raw(const ByteBuf& priv_blob);
Expected<ByteBuf> ecdh_shared_secret(const ByteBuf& priv_blob, const ByteBuf& peer_pub_raw);

} // namespace lb::crypto
