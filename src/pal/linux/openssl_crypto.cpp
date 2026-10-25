// Linux 加密实现（OpenSSL）：与 cng_crypto.cpp 语义一致，输出格式保持跨平台可互通。
// 私钥以 "LBK1" + 32 字节 P-256 标量形式保存（上层视作不透明字节）。
#define OPENSSL_SUPPRESS_DEPRECATED
#include "core/crypto/crypto.h"

#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/obj_mac.h>
#include <openssl/rand.h>

#include <cstring>
#include <memory>

namespace lb::crypto {
namespace {

constexpr char kPrivMagic[4] = {'L', 'B', 'K', '1'};
constexpr std::size_t kScalarLen = 32;

struct EcKeyDeleter {
    void operator()(EC_KEY* k) const { if (k) EC_KEY_free(k); }
};
using EcKeyPtr = std::unique_ptr<EC_KEY, EcKeyDeleter>;

struct PkeyDeleter {
    void operator()(EVP_PKEY* k) const { if (k) EVP_PKEY_free(k); }
};
using PkeyPtr = std::unique_ptr<EVP_PKEY, PkeyDeleter>;

struct CtxDeleter {
    void operator()(EVP_CIPHER_CTX* c) const { if (c) EVP_CIPHER_CTX_free(c); }
};
using CtxPtr = std::unique_ptr<EVP_CIPHER_CTX, CtxDeleter>;

struct MdCtxDeleter {
    void operator()(EVP_MD_CTX* c) const { if (c) EVP_MD_CTX_free(c); }
};
using MdCtxPtr = std::unique_ptr<EVP_MD_CTX, MdCtxDeleter>;

EcKeyPtr new_p256() {
    return EcKeyPtr(EC_KEY_new_by_curve_name(NID_X9_62_prime256v1));
}

bool valid_scalar_blob(const ByteBuf& blob) {
    return blob.size() == sizeof(kPrivMagic) + kScalarLen &&
           std::memcmp(blob.data(), kPrivMagic, sizeof(kPrivMagic)) == 0;
}

// 由标量重建 EC_KEY（含公钥点）
EcKeyPtr key_from_scalar(const ByteBuf& blob) {
    if (!valid_scalar_blob(blob)) return nullptr;
    EcKeyPtr key = new_p256();
    if (!key) return nullptr;
    BIGNUM* scalar = BN_bin2bn(blob.data() + sizeof(kPrivMagic), static_cast<int>(kScalarLen),
                               nullptr);
    if (!scalar) return nullptr;
    const bool ok = EC_KEY_set_private_key(key.get(), scalar) == 1;
    BN_free(scalar);
    if (!ok) return nullptr;

    const EC_GROUP* group = EC_KEY_get0_group(key.get());
    EC_POINT* pub = EC_POINT_new(group);
    if (!pub) return nullptr;
    BIGNUM* priv = const_cast<BIGNUM*>(EC_KEY_get0_private_key(key.get()));
    const bool ok_pub = EC_POINT_mul(group, pub, priv, nullptr, nullptr, nullptr) == 1 &&
                        EC_KEY_set_public_key(key.get(), pub) == 1;
    EC_POINT_free(pub);
    if (!ok_pub) return nullptr;
    return key;
}

ByteBuf export_public_raw(const EC_KEY* key) {
    const EC_GROUP* group = EC_KEY_get0_group(key);
    const EC_POINT* pub = EC_KEY_get0_public_key(key);
    if (!group || !pub) return {};
    ByteBuf out(65);
    const std::size_t n = EC_POINT_point2oct(group, pub, POINT_CONVERSION_UNCOMPRESSED,
                                            out.data(), out.size(), nullptr);
    if (n != out.size()) return {};
    return out;
}

Expected<ByteBuf> generate_scalar() {
    for (int attempt = 0; attempt < 4; ++attempt) {
        ByteBuf blob(sizeof(kPrivMagic) + kScalarLen, 0);
        std::memcpy(blob.data(), kPrivMagic, sizeof(kPrivMagic));
        if (RAND_priv_bytes(blob.data() + sizeof(kPrivMagic), static_cast<int>(kScalarLen)) != 1)
            return Expected<ByteBuf>::fail("openssl: RAND_priv_bytes failed");
        // 标量必须落在 [1, n-1]，极端情况下重新生成
        if (key_from_scalar(blob)) return Expected<ByteBuf>::success(std::move(blob));
    }
    return Expected<ByteBuf>::fail("openssl: 无法生成合法的 P-256 标量");
}

PkeyPtr pkey_from_ec(EC_KEY* key) {
    EVP_PKEY* raw = EVP_PKEY_new();
    if (!raw) {
        EC_KEY_free(key);
        return nullptr;
    }
    if (EVP_PKEY_assign_EC_KEY(raw, key) != 1) {
        EVP_PKEY_free(raw);
        EC_KEY_free(key);
        return nullptr;
    }
    return PkeyPtr(raw);
}

} // namespace

ByteBuf random_bytes(size_t n) {
    ByteBuf out(n);
    if (n == 0) return out;
    if (RAND_bytes(out.data(), static_cast<int>(n)) != 1) return {};
    return out;
}

ByteBuf sha256(const void* data, size_t n) {
    ByteBuf out(kSha256Len);
    unsigned int len = 0;
    EVP_MD_CTX* raw = EVP_MD_CTX_new();
    if (!raw) return {};
    MdCtxPtr ctx(raw);
    if (EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) != 1 ||
        EVP_DigestUpdate(ctx.get(), data, n) != 1 ||
        EVP_DigestFinal_ex(ctx.get(), out.data(), &len) != 1 || len != kSha256Len)
        return {};
    return out;
}

