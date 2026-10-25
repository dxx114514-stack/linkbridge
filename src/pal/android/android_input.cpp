// Android 输入注入：通过 JNI 调用 Kotlin InputBridge（无障碍服务 dispatchGesture / performGlobalAction）。
// 仅当无障碍服务已连接时可用；服务未启用时 inject 返回失败并终止控制会话。
#include "core/screen/backend.h"
#include "pal/android/android_jni.h"

namespace lb {
namespace {

constexpr const char* kInputBridge = "com/linkbridge/app/InputBridge";

int type_code(ScreenInputType type) {
    switch (type) {
        case ScreenInputType::Tap: return 1;
        case ScreenInputType::Swipe: return 2;
        case ScreenInputType::Key: return 3;
    }
    return 0;
}

class AndroidScreenInput final : public IScreenInput {
public:
    Result inject(const ScreenInput& input) override {
        JNIEnv* e = android::env();
        if (!e) return Result::fail("Android 输入注入：JNI 环境不可用");
        jclass cls = android::class_ref(kInputBridge);
        if (!cls) {
            android::release(e);
            return Result::fail("Android 输入注入：桥接类未初始化");
        }
        const jmethodID fn = e->GetStaticMethodID(cls, "inject", "(IIIIII)Z");
        if (!fn) {
            e->ExceptionClear();
            android::release(e);
            return Result::fail("Android 输入注入：缺少 inject");
        }

        int x1 = 0;
        int y1 = 0;
        int x2 = 0;
        int y2 = 0;
        int extra = 0;
        switch (input.type) {
            case ScreenInputType::Tap:
                x1 = input.x1;
                y1 = input.y1;
                break;
            case ScreenInputType::Swipe:
                x1 = input.x1;
                y1 = input.y1;
                x2 = input.x2;
                y2 = input.y2;
                extra = input.duration_ms;
                break;
            case ScreenInputType::Key:
                extra = static_cast<int>(input.key);
                break;
        }
        const jboolean ok =
            e->CallStaticBooleanMethod(cls, fn, type_code(input.type), x1, y1, x2, y2, extra);
        if (e->ExceptionCheck()) {
            e->ExceptionClear();
            android::release(e);
            return Result::fail("Android 输入注入异常");
        }
        android::release(e);
        if (!ok) return Result::fail("无障碍服务未启用或系统拒绝注入");
        return Result::ok();
    }
};

} // namespace

std::shared_ptr<IScreenInput> make_system_screen_input() {
    return std::make_shared<AndroidScreenInput>();
}

} // namespace lb
