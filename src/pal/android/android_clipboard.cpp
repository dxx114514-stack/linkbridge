// Android 剪贴板后端：通过 JNI 调用 Kotlin ClipboardBridge（ClipboardManager 封装）。
// Android 10+ 对后台读取剪贴板有系统限制；本实现采用轮询，前台/前台服务下可用。
#include "core/clipboard/backend.h"
#include "pal/android/android_jni.h"

#include <chrono>
#include <thread>

namespace lb {
namespace {

constexpr const char* kClipboardBridge = "com/linkbridge/app/ClipboardBridge";

struct Bridge {
    JNIEnv* e = nullptr;
    jclass cls = nullptr;

    Bridge() {
        e = android::env();
        if (e) cls = android::class_ref(kClipboardBridge);
    }
    ~Bridge() { android::release(e); }
    explicit operator bool() const { return e && cls; }
};

class AndroidClipboard final : public IClipboardBackend {
public:
    bool wait_for_change(int timeout_ms) override {
        if (timeout_ms <= 0) return false;
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        const std::uint64_t start = sequence();
        while (std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (sequence() != start) return true;
        }
        return false;
    }

    std::uint64_t sequence() override {
        Bridge b;
        if (!b) return 0;
        const jmethodID fn = b.e->GetStaticMethodID(b.cls, "sequence", "()J");
        if (!fn) {
            b.e->ExceptionClear();
            return 0;
        }
        const jlong v = b.e->CallStaticLongMethod(b.cls, fn);
        if (b.e->ExceptionCheck()) {
            b.e->ExceptionClear();
            return 0;
        }
        return static_cast<std::uint64_t>(v);
    }

    Expected<ClipboardContent> read() override {
        ClipboardContent content;
        Bridge b;
        if (!b) return Expected<ClipboardContent>::fail("Android 剪贴板桥接未初始化");
        const jmethodID fn = b.e->GetStaticMethodID(b.cls, "readText", "()[B");
        if (!fn) {
            b.e->ExceptionClear();
            return Expected<ClipboardContent>::fail("Android 剪贴板读取接口缺失");
        }
        auto bytes = static_cast<jbyteArray>(b.e->CallStaticObjectMethod(b.cls, fn));
        if (b.e->ExceptionCheck()) {
            b.e->ExceptionClear();
            return Expected<ClipboardContent>::fail("Android 剪贴板读取失败");
        }
        if (bytes) {
            ByteBuf data = android::from_jbytes(b.e, bytes);
            b.e->DeleteLocalRef(bytes);
            if (!data.empty()) {
                content.mime = "text/plain";
                content.data = std::move(data);
            }
        }
        return Expected<ClipboardContent>::success(std::move(content));
    }

    Result write(const ClipboardContent& content) override {
        if (content.mime != "text/plain") return Result::fail("Android 端暂只同步文本剪贴板");
        if (content.data.empty()) return Result::fail("剪贴板内容为空");
        Bridge b;
        if (!b) return Result::fail("Android 剪贴板桥接未初始化");
        const jmethodID fn = b.e->GetStaticMethodID(b.cls, "writeText", "([B)Z");
        if (!fn) {
            b.e->ExceptionClear();
            return Result::fail("Android 剪贴板写入接口缺失");
        }
        jbyteArray arr = android::to_jbytes(b.e, content.data);
        const jboolean ok = b.e->CallStaticBooleanMethod(b.cls, fn, arr);
        b.e->DeleteLocalRef(arr);
        if (b.e->ExceptionCheck()) {
            b.e->ExceptionClear();
            return Result::fail("Android 剪贴板写入失败");
        }
        return ok ? Result::ok() : Result::fail("Android 剪贴板写入被拒绝");
    }
};

} // namespace

std::shared_ptr<IClipboardBackend> make_system_clipboard() {
    return std::make_shared<AndroidClipboard>();
}

} // namespace lb