ByteBuf sha256(const ByteBuf& b) { return sha256(b.data(), b.size()); }

struct Sha256Hasher::Impl {
    MdCtxPtr ctx{nullptr};
    bool ready = false;
    bool failed = false;
};

Sha256Hasher::Sha256Hasher() : impl_(std::make_unique<Impl>()) {
    impl_->ctx.reset(EVP_MD_CTX_new());
    if (!impl_->ctx || EVP_DigestInit_ex(impl_->ctx.get(), EVP_sha256(), nullptr) != 1) {
        impl_->failed = true;
        return;
    }
    impl_->ready = true;
}

Sha256Hasher::~Sha256Hasher() = default;

bool Sha256Hasher::update(const void* data, size_t n) {
    if (!impl_ || !impl_->ready || impl_->failed) return false;
    if (n == 0) return true;
    if (EVP_DigestUpdate(impl_->ctx.get(), data, n) != 1) {
        impl_->failed = true;
        return false;
    }
    return true;
}

ByteBuf Sha256Hasher::finish() {
    if (!impl_ || !impl_->ready || impl_->failed) return {};
    ByteBuf out(kSha256Len);
    unsigned int len = 0;
    if (EVP_DigestFinal_ex(impl_->ctx.get(), out.data(), &len) != 1 || len != kSha256Len)
        return {};
    impl_->ready = false;
    return out;
}

ByteBuf hmac_sha256(const ByteBuf& key, const void* data, size_t n) {
    ByteBuf out(kSha256Len);
    unsigned int len = 0;
    if (!HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
              static_cast<const unsigned char*>(data), n, out.data(), &len) ||
        len != kSha256Len)
        return {};
    return out;
}

ByteBuf hmac_sha256(const ByteBuf& key, const ByteBuf& data) {
    return hmac_sha256(key, data.data(), data.size());
}

ByteBuf hkdf_sha256(const ByteBuf& ikm, const ByteBuf& salt, const std::string& info,
                    size_t out_len) {
    // 与 Windows CNG 实现保持一致：空 salt 视为 32 字节 0
    ByteBuf actual_salt = salt;
    if (actual_salt.empty()) actual_salt.assign(kSha256Len, 0);
    ByteBuf prk = hmac_sha256(actual_salt, ikm);
    if (prk.size() != kSha256Len) return {};

    ByteBuf out;
    ByteBuf prev;
    for (std::uint8_t counter = 1; out.size() < out_len; ++counter) {
        ByteBuf msg = prev;
        msg.insert(msg.end(), info.begin(), info.end());
        msg.push_back(counter);
        prev = hmac_sha256(prk, msg);
        if (prev.size() != kSha256Len) return {};
        out.insert(out.end(), prev.begin(), prev.end());
    }
    out.resize(out_len);
    return out;
}

ByteBuf aead_encrypt(const ByteBuf& key, const ByteBuf& nonce, const ByteBuf& plaintext,
                     const ByteBuf& aad) {
    if (key.size() != kAeadKeyLen || nonce.size() != kAeadNonceLen) return {};

    CtxPtr ctx(EVP_CIPHER_CTX_new());
    if (!ctx) return {};
    if (EVP_EncryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1)
        return {};
    if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(kAeadNonceLen),
                            nullptr) != 1)
        return {};
    if (EVP_EncryptInit_ex(ctx.get(), nullptr, nullptr, key.data(), nonce.data()) != 1) return {};

    int len = 0;
    if (!aad.empty() &&
        EVP_EncryptUpdate(ctx.get(), nullptr, &len, aad.data(), static_cast<int>(aad.size())) != 1)
        return {};

    ByteBuf out(plaintext.size() + kAeadTagLen, 0);
    int written = 0;
    if (!plaintext.empty() &&
        EVP_EncryptUpdate(ctx.get(), out.data(), &len, plaintext.data(),
                          static_cast<int>(plaintext.size())) != 1)
        return {};
    if (EVP_EncryptFinal_ex(ctx.get(), out.data() + plaintext.size(), &written) != 1) return {};
    if (static_cast<std::size_t>(written) != 0) return {};
    if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_GET_TAG, static_cast<int>(kAeadTagLen),
                            out.data() + plaintext.size()) != 1)
        return {};
    return out;
}

