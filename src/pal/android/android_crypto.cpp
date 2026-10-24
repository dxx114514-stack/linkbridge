// Android 加密原语实现：通过 JNI 调用 JCA（MessageDigest/Mac/Cipher/Signature/KeyAgreement）。
// 私钥以 PKCS#8 DER 形式作为不透明 blob 在核心层传递；公钥为 65 字节未压缩点。
#include "core/crypto/crypto.h"
#include "pal/android/android_jni.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace lb::crypto {
namespace {

using lb::android::from_jbytes;
using lb::android::to_jbytes;

constexpr const char* kECCurve = "secp256r1";
constexpr int kAeadTagBits = 128;

struct ScopedEnv {
    JNIEnv* e = nullptr;
    ScopedEnv() : e(lb::android::env()) {}
    ~ScopedEnv() { lb::android::release(e); }
    explicit operator bool() const { return e != nullptr; }
};

void drop_exception(JNIEnv* e) {
    if (e->ExceptionCheck()) e->ExceptionClear();
}

jclass find_class(JNIEnv* e, const char* name) {
    jclass local = e->FindClass(name);
    if (!local) {
        drop_exception(e);
        return nullptr;
    }
    return local;
}

// 调用静态无参方法返回字符串（如 MessageDigest.getInstance）
jobject call_static_string(JNIEnv* e, const char* cls_name, const char* method,
                           const char* signature, const std::string& arg) {
    jclass cls = find_class(e, cls_name);
    if (!cls) return nullptr;
    const jmethodID fn = e->GetStaticMethodID(cls, method, signature);
    if (!fn) {
        e->DeleteLocalRef(cls);
        return nullptr;
    }
    jstring s = lb::android::to_jstring(e, arg);
    jobject out = e->CallStaticObjectMethod(cls, fn, s);
    e->DeleteLocalRef(s);
    e->DeleteLocalRef(cls);
    if (e->ExceptionCheck()) {
        drop_exception(e);
        return nullptr;
    }
    return out;
}

ByteBuf digest_with(JNIEnv* e, const std::string& algorithm, const void* data, size_t n) {
    jobject md = call_static_string(e, "java/security/MessageDigest", "getInstance",
                                    "(Ljava/lang/String;)Ljava/security/MessageDigest;", algorithm);
    if (!md) return {};
    jclass cls = e->GetObjectClass(md);
    const jmethodID update = e->GetMethodID(cls, "update", "([B)V");
    const jmethodID finish = e->GetMethodID(cls, "digest", "()[B");
    ByteBuf input;
    if (n > 0 && data)
        input.assign(static_cast<const std::uint8_t*>(data),
                     static_cast<const std::uint8_t*>(data) + n);
    jbyteArray arr = to_jbytes(e, input);
    e->CallVoidMethod(md, update, arr);
    e->DeleteLocalRef(arr);
    auto out = static_cast<jbyteArray>(e->CallObjectMethod(md, finish));
    ByteBuf result;
    if (!e->ExceptionCheck() && out) result = from_jbytes(e, out);
    else
        drop_exception(e);
    if (out) e->DeleteLocalRef(out);
    e->DeleteLocalRef(cls);
    e->DeleteLocalRef(md);
    return result;
}

jobject make_secret_key(JNIEnv* e, const ByteBuf& key, const std::string& algorithm) {
    jclass cls = find_class(e, "javax/crypto/spec/SecretKeySpec");
    if (!cls) return nullptr;
    const jmethodID ctor = e->GetMethodID(cls, "<init>", "([BLjava/lang/String;)V");
    jbyteArray raw = to_jbytes(e, key);
    jstring alg = lb::android::to_jstring(e, algorithm);
    jobject spec = e->NewObject(cls, ctor, raw, alg);
    e->DeleteLocalRef(alg);
    e->DeleteLocalRef(raw);
    e->DeleteLocalRef(cls);
    if (e->ExceptionCheck()) {
        drop_exception(e);
        return nullptr;
    }
    return spec;
}

jobject public_key_from_raw(JNIEnv* e, const ByteBuf& pub_raw) {
    if (pub_raw.size() != 65 || pub_raw[0] != 0x04) return nullptr;

    jobject params = nullptr;
    {
        jobject ap = call_static_string(e, "java/security/AlgorithmParameters", "getInstance",
                                        "(Ljava/lang/String;)Ljava/security/AlgorithmParameters;",
                                        "EC");
        if (!ap) return nullptr;
        jclass ap_cls = e->GetObjectClass(ap);
        jclass spec_cls = find_class(e, "java/security/spec/ECGenParameterSpec");
        const jmethodID spec_ctor = e->GetMethodID(spec_cls, "<init>", "(Ljava/lang/String;)V");
        jstring curve = lb::android::to_jstring(e, kECCurve);
        jobject gen_spec = e->NewObject(spec_cls, spec_ctor, curve);
        e->DeleteLocalRef(curve);
        const jmethodID init = e->GetMethodID(ap_cls, "init", "(Ljava/security/spec/AlgorithmParameterSpec;)V");
        e->CallVoidMethod(ap, init, gen_spec);
        e->DeleteLocalRef(gen_spec);
        jclass param_cls = find_class(e, "java/security/spec/ECParameterSpec");
        const jmethodID get_spec =
            e->GetMethodID(ap_cls, "getParameterSpec", "(Ljava/lang/Class;)Ljava/security/spec/AlgorithmParameterSpec;");
        params = e->CallObjectMethod(ap, get_spec, param_cls);
        e->DeleteLocalRef(param_cls);
        e->DeleteLocalRef(ap_cls);
        e->DeleteLocalRef(ap);
        if (e->ExceptionCheck()) {
            drop_exception(e);
            return nullptr;
        }
    }
    if (!params) return nullptr;

    // 拆分 65 字节公钥为 X/Y 两个 BigInteger（用字节构造：Java 的 BigInteger(String)
    // 只按十进制解析，不接受 "0x" 前缀）
    auto to_bigint = [&](const std::uint8_t* p) {
        jclass cls = find_class(e, "java/math/BigInteger");
        if (!cls) return static_cast<jobject>(nullptr);
        const jmethodID ctor = e->GetMethodID(cls, "<init>", "([B)V");
        ByteBuf mag(33, 0);  // 前置 0x00 保证按正数解析
        std::copy(p, p + 32, mag.begin() + 1);
        jbyteArray arr = to_jbytes(e, mag);
        jobject out = e->NewObject(cls, ctor, arr);
        e->DeleteLocalRef(arr);
        e->DeleteLocalRef(cls);
        if (e->ExceptionCheck()) {
            drop_exception(e);
            return static_cast<jobject>(nullptr);
        }
        return out;
    };
    jobject x = to_bigint(pub_raw.data() + 1);
    jobject y = to_bigint(pub_raw.data() + 33);
    if (!x || !y) {
        if (x) e->DeleteLocalRef(x);
        if (y) e->DeleteLocalRef(y);
        e->DeleteLocalRef(params);
        return nullptr;
    }

    jclass point_cls = find_class(e, "java/security/spec/ECPoint");
    const jmethodID point_ctor =
        e->GetMethodID(point_cls, "<init>", "(Ljava/math/BigInteger;Ljava/math/BigInteger;)V");
    jobject point = e->NewObject(point_cls, point_ctor, x, y);
    jclass key_spec_cls = find_class(e, "java/security/spec/ECPublicKeySpec");
    const jmethodID key_spec_ctor =
        e->GetMethodID(key_spec_cls, "<init>",
                       "(Ljava/security/spec/ECPoint;Ljava/security/spec/ECParameterSpec;)V");
    jobject key_spec = e->NewObject(key_spec_cls, key_spec_ctor, point, params);

    jobject kf = call_static_string(e, "java/security/KeyFactory", "getInstance",
                                    "(Ljava/lang/String;)Ljava/security/KeyFactory;", "EC");
    if (!kf) {
        e->DeleteLocalRef(key_spec);
        e->DeleteLocalRef(point);
        e->DeleteLocalRef(x);
        e->DeleteLocalRef(y);
        e->DeleteLocalRef(point_cls);
        e->DeleteLocalRef(key_spec_cls);
        e->DeleteLocalRef(params);
        drop_exception(e);
        return nullptr;
    }
    jclass kf_cls = e->GetObjectClass(kf);
    const jmethodID gen =
        e->GetMethodID(kf_cls, "generatePublic",
                       "(Ljava/security/spec/KeySpec;)Ljava/security/PublicKey;");
    jobject pub = e->CallObjectMethod(kf, gen, key_spec);
    e->DeleteLocalRef(kf_cls);
    e->DeleteLocalRef(kf);
    e->DeleteLocalRef(key_spec);
    e->DeleteLocalRef(point);
    e->DeleteLocalRef(x);
    e->DeleteLocalRef(y);
    e->DeleteLocalRef(point_cls);
    e->DeleteLocalRef(key_spec_cls);
    e->DeleteLocalRef(params);
    if (e->ExceptionCheck()) {
        drop_exception(e);
        return nullptr;
    }
    return pub;
}

jobject private_key_from_blob(JNIEnv* e, const ByteBuf& priv_blob) {
    // 生成时会把 65 字节公钥追加在 PKCS#8 之后，导入私钥前需去掉
    ByteBuf der = priv_blob;
    if (der.size() > 65 && der[der.size() - 65] == 0x04) der.resize(der.size() - 65);
    jobject kf = call_static_string(e, "java/security/KeyFactory", "getInstance",
                                    "(Ljava/lang/String;)Ljava/security/KeyFactory;", "EC");
    if (!kf) return nullptr;
    jclass spec_cls = find_class(e, "java/security/spec/PKCS8EncodedKeySpec");
    const jmethodID spec_ctor = e->GetMethodID(spec_cls, "<init>", "([B)V");
    jbyteArray raw = to_jbytes(e, der);
    jobject spec = e->NewObject(spec_cls, spec_ctor, raw);
    jclass kf_cls = e->GetObjectClass(kf);
    const jmethodID gen =
        e->GetMethodID(kf_cls, "generatePrivate",
                       "(Ljava/security/spec/KeySpec;)Ljava/security/PrivateKey;");
    jobject key = e->CallObjectMethod(kf, gen, spec);
    e->DeleteLocalRef(kf_cls);
    e->DeleteLocalRef(kf);
    e->DeleteLocalRef(spec);
    e->DeleteLocalRef(spec_cls);
    e->DeleteLocalRef(raw);
    if (e->ExceptionCheck()) {
        drop_exception(e);
        return nullptr;
    }
    return key;
}

Expected<ByteBuf> generate_private_pkcs8(JNIEnv* e) {
    jclass gen_cls = find_class(e, "java/security/KeyPairGenerator");
    const jmethodID get_inst = e->GetStaticMethodID(
        gen_cls, "getInstance", "(Ljava/lang/String;)Ljava/security/KeyPairGenerator;");
    jstring alg = lb::android::to_jstring(e, "EC");
    jobject gen = e->CallStaticObjectMethod(gen_cls, get_inst, alg);
    e->DeleteLocalRef(alg);
    jclass spec_cls = find_class(e, "java/security/spec/ECGenParameterSpec");
    const jmethodID spec_ctor = e->GetMethodID(spec_cls, "<init>", "(Ljava/lang/String;)V");
    jstring curve = lb::android::to_jstring(e, kECCurve);
    jobject spec = e->NewObject(spec_cls, spec_ctor, curve);
    e->DeleteLocalRef(curve);
    jclass gen_obj_cls = e->GetObjectClass(gen);
    const jmethodID init =
        e->GetMethodID(gen_obj_cls, "initialize", "(Ljava/security/spec/AlgorithmParameterSpec;)V");
    e->CallVoidMethod(gen, init, spec);
    const jmethodID gen_key = e->GetMethodID(gen_obj_cls, "generateKeyPair", "()Ljava/security/KeyPair;");
    jobject pair = e->CallObjectMethod(gen, gen_key);
    e->DeleteLocalRef(gen_obj_cls);
    e->DeleteLocalRef(spec);
    e->DeleteLocalRef(spec_cls);
    e->DeleteLocalRef(gen);
    e->DeleteLocalRef(gen_cls);
    if (e->ExceptionCheck() || !pair) {
        drop_exception(e);
        return Expected<ByteBuf>::fail("ECDSA 生成密钥失败");
    }
    jclass pair_cls = e->GetObjectClass(pair);
    const jmethodID get_priv = e->GetMethodID(pair_cls, "getPrivate", "()Ljava/security/PrivateKey;");
    jobject priv = e->CallObjectMethod(pair, get_priv);
    if (e->ExceptionCheck() || !priv) {
        drop_exception(e);
        e->DeleteLocalRef(pair_cls);
        e->DeleteLocalRef(pair);
        return Expected<ByteBuf>::fail("ECDSA 取私钥失败");
    }
    jclass priv_cls = e->GetObjectClass(priv);
    const jmethodID encoded = e->GetMethodID(priv_cls, "getEncoded", "()[B");
    auto bytes = static_cast<jbyteArray>(e->CallObjectMethod(priv, encoded));
    ByteBuf out;
    if (!e->ExceptionCheck() && bytes) out = from_jbytes(e, bytes);
    else
        drop_exception(e);
    if (bytes) e->DeleteLocalRef(bytes);
    e->DeleteLocalRef(priv_cls);
    e->DeleteLocalRef(priv);

    // 同步取出公钥：JCA 无法从 EC 私钥推导公钥，因此生成时就把 0x04||X||Y 追加到 blob 尾部
    ByteBuf pub_raw;
    const jmethodID get_pub = e->GetMethodID(pair_cls, "getPublic", "()Ljava/security/PublicKey;");
    jobject pub = e->CallObjectMethod(pair, get_pub);
    e->DeleteLocalRef(pair_cls);
    e->DeleteLocalRef(pair);
    if (!e->ExceptionCheck() && pub) {
        jclass pub_cls = e->GetObjectClass(pub);
        const jmethodID get_w =
            e->GetMethodID(pub_cls, "getW", "()Ljava/security/spec/ECPoint;");
        jobject w = e->CallObjectMethod(pub, get_w);
        e->DeleteLocalRef(pub_cls);
        e->DeleteLocalRef(pub);
        if (!e->ExceptionCheck() && w) {
            jclass w_cls = e->GetObjectClass(w);
            // ECPoint 的 x/y 为 private 字段，JNI 只能走 getAffineX()/getAffineY()
            const jmethodID get_ax =
                e->GetMethodID(w_cls, "getAffineX", "()Ljava/math/BigInteger;");
            const jmethodID get_ay =
                e->GetMethodID(w_cls, "getAffineY", "()Ljava/math/BigInteger;");
            jobject x = e->CallObjectMethod(w, get_ax);
            jobject y = e->CallObjectMethod(w, get_ay);
            e->DeleteLocalRef(w_cls);
            e->DeleteLocalRef(w);
            auto fixed32 = [&](jobject v) {
                ByteBuf buf(32, 0);
                if (!v) return buf;
                jclass cls = e->GetObjectClass(v);
                const jmethodID to = e->GetMethodID(cls, "toByteArray", "()[B");
                auto arr = static_cast<jbyteArray>(e->CallObjectMethod(v, to));
                if (!e->ExceptionCheck() && arr) {
                    ByteBuf b = from_jbytes(e, arr);
                    const size_t n = std::min<size_t>(b.size(), 32);
                    std::copy(b.end() - n, b.end(), buf.end() - static_cast<long>(n));
                } else {
                    drop_exception(e);
                }
                if (arr) e->DeleteLocalRef(arr);
                e->DeleteLocalRef(cls);
                e->DeleteLocalRef(v);
                return buf;
            };
            ByteBuf xb = fixed32(x);
            ByteBuf yb = fixed32(y);
            pub_raw.reserve(65);
            pub_raw.push_back(0x04);
            pub_raw.insert(pub_raw.end(), xb.begin(), xb.end());
            pub_raw.insert(pub_raw.end(), yb.begin(), yb.end());
        } else {
            drop_exception(e);
        }
    } else {
        drop_exception(e);
    }
    if (out.empty()) return Expected<ByteBuf>::fail("ECDSA 私钥导出失败");
    if (pub_raw.size() != 65) return Expected<ByteBuf>::fail("ECDSA 公钥导出失败");
    out.insert(out.end(), pub_raw.begin(), pub_raw.end());
    return Expected<ByteBuf>::success(std::move(out));
}

// 公钥未压缩点（65 字节）由生成时追加在私钥 blob 尾部（JCA 不允许从 EC 私钥推导公钥）
Expected<ByteBuf> public_from_private(JNIEnv*, const ByteBuf& priv_blob) {
    if (priv_blob.size() <= 65 || priv_blob[priv_blob.size() - 65] != 0x04)
        return Expected<ByteBuf>::fail("私钥 blob 缺少公钥");
    ByteBuf out(priv_blob.end() - 65, priv_blob.end());
    return Expected<ByteBuf>::success(std::move(out));
}

// ---- DER <-> r||s ----
ByteBuf der_to_raw64(const ByteBuf& der) {
    if (der.size() < 8 || der[0] != 0x30) return {};
    size_t pos = 2;
    if (der[1] & 0x80) pos = 2 + (der[1] & 0x7f);
    if (pos + 2 > der.size() || der[pos] != 0x02) return {};
    const size_t r_len = der[pos + 1];
    pos += 2;
    if (pos + r_len + 2 > der.size()) return {};
    ByteBuf r(der.begin() + static_cast<long>(pos), der.begin() + static_cast<long>(pos + r_len));
    pos += r_len;
    if (pos + 2 > der.size() || der[pos] != 0x02) return {};
    const size_t s_len = der[pos + 1];
    pos += 2;
    if (pos + s_len > der.size()) return {};

    auto normalize = [](const ByteBuf& v) {
        ByteBuf out = v;
        while (out.size() > 1 && out.front() == 0) out.erase(out.begin());
        return out;
    };
    ByteBuf out(64, 0);
    r = normalize(r);
    ByteBuf s(der.begin() + static_cast<long>(pos), der.begin() + static_cast<long>(pos + s_len));
    s = normalize(s);
    if (r.size() > 32 || s.size() > 32) return {};
    std::copy(r.begin(), r.end(), out.begin() + (32 - r.size()));
    std::copy(s.begin(), s.end(), out.begin() + 32 + (32 - s.size()));
    return out;
}

void append_der_integer(ByteBuf& out, const std::uint8_t* p, size_t len) {
    while (len > 1 && *p == 0) {
        ++p;
        --len;
    }
    out.push_back(0x02);
    if (*p & 0x80) {
        out.push_back(static_cast<std::uint8_t>(len + 1));
        out.push_back(0x00);
        out.insert(out.end(), p, p + len);
    } else {
        out.push_back(static_cast<std::uint8_t>(len));
        out.insert(out.end(), p, p + len);
    }
}

ByteBuf raw64_to_der(const ByteBuf& raw) {
    if (raw.size() != 64) return {};
    ByteBuf body;
    append_der_integer(body, raw.data(), 32);
    append_der_integer(body, raw.data() + 32, 32);
    ByteBuf der;
    der.push_back(0x30);
    der.push_back(static_cast<std::uint8_t>(body.size()));
    der.insert(der.end(), body.begin(), body.end());
    return der;
}

} // namespace

