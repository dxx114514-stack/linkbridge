// Android 通知后端：通过 JNI 调用 Kotlin NotifyBridge。
// 展示使用系统通知（镜像带来源标注与“在来源设备打开”动作），
// 动作执行优先 Shizuku/无障碍注入，其次打开来源应用。
#include "core/notify/backend.h"
#include "pal/android/android_jni.h"

namespace lb {
namespace {

constexpr const char* kNotifyBridge = "com/linkbridge/app/NotifyBridge";

class AndroidNotifyBackend final : public INotifyBackend {
public:
    Result show(const NotifyMessage& msg, const std::string& source_device_id,
                const std::string& source_name) override {
        JNIEnv* e = android::env();
        if (!e) return Result::fail("Android 通知桥：JNI 环境不可用");
        jclass cls = android::class_ref(kNotifyBridge);
        if (!cls) {
            android::release(e);
            return Result::fail("Android 通知桥未初始化（请授予通知使用权）");
        }
        const jmethodID fn = e->GetStaticMethodID(
            cls, "showMirror",
            "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;"
            "Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;J)Z");
        if (!fn) {
            e->ExceptionClear();
            android::release(e);
            return Result::fail("Android 通知桥缺少 showMirror");
        }
        jstring j_source_id = android::to_jstring(e, source_device_id);
        jstring j_source_name = android::to_jstring(e, source_name);
        jstring j_id = android::to_jstring(e, msg.id);
        jstring j_app = android::to_jstring(e, msg.app);
        jstring j_app_name = android::to_jstring(e, msg.app_name);
        jstring j_title = android::to_jstring(e, msg.title);
        jstring j_text = android::to_jstring(e, msg.text);
        const jboolean ok = e->CallStaticBooleanMethod(cls, fn, j_source_id, j_source_name, j_id,
                                                       j_app, j_app_name, j_title, j_text,
                                                       static_cast<jlong>(msg.posted_at));
        e->DeleteLocalRef(j_source_id);
        e->DeleteLocalRef(j_source_name);
        e->DeleteLocalRef(j_id);
        e->DeleteLocalRef(j_app);
        e->DeleteLocalRef(j_app_name);
        e->DeleteLocalRef(j_title);
        e->DeleteLocalRef(j_text);
        if (e->ExceptionCheck()) {
            e->ExceptionClear();
            android::release(e);
            return Result::fail("Android 通知桥展示异常");
        }
        android::release(e);
        return ok ? Result::ok() : Result::fail("Android 未能展示镜像通知（检查通知权限）");
    }

    Result act(const NotifyMessage& msg, const NotifyAction& action) override {
        JNIEnv* e = android::env();
        if (!e) return Result::fail("Android 通知桥：JNI 环境不可用");
        jclass cls = android::class_ref(kNotifyBridge);
        if (!cls) {
            android::release(e);
            return Result::fail("Android 通知桥未初始化");
        }
        const jmethodID fn = e->GetStaticMethodID(
            cls, "performAction",
            "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;III)Z");
        if (!fn) {
            e->ExceptionClear();
            android::release(e);
            return Result::fail("Android 通知桥缺少 performAction");
        }
        jstring j_id = android::to_jstring(e, msg.id);
        jstring j_app = android::to_jstring(e, msg.app);
        jstring j_app_name = android::to_jstring(e, msg.app_name);
        jstring j_title = android::to_jstring(e, msg.title);
        const jboolean ok = e->CallStaticBooleanMethod(
            cls, fn, j_id, j_app, j_app_name, j_title, static_cast<jint>(action.mode),
            static_cast<jint>(action.x), static_cast<jint>(action.y));
        e->DeleteLocalRef(j_id);
        e->DeleteLocalRef(j_app);
        e->DeleteLocalRef(j_app_name);
        e->DeleteLocalRef(j_title);
        if (e->ExceptionCheck()) {
            e->ExceptionClear();
            android::release(e);
            return Result::fail("Android 通知动作执行异常");
        }
        android::release(e);
        return ok ? Result::ok() : Result::fail("来源设备未能执行动作（Shizuku/无障碍不可用）");
    }

    bool available() const override {
        JNIEnv* e = android::env();
        if (!e) return false;
        jclass cls = android::class_ref(kNotifyBridge);
        if (!cls) {
            android::release(e);
            return false;
        }
        const jmethodID fn = e->GetStaticMethodID(cls, "isReady", "()Z");
        if (!fn) {
            e->ExceptionClear();
            android::release(e);
            return false;
        }
        const jboolean ok = e->CallStaticBooleanMethod(cls, fn);
        if (e->ExceptionCheck()) e->ExceptionClear();
        android::release(e);
        return ok == JNI_TRUE;
    }

    std::string unavailable_reason() const override {
        JNIEnv* e = android::env();
        if (!e) return "JNI 环境不可用";
        jclass cls = android::class_ref(kNotifyBridge);
        if (!cls) {
            android::release(e);
            return "通知桥未初始化";
        }
        const jmethodID fn = e->GetStaticMethodID(cls, "unavailableReason", "()Ljava/lang/String;");
        if (!fn) {
            e->ExceptionClear();
            android::release(e);
            return "未授予通知使用权";
        }
        jobject out = e->CallStaticObjectMethod(cls, fn);
        if (e->ExceptionCheck()) {
            e->ExceptionClear();
            android::release(e);
            return "未授予通知使用权";
        }
        const std::string reason = out ? android::to_string(e, static_cast<jstring>(out))
                                       : std::string("未授予通知使用权");
        if (out) e->DeleteLocalRef(out);
        android::release(e);
        return reason.empty() ? std::string("未授予通知使用权") : reason;
    }
};

} // namespace

std::shared_ptr<INotifyBackend> make_system_notify_backend() {
    return std::make_shared<AndroidNotifyBackend>();
}

} // namespace lb
