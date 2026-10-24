#include "test.h"

#include "core/common/bytes.h"
#include "core/crypto/crypto.h"

#include <cstring>

using namespace lb;

namespace {

ByteBuf from_hex_str(const char* hex) {
    auto r = from_hex(hex);
    return r ? *r : ByteBuf{};
}

ByteBuf bytes_of(const char* s) {
    return ByteBuf(s, s + std::strlen(s));
}

} // namespace

TEST(crypto_sha256_known_vector) {
    const ByteBuf digest = crypto::sha256(bytes_of("abc"));
    CHECK_STR_EQ(to_hex(digest),
                 "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    const ByteBuf empty = crypto::sha256(ByteBuf{});
    CHECK_STR_EQ(to_hex(empty),
                 "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST(crypto_hmac_rfc4231_case1) {
    ByteBuf key(20, 0x0b);
    const ByteBuf mac = crypto::hmac_sha256(key, bytes_of("Hi There"));
    CHECK_STR_EQ(to_hex(mac),
                 "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
}

TEST(crypto_hkdf_rfc5869_case1) {
    ByteBuf ikm(22, 0x0b);
    const ByteBuf salt = from_hex_str("000102030405060708090a0b0c");
    const ByteBuf info = from_hex_str("f0f1f2f3f4f5f6f7f8f9");
    const ByteBuf okm =
        crypto::hkdf_sha256(ikm, salt, std::string(info.begin(), info.end()), 42);
    CHECK_STR_EQ(to_hex(okm),
                 "3cb25f25faacd57a90434f64d0362f2a"
                 "2d2d0a90cf1a5a4c5db02d56ecc4c5bf"
                 "34007208d5b887185865");
}

TEST(crypto_aead_roundtrip_and_tamper) {
    const ByteBuf key = crypto::random_bytes(32);
    const ByteBuf nonce = crypto::random_bytes(12);
    const ByteBuf plain = bytes_of("LinkBridge clipboard payload");
    const ByteBuf aad = bytes_of("seq-1");

    const ByteBuf sealed = crypto::aead_encrypt(key, nonce, plain, aad);
    CHECK(sealed.size() == plain.size() + 16);

    auto opened = crypto::aead_decrypt(key, nonce, sealed, aad);
    CHECK(opened.has_value());
    if (opened) CHECK(*opened == plain);

    ByteBuf tampered = sealed;
    tampered[0] ^= 0x01;
    CHECK(!crypto::aead_decrypt(key, nonce, tampered, aad).has_value());

    // AAD 变化也必须失败
    CHECK(!crypto::aead_decrypt(key, nonce, sealed, bytes_of("seq-2")).has_value());
    // 错误密钥必须失败
    const ByteBuf other_key = crypto::random_bytes(32);
    CHECK(!crypto::aead_decrypt(other_key, nonce, sealed, aad).has_value());
}

TEST(crypto_ecdsa_sign_verify) {
    auto priv = crypto::ecdsa_generate_private();
    CHECK(priv.has_value());
    if (!priv) return;
    auto pub = crypto::ecdsa_public_raw(*priv);
    CHECK(pub.has_value());
    if (!pub) return;
    CHECK(pub->size() == 65);
    CHECK((*pub)[0] == 0x04);

    const ByteBuf msg = bytes_of("device fingerprint binding");
    const ByteBuf sig = crypto::ecdsa_sign(*priv, msg);
    CHECK(sig.size() == 64);
    CHECK(crypto::ecdsa_verify(*pub, msg, sig));

    ByteBuf modified = msg;
    modified[0] ^= 0xff;
    CHECK(!crypto::ecdsa_verify(*pub, modified, sig));

    auto other_priv = crypto::ecdsa_generate_private();
    auto other_pub = other_priv ? crypto::ecdsa_public_raw(*other_priv) : Expected<ByteBuf>::fail("");
    CHECK(other_pub.has_value());
    if (other_pub) CHECK(!crypto::ecdsa_verify(*other_pub, msg, sig));
}

TEST(crypto_ecdh_shared_secret_matches) {
    auto a_priv = crypto::ecdh_generate_private();
    auto b_priv = crypto::ecdh_generate_private();
    CHECK(a_priv.has_value() && b_priv.has_value());
    if (!a_priv || !b_priv) return;

    auto a_pub = crypto::ecdh_public_raw(*a_priv);
    auto b_pub = crypto::ecdh_public_raw(*b_priv);
    CHECK(a_pub.has_value() && b_pub.has_value());
    if (!a_pub || !b_pub) return;

    auto ab = crypto::ecdh_shared_secret(*a_priv, *b_pub);
    auto ba = crypto::ecdh_shared_secret(*b_priv, *a_pub);
    CHECK(ab.has_value() && ba.has_value());
    if (ab && ba) {
        CHECK(ab->size() == 32);
        CHECK(*ab == *ba);
    }
}

TEST(crypto_random_unique) {
    const ByteBuf a = crypto::random_bytes(32);
    const ByteBuf b = crypto::random_bytes(32);
    CHECK(a.size() == 32);
    CHECK(a != b);
}

TEST(bytes_roundtrip) {
    ByteWriter w;
    w.u8(0x12);
    w.u16(0x3456);
    w.u32(0x789abcde);
    w.u64(0x1122334455667788ULL);
    w.i64(-42);
    w.str("hello");
    w.bytes(ByteBuf{1, 2, 3});

    ByteReader r(w.buf());
    CHECK(r.u8() == 0x12);
    CHECK(r.u16() == 0x3456);
    CHECK(r.u32() == 0x789abcde);
    CHECK(r.u64() == 0x1122334455667788ULL);
    CHECK(r.i64() == -42);
    CHECK_STR_EQ(r.str(), "hello");
    const ByteBuf tail = r.bytes();
    CHECK(tail == (ByteBuf{1, 2, 3}));
    CHECK(r.remaining() == 0);
}

TEST(bytes_truncated_input_fails_safely) {
    ByteWriter w;
    w.u32(1000);
    w.raw("short", 5);
    ByteReader r(w.buf());
    r.bytes(); // 声明 1000 但只有 5 字节
    CHECK(!r.ok());
}