ByteBuf random_bytes(size_t n) {
    ScopedEnv s;
    if (!s) return {};
    jclass cls = find_class(s.e, "java/security/SecureRandom");
    if (!cls) return {};
    const jmethodID ctor = s.e->GetMethodID(cls, "<init>", "()V");
    jobject rng = s.e->NewObject(cls, ctor);
    const jmethodID next = s.e->GetMethodID(cls, "nextBytes", "([B)V");
    jbyteArray arr = s.e->NewByteArray(static_cast<jsize>(n));
    s.e->CallVoidMethod(rng, next, arr);
    ByteBuf out;
    if (!s.e->ExceptionCheck()) out = from_jbytes(s.e, arr);
    else
        drop_exception(s.e);
    s.e->DeleteLocalRef(arr);
    s.e->DeleteLocalRef(rng);
    s.e->DeleteLocalRef(cls);
    return out;
}

ByteBuf sha256(const void* data, size_t n) {
    ScopedEnv s;
    if (!s) return {};
    return digest_with(s.e, "SHA-256", data, n);
}

ByteBuf sha256(const ByteBuf& b) { return sha256(b.data(), b.size()); }

struct Sha256Hasher::Impl {
    jobject md = nullptr;
    jmethodID update = nullptr;
    jmethodID finish = nullptr;
    ~Impl() {
        if (!md) return;
        ScopedEnv s;
        if (s) s.e->DeleteGlobalRef(md);
    }
};

