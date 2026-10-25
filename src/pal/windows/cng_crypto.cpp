// Windows CNG 加密实现：SHA-256 / HMAC / HKDF / AES-256-GCM / ECDSA P-256 / ECDH P-256。
// 选择 CNG 的原因：零第三方依赖、FIPS 认证实现、导出私钥可交由 DPAPI 保护。
#include "core/crypto/crypto.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cstring>

// 旧 SDK/以 Win7 为目标编译时头文件不声明该宏；CNG 识别的 KDF 名是 "TRUNCATE"。
#ifndef BCRYPT_KDF_RAW_SECRET
#define BCRYPT_KDF_RAW_SECRET L"TRUNCATE"
#endif

namespace lb::crypto {
namespace {

struct AlgHandle {
    BCRYPT_ALG_HANDLE h = nullptr;
    ~AlgHandle() { if (h) BCryptCloseAlgorithmProvider(h, 0); }
    bool open(LPCWSTR name, ULONG flags = 0) {
        return BCryptOpenAlgorithmProvider(&h, name, nullptr, flags) == 0;
    }
};

struct KeyHandle {
    BCRYPT_KEY_HANDLE h = nullptr;
    ~KeyHandle() { if (h) BCryptDestroyKey(h); }
};

struct HashHandle {
    BCRYPT_HASH_HANDLE h = nullptr;
    ~HashHandle() { if (h) BCryptDestroyHash(h); }
};

struct SecretHandle {
    BCRYPT_SECRET_HANDLE h = nullptr;
    ~SecretHandle() { if (h) BCryptDestroySecret(h); }
};

ByteBuf hash_with(BCRYPT_ALG_HANDLE alg, const void* key, size_t key_len, const void* data,
                  size_t data_len) {
    HashHandle hash;
    if (BCryptCreateHash(alg, &hash.h, nullptr, 0,
                         reinterpret_cast<PUCHAR>(const_cast<void*>(key)),
                         static_cast<ULONG>(key_len), 0) != 0)
        return {};
    if (data_len > 0) {
        if (BCryptHashData(hash.h, reinterpret_cast<PUCHAR>(const_cast<void*>(data)),
                           static_cast<ULONG>(data_len), 0) != 0)
            return {};
    }
    ByteBuf out(kSha256Len);
    if (BCryptFinishHash(hash.h, out.data(), static_cast<ULONG>(out.size()), 0) != 0) return {};
    return out;
}

// 将 65 字节裸公钥 0x04||X||Y 转为 CNG ECCPUBLIC_BLOB
ByteBuf make_public_blob(const ByteBuf& raw65, ULONG magic) {
    if (raw65.size() != 65 || raw65[0] != 0x04) return {};
    ByteBuf blob(sizeof(BCRYPT_ECCKEY_BLOB) + 64);
    auto* hdr = reinterpret_cast<BCRYPT_ECCKEY_BLOB*>(blob.data());
    hdr->dwMagic = magic;
    hdr->cbKey = 32;
    std::memcpy(blob.data() + sizeof(BCRYPT_ECCKEY_BLOB), raw65.data() + 1, 64);
    return blob;
}

ByteBuf export_public_raw(BCRYPT_KEY_HANDLE key) {
    ULONG len = 0;
    if (BCryptExportKey(key, nullptr, BCRYPT_ECCPUBLIC_BLOB, nullptr, 0, &len, 0) != 0) return {};
    ByteBuf blob(len);
    if (BCryptExportKey(key, nullptr, BCRYPT_ECCPUBLIC_BLOB, blob.data(), len, &len, 0) != 0)
        return {};
    if (blob.size() < sizeof(BCRYPT_ECCKEY_BLOB) + 64) return {};
    ByteBuf raw(65);
    raw[0] = 0x04;
    std::memcpy(raw.data() + 1, blob.data() + sizeof(BCRYPT_ECCKEY_BLOB), 64);
    return raw;
}

Expected<ByteBuf> ecc_generate(LPCWSTR alg_name) {
    AlgHandle alg;
    if (!alg.open(alg_name)) return Expected<ByteBuf>::fail("cng: open algorithm failed");
    KeyHandle key;
    if (BCryptGenerateKeyPair(alg.h, &key.h, 256, 0) != 0)
        return Expected<ByteBuf>::fail("cng: generate key pair failed");
    if (BCryptFinalizeKeyPair(key.h, 0) != 0)
        return Expected<ByteBuf>::fail("cng: finalize key pair failed");
    ULONG len = 0;
    if (BCryptExportKey(key.h, nullptr, BCRYPT_ECCPRIVATE_BLOB, nullptr, 0, &len, 0) != 0)
        return Expected<ByteBuf>::fail("cng: export size failed");
    ByteBuf blob(len);
    if (BCryptExportKey(key.h, nullptr, BCRYPT_ECCPRIVATE_BLOB, blob.data(), len, &len, 0) != 0)
        return Expected<ByteBuf>::fail("cng: export private failed");
    blob.resize(len);
    return Expected<ByteBuf>::success(std::move(blob));
}

Expected<ByteBuf> ecc_public_from_private(LPCWSTR alg_name, const ByteBuf& priv_blob) {
    AlgHandle alg;
    if (!alg.open(alg_name)) return Expected<ByteBuf>::fail("cng: open algorithm failed");
    KeyHandle key;
    if (BCryptImportKeyPair(alg.h, nullptr, BCRYPT_ECCPRIVATE_BLOB, &key.h,
                            reinterpret_cast<PUCHAR>(const_cast<std::uint8_t*>(priv_blob.data())),
                            static_cast<ULONG>(priv_blob.size()), 0) != 0)
        return Expected<ByteBuf>::fail("cng: import private key failed");
    ByteBuf raw = export_public_raw(key.h);
    if (raw.empty()) return Expected<ByteBuf>::fail("cng: export public failed");
    return Expected<ByteBuf>::success(std::move(raw));
}

} // namespace

ByteBuf random_bytes(size_t n) {
    ByteBuf out(n);
    if (n == 0) return out;
    if (BCryptGenRandom(nullptr, out.data(), static_cast<ULONG>(n),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        return {};
    return out;
}

ByteBuf sha256(const void* data, size_t n) {
    AlgHandle alg;
    if (!alg.open(BCRYPT_SHA256_ALGORITHM)) return {};
    return hash_with(alg.h, nullptr, 0, data, n);
}

ByteBuf sha256(const ByteBuf& b) { return sha256(b.data(), b.size()); }

struct Sha256Hasher::Impl {
    AlgHandle alg;
    HashHandle hash;
    bool ready = false;
    bool failed = false;
};

Sha256Hasher::Sha256Hasher() : impl_(std::make_unique<Impl>()) {
    if (!impl_->alg.open(BCRYPT_SHA256_ALGORITHM)) {
        impl_->failed = true;
        return;
    }
    if (BCryptCreateHash(impl_->alg.h, &impl_->hash.h, nullptr, 0, nullptr, 0, 0) != 0) {
        impl_->failed = true;
        return;
    }
    impl_->ready = true;
}

Sha256Hasher::~Sha256Hasher() = default;

bool Sha256Hasher::update(const void* data, size_t n) {
    if (!impl_ || !impl_->ready || impl_->failed) return false;
    if (n == 0) return true;
    if (n > 0xFFFFFFFFu) return false;
    if (BCryptHashData(impl_->hash.h, reinterpret_cast<PUCHAR>(const_cast<void*>(data)),
                       static_cast<ULONG>(n), 0) != 0) {
        impl_->failed = true;
        return false;
    }
    return true;
}

ByteBuf Sha256Hasher::finish() {
    if (!impl_ || !impl_->ready || impl_->failed) return {};
    ByteBuf out(kSha256Len);
    if (BCryptFinishHash(impl_->hash.h, out.data(), static_cast<ULONG>(out.size()), 0) != 0)
        return {};
    impl_->ready = false;
    return out;
}

ByteBuf hmac_sha256(const ByteBuf& key, const void* data, size_t n) {
    AlgHandle alg;
    if (!alg.open(BCRYPT_SHA256_ALGORITHM, BCRYPT_ALG_HANDLE_HMAC_FLAG)) return {};
    return hash_with(alg.h, key.data(), key.size(), data, n);
}

ByteBuf hmac_sha256(const ByteBuf& key, const ByteBuf& data) {
    return hmac_sha256(key, data.data(), data.size());
}

ByteBuf hkdf_sha256(const ByteBuf& ikm, const ByteBuf& salt, const std::string& info,
                    size_t out_len) {
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

namespace {

bool gcm_crypt(bool encrypt, const ByteBuf& key, const ByteBuf& nonce, const ByteBuf& input,
               const ByteBuf& aad, const ByteBuf& tag, ByteBuf& output) {
    AlgHandle alg;
    if (!alg.open(BCRYPT_AES_ALGORITHM)) return false;
    if (BCryptSetProperty(alg.h, BCRYPT_CHAINING_MODE,
                          reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
                          sizeof(BCRYPT_CHAIN_MODE_GCM), 0) != 0)
        return false;
    KeyHandle key_handle;
    if (BCryptGenerateSymmetricKey(alg.h, &key_handle.h, nullptr, 0,
                                   reinterpret_cast<PUCHAR>(const_cast<std::uint8_t*>(key.data())),
                                   static_cast<ULONG>(key.size()), 0) != 0)
        return false;

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = reinterpret_cast<PUCHAR>(const_cast<std::uint8_t*>(nonce.data()));
    info.cbNonce = static_cast<ULONG>(nonce.size());
    info.pbAuthData =
        aad.empty() ? nullptr : reinterpret_cast<PUCHAR>(const_cast<std::uint8_t*>(aad.data()));
    info.cbAuthData = static_cast<ULONG>(aad.size());
    info.pbTag = encrypt ? nullptr : reinterpret_cast<PUCHAR>(const_cast<std::uint8_t*>(tag.data()));
    info.cbTag = static_cast<ULONG>(kAeadTagLen);

    output.assign(input.size(), 0);
    ULONG done = 0;
    const PUCHAR in_ptr = input.empty()
                              ? nullptr
                              : reinterpret_cast<PUCHAR>(const_cast<std::uint8_t*>(input.data()));
    const ULONG in_len = static_cast<ULONG>(input.size());
    if (encrypt) {
        if (BCryptEncrypt(key_handle.h, in_ptr, in_len, &info, nullptr, 0, output.data(),
                          static_cast<ULONG>(output.size()), &done, 0) != 0)
            return false;
    } else {
        if (BCryptDecrypt(key_handle.h, in_ptr, in_len, &info, nullptr, 0, output.data(),
                          static_cast<ULONG>(output.size()), &done, 0) != 0)
            return false;
    }
    output.resize(done);
    return true;
}

} // namespace

ByteBuf aead_encrypt(const ByteBuf& key, const ByteBuf& nonce, const ByteBuf& plaintext,
                     const ByteBuf& aad) {
    if (key.size() != kAeadKeyLen || nonce.size() != kAeadNonceLen) return {};

    AlgHandle alg;
    if (!alg.open(BCRYPT_AES_ALGORITHM)) return {};
    if (BCryptSetProperty(alg.h, BCRYPT_CHAINING_MODE,
                          reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
                          sizeof(BCRYPT_CHAIN_MODE_GCM), 0) != 0)
        return {};
    KeyHandle key_handle;
    if (BCryptGenerateSymmetricKey(alg.h, &key_handle.h, nullptr, 0,
                                   reinterpret_cast<PUCHAR>(const_cast<std::uint8_t*>(key.data())),
                                   static_cast<ULONG>(key.size()), 0) != 0)
        return {};

    ByteBuf tag(kAeadTagLen, 0);
    ByteBuf ciphertext(plaintext.size(), 0);

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = reinterpret_cast<PUCHAR>(const_cast<std::uint8_t*>(nonce.data()));
    info.cbNonce = static_cast<ULONG>(nonce.size());
    info.pbAuthData =
        aad.empty() ? nullptr : reinterpret_cast<PUCHAR>(const_cast<std::uint8_t*>(aad.data()));
    info.cbAuthData = static_cast<ULONG>(aad.size());
    info.pbTag = tag.data();
    info.cbTag = static_cast<ULONG>(tag.size());

    ULONG done = 0;
    const PUCHAR in_ptr = plaintext.empty()
                              ? nullptr
                              : reinterpret_cast<PUCHAR>(const_cast<std::uint8_t*>(plaintext.data()));
    if (BCryptEncrypt(key_handle.h, in_ptr, static_cast<ULONG>(plaintext.size()), &info, nullptr, 0,
                      ciphertext.data(), static_cast<ULONG>(ciphertext.size()), &done, 0) != 0)
        return {};
    ciphertext.resize(done);
    ciphertext.insert(ciphertext.end(), tag.begin(), tag.end());
    return ciphertext;
}

Expected<ByteBuf> aead_decrypt(const ByteBuf& key, const ByteBuf& nonce, const ByteBuf& ciphertext,
                               const ByteBuf& aad) {
    if (key.size() != kAeadKeyLen || nonce.size() != kAeadNonceLen)
        return Expected<ByteBuf>::fail("aead: bad key/nonce length");
    if (ciphertext.size() < kAeadTagLen)
        return Expected<ByteBuf>::fail("aead: ciphertext too short");

    const size_t ct_len = ciphertext.size() - kAeadTagLen;
    ByteBuf ct(ciphertext.begin(), ciphertext.begin() + static_cast<long>(ct_len));
    ByteBuf tag(ciphertext.begin() + static_cast<long>(ct_len), ciphertext.end());

    AlgHandle alg;
    if (!alg.open(BCRYPT_AES_ALGORITHM)) return Expected<ByteBuf>::fail("aead: open failed");
    if (BCryptSetProperty(alg.h, BCRYPT_CHAINING_MODE,
                          reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
                          sizeof(BCRYPT_CHAIN_MODE_GCM), 0) != 0)
        return Expected<ByteBuf>::fail("aead: set gcm failed");
    KeyHandle key_handle;
    if (BCryptGenerateSymmetricKey(alg.h, &key_handle.h, nullptr, 0,
                                   reinterpret_cast<PUCHAR>(const_cast<std::uint8_t*>(key.data())),
                                   static_cast<ULONG>(key.size()), 0) != 0)
        return Expected<ByteBuf>::fail("aead: import key failed");

    ByteBuf plaintext(ct.size(), 0);
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = reinterpret_cast<PUCHAR>(const_cast<std::uint8_t*>(nonce.data()));
    info.cbNonce = static_cast<ULONG>(nonce.size());
    info.pbAuthData =
        aad.empty() ? nullptr : reinterpret_cast<PUCHAR>(const_cast<std::uint8_t*>(aad.data()));
    info.cbAuthData = static_cast<ULONG>(aad.size());
    info.pbTag = tag.data();
    info.cbTag = static_cast<ULONG>(tag.size());

    ULONG done = 0;
    const PUCHAR in_ptr =
        ct.empty() ? nullptr : reinterpret_cast<PUCHAR>(const_cast<std::uint8_t*>(ct.data()));
    const NTSTATUS status =
        BCryptDecrypt(key_handle.h, in_ptr, static_cast<ULONG>(ct.size()), &info, nullptr, 0,
                      plaintext.data(), static_cast<ULONG>(plaintext.size()), &done, 0);
    if (status != 0) return Expected<ByteBuf>::fail("aead: authentication failed");
    plaintext.resize(done);
    return Expected<ByteBuf>::success(std::move(plaintext));
}

Expected<ByteBuf> ecdsa_generate_private() { return ecc_generate(BCRYPT_ECDSA_P256_ALGORITHM); }

Expected<ByteBuf> ecdsa_public_raw(const ByteBuf& priv_blob) {
    return ecc_public_from_private(BCRYPT_ECDSA_P256_ALGORITHM, priv_blob);
}

ByteBuf ecdsa_sign(const ByteBuf& priv_blob, const ByteBuf& message) {
    const ByteBuf digest = sha256(message);
    if (digest.size() != kSha256Len) return {};

    AlgHandle alg;
    if (!alg.open(BCRYPT_ECDSA_P256_ALGORITHM)) return {};
    KeyHandle key;
    if (BCryptImportKeyPair(alg.h, nullptr, BCRYPT_ECCPRIVATE_BLOB, &key.h,
                            reinterpret_cast<PUCHAR>(const_cast<std::uint8_t*>(priv_blob.data())),
                            static_cast<ULONG>(priv_blob.size()), 0) != 0)
        return {};
    ULONG sig_len = 0;
    if (BCryptSignHash(key.h, nullptr, reinterpret_cast<PUCHAR>(const_cast<std::uint8_t*>(digest.data())),
                       static_cast<ULONG>(digest.size()), nullptr, 0, &sig_len, 0) != 0)
        return {};
    ByteBuf sig(sig_len);
    if (BCryptSignHash(key.h, nullptr,
                       reinterpret_cast<PUCHAR>(const_cast<std::uint8_t*>(digest.data())),
                       static_cast<ULONG>(digest.size()), sig.data(), sig_len, &sig_len, 0) != 0)
        return {};
    sig.resize(sig_len);
    return sig;
}

bool ecdsa_verify(const ByteBuf& pub_raw, const ByteBuf& message, const ByteBuf& sig) {
    const ByteBuf digest = sha256(message);
    if (digest.size() != kSha256Len || pub_raw.size() != 65 || sig.empty()) return false;

    AlgHandle alg;
    if (!alg.open(BCRYPT_ECDSA_P256_ALGORITHM)) return false;
    ByteBuf blob = make_public_blob(pub_raw, BCRYPT_ECDSA_PUBLIC_P256_MAGIC);
    if (blob.empty()) return false;
    KeyHandle key;
    if (BCryptImportKeyPair(alg.h, nullptr, BCRYPT_ECCPUBLIC_BLOB, &key.h, blob.data(),
                            static_cast<ULONG>(blob.size()), 0) != 0)
        return false;
    const NTSTATUS status =
        BCryptVerifySignature(key.h, nullptr,
                              reinterpret_cast<PUCHAR>(const_cast<std::uint8_t*>(digest.data())),
                              static_cast<ULONG>(digest.size()),
                              reinterpret_cast<PUCHAR>(const_cast<std::uint8_t*>(sig.data())),
                              static_cast<ULONG>(sig.size()), 0);
    return status == 0;
}

Expected<ByteBuf> ecdh_generate_private() { return ecc_generate(BCRYPT_ECDH_P256_ALGORITHM); }

Expected<ByteBuf> ecdh_public_raw(const ByteBuf& priv_blob) {
    return ecc_public_from_private(BCRYPT_ECDH_P256_ALGORITHM, priv_blob);
}

Expected<ByteBuf> ecdh_shared_secret(const ByteBuf& priv_blob, const ByteBuf& peer_pub_raw) {
    AlgHandle alg;
    if (!alg.open(BCRYPT_ECDH_P256_ALGORITHM))
        return Expected<ByteBuf>::fail("ecdh: open failed");
    KeyHandle local;
    if (BCryptImportKeyPair(alg.h, nullptr, BCRYPT_ECCPRIVATE_BLOB, &local.h,
                            reinterpret_cast<PUCHAR>(const_cast<std::uint8_t*>(priv_blob.data())),
                            static_cast<ULONG>(priv_blob.size()), 0) != 0)
        return Expected<ByteBuf>::fail("ecdh: import local key failed");
    ByteBuf blob = make_public_blob(peer_pub_raw, BCRYPT_ECDH_PUBLIC_P256_MAGIC);
    if (blob.empty()) return Expected<ByteBuf>::fail("ecdh: bad peer public key");
    KeyHandle peer;
    if (BCryptImportKeyPair(alg.h, nullptr, BCRYPT_ECCPUBLIC_BLOB, &peer.h, blob.data(),
                            static_cast<ULONG>(blob.size()), 0) != 0)
        return Expected<ByteBuf>::fail("ecdh: import peer key failed");

    SecretHandle secret;
    if (BCryptSecretAgreement(local.h, peer.h, &secret.h, 0) != 0)
        return Expected<ByteBuf>::fail("ecdh: agreement failed");

    ByteBuf raw(kSha256Len);
    ULONG out_len = 0;
    if (BCryptDeriveKey(secret.h, BCRYPT_KDF_RAW_SECRET, nullptr, raw.data(),
                        static_cast<ULONG>(raw.size()), &out_len, 0) != 0)
        return Expected<ByteBuf>::fail("ecdh: derive failed");
    raw.resize(out_len);
    // CNG 的 RAW_SECRET 为大端 X 坐标的逆序（小端）；统一转为大端。
    std::reverse(raw.begin(), raw.end());
    return Expected<ByteBuf>::success(std::move(raw));
}

} // namespace lb::crypto