Expected<ByteBuf> aead_decrypt(const ByteBuf& key, const ByteBuf& nonce,
                               const ByteBuf& ciphertext, const ByteBuf& aad) {
    if (key.size() != kAeadKeyLen || nonce.size() != kAeadNonceLen)
        return Expected<ByteBuf>::fail("aead: 密钥或 nonce 长度非法");
    if (ciphertext.size() < kAeadTagLen) return Expected<ByteBuf>::fail("aead: 密文过短");

    const std::size_t body = ciphertext.size() - kAeadTagLen;
    CtxPtr ctx(EVP_CIPHER_CTX_new());
    if (!ctx) return Expected<ByteBuf>::fail("aead: 上下文分配失败");
    if (EVP_DecryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1)
        return Expected<ByteBuf>::fail("aead: 初始化解密失败");
    if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(kAeadNonceLen),
                            nullptr) != 1)
        return Expected<ByteBuf>::fail("aead: 设置 IV 长度失败");
    if (EVP_DecryptInit_ex(ctx.get(), nullptr, nullptr, key.data(), nonce.data()) != 1)
        return Expected<ByteBuf>::fail("aead: 设置密钥失败");

    int len = 0;
    if (!aad.empty() &&
        EVP_DecryptUpdate(ctx.get(), nullptr, &len, aad.data(), static_cast<int>(aad.size())) != 1)
        return Expected<ByteBuf>::fail("aead: AAD 处理失败");

    ByteBuf out(body, 0);
    int written = 0;
    if (body > 0 &&
        EVP_DecryptUpdate(ctx.get(), out.data(), &len, ciphertext.data(),
                          static_cast<int>(body)) != 1)
        return Expected<ByteBuf>::fail("aead: 解密失败");
    if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_TAG, static_cast<int>(kAeadTagLen),
                            const_cast<std::uint8_t*>(ciphertext.data() + body)) != 1)
        return Expected<ByteBuf>::fail("aead: 设置 tag 失败");
    if (EVP_DecryptFinal_ex(ctx.get(), out.data() + body, &written) != 1)
        return Expected<ByteBuf>::fail("aead: 校验失败（密钥错误或数据被篡改）");
    return Expected<ByteBuf>::success(std::move(out));
}

Expected<ByteBuf> ecdsa_generate_private() { return generate_scalar(); }

Expected<ByteBuf> ecdsa_public_raw(const ByteBuf& priv_blob) {
    EcKeyPtr key = key_from_scalar(priv_blob);
    if (!key) return Expected<ByteBuf>::fail("openssl: 私钥格式非法");
    ByteBuf raw = export_public_raw(key.get());
    if (raw.empty()) return Expected<ByteBuf>::fail("openssl: 导出公钥失败");
    return Expected<ByteBuf>::success(std::move(raw));
}

ByteBuf ecdsa_sign(const ByteBuf& priv_blob, const ByteBuf& message) {
    EcKeyPtr key = key_from_scalar(priv_blob);
    if (!key) return {};
    PkeyPtr pkey = pkey_from_ec(key.release());
    if (!pkey) return {};

    MdCtxPtr ctx(EVP_MD_CTX_new());
    if (!ctx) return {};
    if (EVP_DigestSignInit(ctx.get(), nullptr, EVP_sha256(), nullptr, pkey.get()) != 1) return {};

    const unsigned char* msg = message.empty() ? reinterpret_cast<const unsigned char*>("")
                                               : message.data();
    std::size_t der_len = 0;
    if (EVP_DigestSign(ctx.get(), nullptr, &der_len, msg, message.size()) != 1 || der_len == 0)
        return {};
    ByteBuf der(der_len);
    if (EVP_DigestSign(ctx.get(), der.data(), &der_len, msg, message.size()) != 1) return {};
    der.resize(der_len);

    const unsigned char* p = der.data();
    ECDSA_SIG* sig = d2i_ECDSA_SIG(nullptr, &p, static_cast<long>(der.size()));
    if (!sig) return {};
    const BIGNUM* r = nullptr;
    const BIGNUM* s = nullptr;
    ECDSA_SIG_get0(sig, &r, &s);
    ByteBuf out(64, 0);
    const bool ok = r && s &&
                    BN_bn2binpad(r, out.data(), 32) == 32 &&
                    BN_bn2binpad(s, out.data() + 32, 32) == 32;
    ECDSA_SIG_free(sig);
    if (!ok) return {};
    return out;
}