Sha256Hasher::Sha256Hasher() : impl_(std::make_unique<Impl>()) {
    ScopedEnv s;
    if (!s) return;
    jobject md = call_static_string(s.e, "java/security/MessageDigest", "getInstance",
                                    "(Ljava/lang/String;)Ljava/security/MessageDigest;", "SHA-256");
    if (!md) return;
    impl_->md = s.e->NewGlobalRef(md);
    jclass cls = s.e->GetObjectClass(md);
    impl_->update = s.e->GetMethodID(cls, "update", "([B)V");
    impl_->finish = s.e->GetMethodID(cls, "digest", "()[B");
    s.e->DeleteLocalRef(cls);
    s.e->DeleteLocalRef(md);
}

Sha256Hasher::~Sha256Hasher() = default;

bool Sha256Hasher::update(const void* data, size_t n) {
    if (!impl_->md || !impl_->update) return false;
    ScopedEnv s;
    if (!s) return false;
    ByteBuf input;
    if (n > 0 && data)
        input.assign(static_cast<const std::uint8_t*>(data),
                     static_cast<const std::uint8_t*>(data) + n);
    jbyteArray arr = to_jbytes(s.e, input);
    s.e->CallVoidMethod(impl_->md, impl_->update, arr);
    s.e->DeleteLocalRef(arr);
    if (s.e->ExceptionCheck()) {
        drop_exception(s.e);
        return false;
    }
    return true;
}

