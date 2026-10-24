// Android 密钥保护：AES-256-GCM 主密钥由 Android Keystore 托管（Kotlin SecretStore）。
// 落盘格式：nonce(12) || 密文 || tag(16)。
#include "core/platform/platform.h"
#include "pal/android/android_jni.h"

namespace lb::pal {
namespace {

constexpr const char* kSecretStore = "com/linkbridge/app/SecretStore";

} // namespace

ByteBuf protect_secret(const ByteBuf& plain) {
    if (plain.empty()) return {};
    JNIEnv* e = android::env();
    if (!e) return {};
    jclass cls = android::class_ref(kSecretStore);
    if (!cls) {
        android::release(e);
        return {};
    }
    const jmethodID fn = e->GetStaticMethodID(cls, "encrypt", "([B)[B");
    if (!fn) {
        e->ExceptionClear();
        android::release(e);
        return {};
    }
    jbyteArray input = android::to_jbytes(e, plain);
    auto out = static_cast<jbyteArray>(e->CallStaticObjectMethod(cls, fn, input));
    ByteBuf result;
    if (!e->ExceptionCheck() && out) result = android::from_jbytes(e, out);
    else
        e->ExceptionClear();
    if (out) e->DeleteLocalRef(out);
    if (input) e->DeleteLocalRef(input);
    android::release(e);
    return result;
}

Expected<ByteBuf> unprotect_secret(const ByteBuf& sealed) {
    if (sealed.empty()) return Expected<ByteBuf>::fail("keystore: empty input");
    JNIEnv* e = android::env();
    if (!e) return Expected<ByteBuf>::fail("keystore: no JNI env");
    jclass cls = android::class_ref(kSecretStore);
    if (!cls) {
        android::release(e);
        return Expected<ByteBuf>::fail("keystore: SecretStore 未初始化");
    }
    const jmethodID fn = e->GetStaticMethodID(cls, "decrypt", "([B)[B");
    if (!fn) {
        e->ExceptionClear();
        android::release(e);
        return Expected<ByteBuf>::fail("keystore: 缺少 decrypt");
    }
    jbyteArray input = android::to_jbytes(e, sealed);
    auto out = static_cast<jbyteArray>(e->CallStaticObjectMethod(cls, fn, input));
    if (e->ExceptionCheck()) {
        e->ExceptionClear();
        if (out) e->DeleteLocalRef(out);
        if (input) e->DeleteLocalRef(input);
        android::release(e);
        return Expected<ByteBuf>::fail("keystore: 解密失败");
    }
    if (!out) {
        if (input) e->DeleteLocalRef(input);
        android::release(e);
        return Expected<ByteBuf>::fail("keystore: 解密返回空");
    }
    ByteBuf result = android::from_jbytes(e, out);
    e->DeleteLocalRef(out);
    if (input) e->DeleteLocalRef(input);
    android::release(e);
    return Expected<ByteBuf>::success(std::move(result));
}

} // namespace lb::pal
