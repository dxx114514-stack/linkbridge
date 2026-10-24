// Android 文件监听：由 Kotlin SyncFileObserver（FileObserver 递归注册）回调原生。
// 句柄以 jlong 传入 Kotlin，变化发生时回调 Java_com_linkbridge_app_SyncFileObserver_nativeOnChange。
#include "core/sync/watcher.h"
#include "pal/android/android_jni.h"

#include <jni.h>

#include <memory>
#include <mutex>

namespace lb {
namespace {

constexpr const char* kObserverClass = "com/linkbridge/app/SyncFileObserver";

struct WatcherContext {
    std::mutex mu;
    std::function<void()> on_change;

    // 在 mutex 保护下调用，确保销毁等待在途回调返回
    void fire() {
        std::lock_guard<std::mutex> lk(mu);
        if (on_change) on_change();
    }
};

class AndroidFileWatcher final : public IFileWatcher {
public:
    ~AndroidFileWatcher() override { stop(); }

    Result start(const std::string& root, std::function<void()> on_change) override {
        if (ctx_) return Result::fail("文件监听已启动");
        JNIEnv* e = android::env();
        if (!e) return Result::fail("Android 文件监听：JNI 环境不可用");
        jclass cls = android::class_ref(kObserverClass);
        if (!cls) {
            android::release(e);
            return Result::fail("Android 文件监听：桥接类未初始化");
        }
        const jmethodID fn = e->GetStaticMethodID(cls, "start", "(Ljava/lang/String;J)Z");
        if (!fn) {
            e->ExceptionClear();
            android::release(e);
            return Result::fail("Android 文件监听：缺少 start");
        }

        auto ctx = std::make_unique<WatcherContext>();
        ctx->on_change = std::move(on_change);
        jstring jroot = android::to_jstring(e, root);
        const jboolean ok =
            e->CallStaticBooleanMethod(cls, fn, jroot, static_cast<jlong>(reinterpret_cast<jlong>(ctx.get())));
        e->DeleteLocalRef(jroot);
        if (e->ExceptionCheck()) {
            e->ExceptionClear();
            android::release(e);
            return Result::fail("Android 文件监听启动失败");
        }
        android::release(e);
        if (!ok) return Result::fail("同步文件夹不可监听");

        ctx_ = std::move(ctx);
        return Result::ok();
    }

    void stop() override {
        if (!ctx_) return;
        JNIEnv* e = android::env();
        if (e) {
            jclass cls = android::class_ref(kObserverClass);
            if (cls) {
                const jmethodID fn = e->GetStaticMethodID(cls, "stop", "(J)V");
                if (fn) {
                    e->CallStaticVoidMethod(cls, fn,
                                            static_cast<jlong>(reinterpret_cast<jlong>(ctx_.get())));
                    if (e->ExceptionCheck()) e->ExceptionClear();
                }
            }
            android::release(e);
        }
        // 析构时持有 mutex，等待可能正在执行的回调返回
        ctx_.reset();
    }

private:
    std::unique_ptr<WatcherContext> ctx_;
};

} // namespace

std::shared_ptr<IFileWatcher> make_file_watcher() { return std::make_shared<AndroidFileWatcher>(); }

} // namespace lb

// Kotlin: @JvmStatic external fun nativeOnChange(handle: Long)
extern "C" JNIEXPORT void JNICALL
Java_com_linkbridge_app_SyncFileObserver_nativeOnChange(JNIEnv*, jclass, jlong handle) {
    // WatcherContext 位于匿名命名空间，但仍可通过 lb:: 限定查找
    auto* ctx = reinterpret_cast<lb::WatcherContext*>(handle);
    if (ctx) ctx->fire();
}