ByteBuf Sha256Hasher::finish() {
    if (!impl_->md || !impl_->finish) return {};
    ScopedEnv s;
    if (!s) return {};
    auto out = static_cast<jbyteArray>(s.e->CallObjectMethod(impl_->md, impl_->finish));
    ByteBuf result;
    if (!s.e->ExceptionCheck() && out) result = from_jbytes(s.e, out);
    else
        drop_exception(s.e);
    if (out) s.e->DeleteLocalRef(out);
    return result;
}

ByteBuf hmac_sha256(const ByteBuf& key, const void* data, size_t n) {
    ScopedEnv s;
    if (!s) return {};
    jclass mac_cls = find_class(s.e, "javax/crypto/Mac");
    if (!mac_cls) return {};
    const jmethodID get_inst = s.e->GetStaticMethodID(
        mac_cls, "getInstance", "(Ljava/lang/String;)Ljavax/crypto/Mac;");
    jstring alg = lb::android::to_jstring(s.e, "HmacSHA256");
    jobject mac = s.e->CallStaticObjectMethod(mac_cls, get_inst, alg);
    s.e->DeleteLocalRef(alg);
    s.e->DeleteLocalRef(mac_cls);
    jobject spec = make_secret_key(s.e, key, "HmacSHA256");
    if (!mac || !spec) {
        drop_exception(s.e);
        return {};
    }
    jclass mac_obj_cls = s.e->GetObjectClass(mac);
    const jmethodID init = s.e->GetMethodID(mac_obj_cls, "init", "(Ljava/security/Key;)V");
    s.e->CallVoidMethod(mac, init, spec);
    const jmethodID do_final = s.e->GetMethodID(mac_obj_cls, "doFinal", "([B)[B");
    ByteBuf input;
    if (n > 0 && data)
        input.assign(static_cast<const std::uint8_t*>(data),
                     static_cast<const std::uint8_t*>(data) + n);
    jbyteArray arr = to_jbytes(s.e, input);
    auto out = static_cast<jbyteArray>(s.e->CallObjectMethod(mac, do_final, arr));
    ByteBuf result;
    if (!s.e->ExceptionCheck() && out) result = from_jbytes(s.e, out);
    else
        drop_exception(s.e);
    if (out) s.e->DeleteLocalRef(out);
    s.e->DeleteLocalRef(arr);
    s.e->DeleteLocalRef(mac_obj_cls);
    s.e->DeleteLocalRef(mac);
    s.e->DeleteLocalRef(spec);
    return result;
}