bool ecdsa_verify(const ByteBuf& pub_raw, const ByteBuf& message, const ByteBuf& sig) {
    if (pub_raw.size() != 65 || pub_raw[0] != 0x04 || sig.size() != 64) return false;

    EcKeyPtr key = new_p256();
    if (!key) return false;
    const EC_GROUP* group = EC_KEY_get0_group(key.get());
    EC_POINT* point = EC_POINT_new(group);
    if (!point) return false;
    const bool point_ok = EC_POINT_oct2point(group, point, pub_raw.data(), pub_raw.size(), nullptr) == 1 &&
                          EC_KEY_set_public_key(key.get(), point) == 1;
    EC_POINT_free(point);
    if (!point_ok) return false;

    PkeyPtr pkey = pkey_from_ec(key.release());
    if (!pkey) return false;

    ECDSA_SIG* sig_obj = ECDSA_SIG_new();
    if (!sig_obj) return false;
    BIGNUM* r = BN_bin2bn(sig.data(), 32, nullptr);
    BIGNUM* s = BN_bin2bn(sig.data() + 32, 32, nullptr);
    if (!r || !s || ECDSA_SIG_set0(sig_obj, r, s) != 1) {
        BN_free(r);
        BN_free(s);
        ECDSA_SIG_free(sig_obj);
        return false;
    }
    unsigned char* der = nullptr;
    const int der_len = i2d_ECDSA_SIG(sig_obj, &der);
    ECDSA_SIG_free(sig_obj);
    if (der_len <= 0 || der == nullptr) return false;

    MdCtxPtr ctx(EVP_MD_CTX_new());
    bool ok = false;
    if (ctx && EVP_DigestVerifyInit(ctx.get(), nullptr, EVP_sha256(), nullptr, pkey.get()) == 1) {
        const unsigned char* msg = message.empty()
                                       ? reinterpret_cast<const unsigned char*>("")
                                       : message.data();
        ok = EVP_DigestVerify(ctx.get(), der, static_cast<std::size_t>(der_len), msg,
                              message.size()) == 1;
    }
    OPENSSL_free(der);
    return ok;
}

Expected<ByteBuf> ecdh_generate_private() { return generate_scalar(); }

Expected<ByteBuf> ecdh_public_raw(const ByteBuf& priv_blob) {
    return ecdsa_public_raw(priv_blob);
}

Expected<ByteBuf> ecdh_shared_secret(const ByteBuf& priv_blob, const ByteBuf& peer_pub_raw) {
    if (peer_pub_raw.size() != 65 || peer_pub_raw[0] != 0x04)
        return Expected<ByteBuf>::fail("openssl: 对端公钥格式非法");
    EcKeyPtr key = key_from_scalar(priv_blob);
    if (!key) return Expected<ByteBuf>::fail("openssl: 私钥格式非法");

    const EC_GROUP* group = EC_KEY_get0_group(key.get());
    EC_POINT* peer = EC_POINT_new(group);
    if (!peer) return Expected<ByteBuf>::fail("openssl: 分配点失败");
    if (EC_POINT_oct2point(group, peer, peer_pub_raw.data(), peer_pub_raw.size(), nullptr) != 1) {
        EC_POINT_free(peer);
        return Expected<ByteBuf>::fail("openssl: 解析对端公钥失败");
    }
    ByteBuf secret(32, 0);
    const int n = ECDH_compute_key(secret.data(), secret.size(), peer, key.get(), nullptr);
    EC_POINT_free(peer);
    if (n <= 0) return Expected<ByteBuf>::fail("openssl: ECDH 计算失败");
    if (n < 32) {
        // 兼容旧版 OpenSSL 去掉前导零的行为：左补零到 32 字节
        ByteBuf padded(32, 0);
        std::memcpy(padded.data() + (32 - n), secret.data(), static_cast<std::size_t>(n));
        return Expected<ByteBuf>::success(std::move(padded));
    }
    if (n != 32) return Expected<ByteBuf>::fail("openssl: ECDH 结果长度非法");
    return Expected<ByteBuf>::success(std::move(secret));
}

} // namespace lb::crypto
