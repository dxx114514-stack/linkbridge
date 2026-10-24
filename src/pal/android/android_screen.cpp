// Android 屏幕采集：通过 JNI 调用 Kotlin ScreenCaptureBridge（MediaProjection + ImageReader）。
// 桥接端输出与 Windows 一致的 32 位 top-down DIB（BITMAPINFOHEADER + 像素），
// 因此核心层的 dib_to_bmp / dib_info 无需平台分支。
#include "core/screen/backend.h"
#include "pal/android/android_jni.h"

#include <cstdint>
#include <cstring>

namespace lb {
namespace {

constexpr const char* kScreenBridge = "com/linkbridge/app/ScreenCaptureBridge";

// 从 BITMAPINFOHEADER 读取宽高；biHeight 为负表示 top-down。
bool dib_dimensions(const ByteBuf& dib, int& width, int& height) {
    if (dib.size() < 40) return false;
    std::int32_t w = 0;
    std::int32_t h = 0;
    std::memcpy(&w, dib.data() + 4, sizeof(w));
    std::memcpy(&h, dib.data() + 8, sizeof(h));
    if (w <= 0 || h == 0) return false;
    width = static_cast<int>(w);
    height = h < 0 ? static_cast<int>(-h) : static_cast<int>(h);
    return true;
}

class AndroidScreenCapture final : public IScreenCapture {
public:
    Expected<ScreenFrame> capture() override {
        JNIEnv* e = android::env();
        if (!e) return Expected<ScreenFrame>::fail("Android 屏幕采集：JNI 环境不可用");
        jclass cls = android::class_ref(kScreenBridge);
        if (!cls) {
            android::release(e);
            return Expected<ScreenFrame>::fail("Android 屏幕采集：桥接类未初始化");
        }
        const jmethodID fn = e->GetStaticMethodID(cls, "captureDib", "()[B");
        if (!fn) {
            e->ExceptionClear();
            android::release(e);
            return Expected<ScreenFrame>::fail("Android 屏幕采集：缺少 captureDib");
        }
        auto bytes = static_cast<jbyteArray>(e->CallStaticObjectMethod(cls, fn));
        if (e->ExceptionCheck()) {
            e->ExceptionClear();
            if (bytes) e->DeleteLocalRef(bytes);
            android::release(e);
            return Expected<ScreenFrame>::fail("Android 屏幕采集失败");
        }
        if (!bytes) {
            android::release(e);
            return Expected<ScreenFrame>::fail("屏幕共享未授权或 MediaProjection 已失效");
        }

        ByteBuf dib = android::from_jbytes(e, bytes);
        e->DeleteLocalRef(bytes);
        android::release(e);

        ScreenFrame frame;
        frame.mime = "image/dib";
        if (!dib_dimensions(dib, frame.width, frame.height))
            return Expected<ScreenFrame>::fail("Android 屏幕采集：DIB 头非法");
        frame.data = std::move(dib);
        return Expected<ScreenFrame>::success(std::move(frame));
    }
};

} // namespace

std::shared_ptr<IScreenCapture> make_system_screen_capture() {
    return std::make_shared<AndroidScreenCapture>();
}

} // namespace lb