ByteBuf hmac_sha256(const ByteBuf& key, const ByteBuf& data) {
    return hmac_sha256(key, data.data(), data.size());
}

ByteBuf hkdf_sha256(const ByteBuf& ikm, const ByteBuf& salt, const std::string& info,
                    size_t out_len) {
    ByteBuf real_salt = salt;
    if (real_salt.empty()) real_salt.assign(32, 0);
    const ByteBuf prk = hmac_sha256(real_salt, ikm);
    ByteBuf out;
    ByteBuf prev;
    std::uint8_t counter = 1;
    while (out.size() < out_len) {
        ByteBuf block = prev;
        block.insert(block.end(), info.begin(), info.end());
        block.push_back(counter++);
        prev = hmac_sha256(prk, block);
        out.insert(out.end(), prev.begin(), prev.end());
    }
    out.resize(out_len);
    return out;
}

ByteBuf aead_encrypt(const ByteBuf& key, const ByteBuf& nonce, const ByteBuf& plaintext,
                     const ByteBuf& aad) {
    ScopedEnv s;
    if (!s || nonce.size() != kAeadNonceLen) return {};
    jclass cipher_cls = find_class(s.e, "javax/crypto/Cipher");
    const jmethodID get_inst =
        s.e->GetStaticMethodID(cipher_cls, "getInstance", "(Ljava/lang/String;)Ljavax/crypto/Cipher;");
    jstring alg = lb::android::to_jstring(s.e, "AES/GCM/NoPadding");
    jobject cipher = s.e->CallStaticObjectMethod(cipher_cls, get_inst, alg);
    s.e->DeleteLocalRef(alg);
    s.e->DeleteLocalRef(cipher_cls);
    jobject spec_key = make_secret_key(s.e, key, "AES");
    jclass gcm_cls = find_class(s.e, "javax/crypto/spec/GCMParameterSpec");
    const jmethodID gcm_ctor = s.e->GetMethodID(gcm_cls, "<init>", "(I[B)V");
    jbyteArray iv = to_jbytes(s.e, nonce);
    jobject gcm = s.e->NewObject(gcm_cls, gcm_ctor, kAeadTagBits, iv);
    s.e->DeleteLocalRef(iv);
    s.e->DeleteLocalRef(gcm_cls);
    if (!cipher || !spec_key || !gcm) {
        drop_exception(s.e);
        return {};
    }
    jclass cipher_obj_cls = s.e->GetObjectClass(cipher);
    const jmethodID init = s.e->GetMethodID(
        cipher_obj_cls, "init", "(ILjava/security/Key;Ljava/security/spec/AlgorithmParameterSpec;)V");
    s.e->CallVoidMethod(cipher, init, 1 /*ENCRYPT_MODE*/, spec_key, gcm);
    const jmethodID update_aad = s.e->GetMethodID(cipher_obj_cls, "updateAAD", "([B)V");
    const jmethodID do_final = s.e->GetMethodID(cipher_obj_cls, "doFinal", "([B)[B");

    ByteBuf out;
    jbyteArray aad_arr = to_jbytes(s.e, aad);
    s.e->CallVoidMethod(cipher, update_aad, aad_arr);
    s.e->DeleteLocalRef(aad_arr);

    jbyteArray pt = to_jbytes(s.e, plaintext);
    auto ct = static_cast<jbyteArray>(s.e->CallObjectMethod(cipher, do_final, pt));
    s.e->DeleteLocalRef(pt);
    if (!s.e->ExceptionCheck() && ct) out = from_jbytes(s.e, ct);
    else
        drop_exception(s.e);
    if (ct) s.e->DeleteLocalRef(ct);
    s.e->DeleteLocalRef(cipher_obj_cls);
    s.e->DeleteLocalRef(cipher);
    s.e->DeleteLocalRef(spec_key);
    s.e->DeleteLocalRef(gcm);
    return out;
}

