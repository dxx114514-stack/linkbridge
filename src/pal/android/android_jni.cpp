// Android JNI 公共设施实现。
#include "pal/android/android_jni.h"

#include <map>
#include <mutex>
#include <string>

namespace lb::android {
namespace {

JavaVM* g_vm = nullptr;
thread_local bool t_attached = false;
std::mutex g_mu;
std::map<std::string, jclass> g_classes;

constexpr const char* kBridgeClasses[] = {
    "com/linkbridge/app/SecretStore",
    "com/linkbridge/app/ClipboardBridge",
    "com/linkbridge/app/ScreenCaptureBridge",
    "com/linkbridge/app/SyncFileObserver",
    "com/linkbridge/app/NativeBridge",
};

jclass find_and_cache(JNIEnv* e, const char* name) {
    std::lock_guard<std::mutex> lk(g_mu);
    const auto it = g_classes.find(name);
    if (it != g_classes.end()) return it->second;

    jclass local = e->FindClass(name);
    if (!local) {
        e->ExceptionClear();
        return nullptr;
    }
    auto global = static_cast<jclass>(e->NewGlobalRef(local));
    e->DeleteLocalRef(local);
    if (!global) return nullptr;
    g_classes.emplace(name, global);
    return global;
}

} // namespace

bool init(JavaVM* vm) {
    g_vm = vm;
    return g_vm != nullptr;
}

JNIEnv* env() {
    if (!g_vm) return nullptr;
    JNIEnv* e = nullptr;
    const jint rc = g_vm->GetEnv(reinterpret_cast<void**>(&e), JNI_VERSION_1_6);
    if (rc == JNI_OK) return e;
    if (rc == JNI_EDETACHED) {
        if (g_vm->AttachCurrentThread(&e, nullptr) != JNI_OK) return nullptr;
        t_attached = true;
        return e;
    }
    return nullptr;
}

void release(JNIEnv*) {
    if (!g_vm || !t_attached) return;
    g_vm->DetachCurrentThread();
    t_attached = false;
}

bool ensure_classes(JNIEnv* e) {
    if (!e) return false;
    bool ok = true;
    for (const char* name : kBridgeClasses)
        if (!find_and_cache(e, name)) ok = false;
    return ok;
}

jclass class_ref(const char* name) {
    std::lock_guard<std::mutex> lk(g_mu);
    const auto it = g_classes.find(name);
    return it == g_classes.end() ? nullptr : it->second;
}

std::string to_string(JNIEnv* e, jstring s) {
    if (!s) return {};
    const char* chars = e->GetStringUTFChars(s, nullptr);
    if (!chars) return {};
    std::string out(chars);
    e->ReleaseStringUTFChars(s, chars);
    return out;
}

jstring to_jstring(JNIEnv* e, const std::string& s) {
    return e->NewStringUTF(s.c_str());
}

jbyteArray to_jbytes(JNIEnv* e, const ByteBuf& data) {
    auto arr = e->NewByteArray(static_cast<jsize>(data.size()));
    if (!arr) return nullptr;
    if (!data.empty())
        e->SetByteArrayRegion(arr, 0, static_cast<jsize>(data.size()),
                              reinterpret_cast<const jbyte*>(data.data()));
    return arr;
}

ByteBuf from_jbytes(JNIEnv* e, jbyteArray arr) {
    if (!arr) return {};
    const jsize len = e->GetArrayLength(arr);
    ByteBuf out(static_cast<size_t>(len));
    if (len > 0) e->GetByteArrayRegion(arr, 0, len, reinterpret_cast<jbyte*>(out.data()));
    return out;
}

void throw_java(JNIEnv* e, const std::string& message) {
    if (!e) return;
    jclass cls = e->FindClass("java/lang/IllegalStateException");
    if (cls) {
        e->ThrowNew(cls, message.c_str());
        e->DeleteLocalRef(cls);
    }
}

void shutdown(JNIEnv* e) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (e) {
        for (auto& kv : g_classes) e->DeleteGlobalRef(kv.second);
    }
    g_classes.clear();
}

} // namespace lb::android