Expected<ByteBuf> aead_decrypt(const ByteBuf& key, const ByteBuf& nonce,
                               const ByteBuf& ciphertext, const ByteBuf& aad) {
    ScopedEnv s;
    if (!s || nonce.size() != kAeadNonceLen || ciphertext.size() < kAeadTagLen)
        return Expected<ByteBuf>::fail("GCM 参数非法");
    jclass cipher_cls = find_class(s.e, "javax/crypto/Cipher");
    const jmethodID get_inst =
        s.e->GetStaticMethodID(cipher_cls, "getInstance", "(Ljava/lang/String;)Ljavax/crypto/Cipher;");
    jstring alg = lb::android::to_jstring(s.e, "AES/GCM/NoPadding");
    jobject cipher = s.e->CallStaticObjectMethod(cipher_cls, get_inst, alg);
    s.e->DeleteLocalRef(alg);
    s.e->DeleteLocalRef(cipher_cls);
    jobject spec_key = make_secret_key(s.e, key, "AES");
    jclass gcm_cls = find_class(s.e, "javax/crypto/spec/GCMParameterSpec");
    const jmethodID gcm_ctor = s.e->GetMethodID(gcm_cls, "<init>", "(I[B)V");
    jbyteArray iv = to_jbytes(s.e, nonce);
    jobject gcm = s.e->NewObject(gcm_cls, gcm_ctor, kAeadTagBits, iv);
    s.e->DeleteLocalRef(iv);
    s.e->DeleteLocalRef(gcm_cls);
    if (!cipher || !spec_key || !gcm) {
        drop_exception(s.e);
        return Expected<ByteBuf>::fail("GCM 初始化失败");
    }
    jclass cipher_obj_cls = s.e->GetObjectClass(cipher);
    const jmethodID init = s.e->GetMethodID(
        cipher_obj_cls, "init", "(ILjava/security/Key;Ljava/security/spec/AlgorithmParameterSpec;)V");
    s.e->CallVoidMethod(cipher, init, 2 /*DECRYPT_MODE*/, spec_key, gcm);
    const jmethodID update_aad = s.e->GetMethodID(cipher_obj_cls, "updateAAD", "([B)V");
    const jmethodID do_final = s.e->GetMethodID(cipher_obj_cls, "doFinal", "([B)[B");

    jbyteArray aad_arr = to_jbytes(s.e, aad);
    s.e->CallVoidMethod(cipher, update_aad, aad_arr);
    s.e->DeleteLocalRef(aad_arr);

    jbyteArray ct = to_jbytes(s.e, ciphertext);
    auto pt = static_cast<jbyteArray>(s.e->CallObjectMethod(cipher, do_final, ct));
    s.e->DeleteLocalRef(ct);
    Expected<ByteBuf> result = Expected<ByteBuf>::fail("GCM 解密失败");
    if (!s.e->ExceptionCheck() && pt) result = Expected<ByteBuf>::success(from_jbytes(s.e, pt));
    else
        drop_exception(s.e);
    if (pt) s.e->DeleteLocalRef(pt);
    s.e->DeleteLocalRef(cipher_obj_cls);
    s.e->DeleteLocalRef(cipher);
    s.e->DeleteLocalRef(spec_key);
    s.e->DeleteLocalRef(gcm);
    return result;
}

Expected<ByteBuf> ecdsa_generate_private() {
    ScopedEnv s;
    if (!s) return Expected<ByteBuf>::fail("no JNI env");
    return generate_private_pkcs8(s.e);
}

Expected<ByteBuf> ecdsa_public_raw(const ByteBuf& priv_blob) {
    ScopedEnv s;
    if (!s) return Expected<ByteBuf>::fail("no JNI env");
    return public_from_private(s.e, priv_blob);
}

ByteBuf ecdsa_sign(const ByteBuf& priv_blob, const ByteBuf& message) {
    ScopedEnv s;
    if (!s) return {};
    jobject priv = private_key_from_blob(s.e, priv_blob);
    if (!priv) return {};
    jobject sig = call_static_string(s.e, "java/security/Signature", "getInstance",
                                     "(Ljava/lang/String;)Ljava/security/Signature;",
                                     "SHA256withECDSA");
    if (!sig) {
        s.e->DeleteLocalRef(priv);
        return {};
    }
    jclass sig_cls = s.e->GetObjectClass(sig);
    const jmethodID init = s.e->GetMethodID(sig_cls, "initSign", "(Ljava/security/PrivateKey;)V");
    const jmethodID update = s.e->GetMethodID(sig_cls, "update", "([B)V");
    const jmethodID sign = s.e->GetMethodID(sig_cls, "sign", "()[B");
    s.e->CallVoidMethod(sig, init, priv);
    jbyteArray msg = to_jbytes(s.e, message);
    s.e->CallVoidMethod(sig, update, msg);
    auto der = static_cast<jbyteArray>(s.e->CallObjectMethod(sig, sign));
    ByteBuf der_bytes;
    if (!s.e->ExceptionCheck() && der) der_bytes = from_jbytes(s.e, der);
    else
        drop_exception(s.e);
    if (der) s.e->DeleteLocalRef(der);
    s.e->DeleteLocalRef(msg);
    s.e->DeleteLocalRef(sig_cls);
    s.e->DeleteLocalRef(sig);
    s.e->DeleteLocalRef(priv);
    return der_to_raw64(der_bytes);
}

bool ecdsa_verify(const ByteBuf& pub_raw, const ByteBuf& message, const ByteBuf& sig) {
    ScopedEnv s;
    if (!s) return false;
    if (pub_raw.size() != 65 || sig.size() != 64) return false;
    jobject pub = public_key_from_raw(s.e, pub_raw);
    if (!pub) return false;
    jobject verifier = call_static_string(s.e, "java/security/Signature", "getInstance",
                                          "(Ljava/lang/String;)Ljava/security/Signature;",
                                          "SHA256withECDSA");
    if (!verifier) {
        s.e->DeleteLocalRef(pub);
        return false;
    }
    jclass cls = s.e->GetObjectClass(verifier);
    const jmethodID init = s.e->GetMethodID(cls, "initVerify", "(Ljava/security/PublicKey;)V");
    const jmethodID update = s.e->GetMethodID(cls, "update", "([B)V");
    const jmethodID verify = s.e->GetMethodID(cls, "verify", "([B)Z");
    s.e->CallVoidMethod(verifier, init, pub);
    jbyteArray msg = to_jbytes(s.e, message);
    s.e->CallVoidMethod(verifier, update, msg);
    const ByteBuf der = raw64_to_der(sig);
    jbyteArray der_arr = to_jbytes(s.e, der);
    const bool ok = !s.e->ExceptionCheck() && s.e->CallBooleanMethod(verifier, verify, der_arr);
    if (s.e->ExceptionCheck()) drop_exception(s.e);
    s.e->DeleteLocalRef(der_arr);
    s.e->DeleteLocalRef(msg);
    s.e->DeleteLocalRef(cls);
    s.e->DeleteLocalRef(verifier);
    s.e->DeleteLocalRef(pub);
    return ok;
}

Expected<ByteBuf> ecdh_generate_private() {
    ScopedEnv s;
    if (!s) return Expected<ByteBuf>::fail("no JNI env");
    return generate_private_pkcs8(s.e);
}

Expected<ByteBuf> ecdh_public_raw(const ByteBuf& priv_blob) {
    ScopedEnv s;
    if (!s) return Expected<ByteBuf>::fail("no JNI env");
    return public_from_private(s.e, priv_blob);
}

Expected<ByteBuf> ecdh_shared_secret(const ByteBuf& priv_blob, const ByteBuf& peer_pub_raw) {
    ScopedEnv s;
    if (!s) return Expected<ByteBuf>::fail("no JNI env");
    jobject priv = private_key_from_blob(s.e, priv_blob);
    if (!priv) return Expected<ByteBuf>::fail("解析 ECDH 私钥失败");
    jobject pub = public_key_from_raw(s.e, peer_pub_raw);
    if (!pub) {
        s.e->DeleteLocalRef(priv);
        return Expected<ByteBuf>::fail("解析 ECDH 公钥失败");
    }
    jobject ka = call_static_string(s.e, "javax/crypto/KeyAgreement", "getInstance",
                                    "(Ljava/lang/String;)Ljavax/crypto/KeyAgreement;", "ECDH");
    if (!ka) {
        s.e->DeleteLocalRef(pub);
        s.e->DeleteLocalRef(priv);
        return Expected<ByteBuf>::fail("KeyAgreement 初始化失败");
    }
    jclass cls = s.e->GetObjectClass(ka);
    const jmethodID init = s.e->GetMethodID(cls, "init", "(Ljava/security/Key;)V");
    const jmethodID phase = s.e->GetMethodID(cls, "doPhase", "(Ljava/security/Key;Z)Ljava/security/Key;");
    const jmethodID secret = s.e->GetMethodID(cls, "generateSecret", "()[B");
    s.e->CallVoidMethod(ka, init, priv);
    s.e->CallObjectMethod(ka, phase, pub, JNI_TRUE);
    auto bytes = static_cast<jbyteArray>(s.e->CallObjectMethod(ka, secret));
    Expected<ByteBuf> result = Expected<ByteBuf>::fail("ECDH 协商失败");
    if (!s.e->ExceptionCheck() && bytes) result = Expected<ByteBuf>::success(from_jbytes(s.e, bytes));
    else
        drop_exception(s.e);
    if (bytes) s.e->DeleteLocalRef(bytes);
    s.e->DeleteLocalRef(cls);
    s.e->DeleteLocalRef(ka);
    s.e->DeleteLocalRef(pub);
    s.e->DeleteLocalRef(priv);
    return result;
}

} // namespace lb::crypto
