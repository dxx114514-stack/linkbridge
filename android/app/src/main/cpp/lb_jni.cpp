// LinkBridge Android JNI 桥：把核心 Node 暴露给 Kotlin（Compose UI / 前台服务）。
// 事件通过 com/linkbridge/app/NativeBridge.onNativeEvent(String) 以 JSON 回传。
#include <jni.h>

#include <android/log.h>

#include "core/node/node.h"
#include "core/platform/platform.h"
#include "pal/android/android_jni.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr const char* kTag = "LinkBridge";
constexpr const char* kBridgeClass = "com/linkbridge/app/NativeBridge";
constexpr int kConfirmTimeoutMs = 60000;

std::mutex g_mu;
std::mutex g_start_mu;
std::atomic<bool> g_stopping{false};
std::shared_ptr<lb::Node> g_node;
std::vector<lb::Subscription> g_subs;

// ---- 事件 JSON ----

std::string escape_json(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

struct Json {
    std::string s = "{";
    bool first = true;

    Json& sep() {
        if (!first) s += ",";
        first = false;
        return *this;
    }
    Json& str(const std::string& k, const std::string& v) {
        sep();
        s += "\"" + k + "\":\"" + escape_json(v) + "\"";
        return *this;
    }
    Json& num(const std::string& k, long long v) {
        sep();
        s += "\"" + k + "\":" + std::to_string(v);
        return *this;
    }
    Json& boolean(const std::string& k, bool v) {
        sep();
        s += "\"" + k + "\":" + (v ? "true" : "false");
        return *this;
    }
    Json& raw(const std::string& k, const std::string& v) {
        sep();
        s += "\"" + k + "\":" + v;
        return *this;
    }
    std::string done() { return s + "}"; }
};

void emit(const std::string& json) {
    JNIEnv* e = lb::android::env();
    if (!e) return;
    jclass cls = lb::android::class_ref(kBridgeClass);
    if (cls) {
        const jmethodID fn = e->GetStaticMethodID(cls, "onNativeEvent", "(Ljava/lang/String;)V");
        if (fn) {
            jstring payload = lb::android::to_jstring(e, json);
            e->CallStaticVoidMethod(cls, fn, payload);
            e->DeleteLocalRef(payload);
            if (e->ExceptionCheck()) e->ExceptionClear();
        } else {
            e->ExceptionClear();
        }
    }
    lb::android::release(e);
}

// ---- 确认请求（配对/剪贴板/屏幕）：阻塞后端线程，等待 UI 调用 nativeRespondConfirm ----

struct ConfirmSlot {
    std::mutex mu;
    std::condition_variable cv;
    bool done = false;
    bool accept = false;
};

std::mutex g_confirm_mu;
std::map<int, std::shared_ptr<ConfirmSlot>> g_pending;
int g_next_confirm = 1;

// 「总是允许」策略：按 kind 记录，命中后 request_confirm 直接放行，不再询问用户。
// 状态保存在进程内，由 Kotlin 侧在启动时从 SharedPreferences 回填。
std::mutex g_allow_mu;
std::map<std::string, bool> g_auto_allow;

bool auto_allowed(const std::string& kind) {
    std::lock_guard<std::mutex> lk(g_allow_mu);
    const auto it = g_auto_allow.find(kind);
    return it != g_auto_allow.end() && it->second;
}

bool request_confirm(const std::string& kind, const std::string& name, const std::string& detail,
                     const std::string& fingerprint) {
    if (g_stopping.load()) return false;
    if (auto_allowed(kind)) {
        __android_log_print(ANDROID_LOG_INFO, kTag, "confirm auto-allowed: kind=%s peer=%s",
                            kind.c_str(), name.c_str());
        return true;
    }
    auto slot = std::make_shared<ConfirmSlot>();
    int id = 0;
    {
        std::lock_guard<std::mutex> lk(g_confirm_mu);
        id = g_next_confirm++;
        g_pending[id] = slot;
    }
    Json j;
    j.str("type", "confirm_request")
        .num("id", id)
        .str("kind", kind)
        .str("name", name)
        .str("detail", detail)
        .str("fingerprint", fingerprint);
    emit(j.done());

    std::unique_lock<std::mutex> lk(slot->mu);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kConfirmTimeoutMs);
    while (!slot->done && !g_stopping.load()) {
        if (slot->cv.wait_until(lk, deadline) == std::cv_status::timeout) break;
    }
    const bool accepted = slot->done && slot->accept;
    {
        std::lock_guard<std::mutex> lk2(g_confirm_mu);
        g_pending.erase(id);
    }
    return accepted;
}

void respond_confirm(int id, bool accept) {
    std::shared_ptr<ConfirmSlot> slot;
    {
        std::lock_guard<std::mutex> lk(g_confirm_mu);
        const auto it = g_pending.find(id);
        if (it == g_pending.end()) return;
        slot = it->second;
    }
    std::lock_guard<std::mutex> lk(slot->mu);
    slot->done = true;
    slot->accept = accept;
    slot->cv.notify_all();
}

// 停止服务时唤醒所有等待中的确认请求（一律视为拒绝），避免阻塞监听线程退出
void cancel_all_confirms() {
    std::vector<std::shared_ptr<ConfirmSlot>> slots;
    {
        std::lock_guard<std::mutex> lk(g_confirm_mu);
        for (auto& kv : g_pending) slots.push_back(kv.second);
    }
    for (auto& slot : slots) {
        std::lock_guard<std::mutex> lk(slot->mu);
        slot->done = true;
        slot->accept = false;
        slot->cv.notify_all();
    }
}

// ---- 实时屏幕流：后台线程持续拉帧，逐帧回传 Kotlin 渲染 ----

struct ScreenStreamSession {
    std::atomic<bool> stop{false};
};

std::mutex g_stream_mu;
std::shared_ptr<ScreenStreamSession> g_stream;

std::uint16_t dib_u16(const lb::ByteBuf& b, std::size_t off) {
    return static_cast<std::uint16_t>(b[off] | (b[off + 1] << 8));
}

std::uint32_t dib_u32(const lb::ByteBuf& b, std::size_t off) {
    return static_cast<std::uint32_t>(b[off]) | (static_cast<std::uint32_t>(b[off + 1]) << 8) |
           (static_cast<std::uint32_t>(b[off + 2]) << 16) |
           (static_cast<std::uint32_t>(b[off + 3]) << 24);
}

// DIB（BITMAPINFOHEADER 起始）→ ARGB int[]，按 2x 降采样控制 JNI 拷贝量
void deliver_screen_frame(JNIEnv* e, std::uint64_t seq, int width, int height,
                          const lb::ByteBuf& dib) {
    constexpr std::size_t kHeader = 40;
    if (width <= 0 || height <= 0 || dib.size() < kHeader) return;
    const std::uint32_t header_size = dib_u32(dib, 0);
    if (header_size < kHeader || dib.size() < header_size) return;
    const std::int32_t raw_height = static_cast<std::int32_t>(dib_u32(dib, 8));
    const std::uint16_t bpp = dib_u16(dib, 14);
    if (bpp != 24 && bpp != 32) return;
    const bool top_down = raw_height < 0;
    const std::size_t row_size =
        ((static_cast<std::size_t>(width) * bpp + 31) / 32) * 4;
    if (header_size + row_size * static_cast<std::size_t>(height) > dib.size()) return;

    const int out_w = std::max(1, width / 2);
    const int out_h = std::max(1, height / 2);
    std::vector<jint> pixels(static_cast<std::size_t>(out_w) * out_h);
    const std::size_t step = bpp / 8;
    for (int y = 0; y < out_h; ++y) {
        const int src_y = std::min(height - 1, y * 2);
        const std::size_t row = top_down ? static_cast<std::size_t>(src_y)
                                         : static_cast<std::size_t>(height - 1 - src_y);
        const std::size_t base = header_size + row * row_size;
        for (int x = 0; x < out_w; ++x) {
            const std::size_t p = base + static_cast<std::size_t>(x * 2) * step;
            const jint b = dib[p];
            const jint g = dib[p + 1];
            const jint r = dib[p + 2];
            pixels[static_cast<std::size_t>(y) * out_w + x] =
                static_cast<jint>(0xFF000000u) | (r << 16) | (g << 8) | b;
        }
    }

    jclass cls = lb::android::class_ref(kBridgeClass);
    if (!cls) return;
    const jmethodID fn = e->GetStaticMethodID(cls, "onScreenFrame", "([IIIJ)V");
    if (!fn) {
        e->ExceptionClear();
        return;
    }
    jintArray arr = e->NewIntArray(static_cast<jsize>(pixels.size()));
    if (arr) {
        e->SetIntArrayRegion(arr, 0, static_cast<jsize>(pixels.size()), pixels.data());
        e->CallStaticVoidMethod(cls, fn, arr, out_w, out_h, static_cast<jlong>(seq));
        e->DeleteLocalRef(arr);
        if (e->ExceptionCheck()) e->ExceptionClear();
    }
}

// ---- Node 生命周期 ----

std::shared_ptr<lb::Node> snapshot() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_node;
}

std::string ensure_config() {
    const std::string path = lb::pal::default_config_path();
    bool created = false;
    auto cfg = lb::load_config(path, &created);
    if (!cfg) return "配置加载失败: " + cfg.error;

    if (created) {
        // Android 首启：默认把同步目录放到应用私有存储，并启用监听同步。
        const std::string folder = lb::pal::join_path(lb::pal::data_dir(), "sync");
        if (auto r = lb::pal::ensure_dir(folder); !r) return "创建同步目录失败: " + r.error;
        std::string text = lb::default_config_text();
        const std::string from = "enabled = false\nfolder = \"\"";
        const std::string to = "enabled = true\nfolder = \"" + folder + "\"";
        const auto pos = text.find(from);
        if (pos != std::string::npos) text.replace(pos, from.size(), to);
        const lb::ByteBuf data(text.begin(), text.end());
        if (auto w = lb::pal::write_file_atomic(path, data); !w)
            return "写入默认配置失败: " + w.error;
    }
    return {};
}

void attach_event_forwarding(lb::Node& node) {
    using namespace lb;
    auto& ev = node.events();
    std::vector<Subscription> subs;

    subs.push_back(ev.subscribe<PairingCompleted>([](const PairingCompleted& e) {
        Json j;
        j.str("type", "pairing_completed")
            .str("device_id", e.peer.device_id)
            .str("name", e.peer.name)
            .str("platform", e.peer.platform)
            .str("fingerprint", e.fingerprint);
        emit(j.done());
    }));
    subs.push_back(ev.subscribe<PairingStateChanged>([](const PairingStateChanged& e) {
        if (e.state == PairState::Failed) {
            Json j;
            j.str("type", "pairing_failed").str("message", e.message);
            emit(j.done());
        }
    }));
    subs.push_back(ev.subscribe<DeviceOnline>([](const DeviceOnline& e) {
        Json j;
        j.str("type", "device_online").str("device_id", e.device_id).str("name", e.name);
        emit(j.done());
    }));
    subs.push_back(ev.subscribe<DeviceOffline>([](const DeviceOffline& e) {
        Json j;
        j.str("type", "device_offline").str("device_id", e.device_id).str("name", e.name);
        emit(j.done());
    }));
    subs.push_back(ev.subscribe<TransferProgress>([](const TransferProgress& e) {
        Json j;
        j.str("type", "transfer_progress")
            .str("task_id", e.task_id)
            .str("file_name", e.file_name)
            .num("transferred", static_cast<long long>(e.transferred))
            .num("total", static_cast<long long>(e.total))
            .boolean("outgoing", e.outgoing);
        emit(j.done());
    }));
    subs.push_back(ev.subscribe<TransferCompleted>([](const TransferCompleted& e) {
        Json j;
        j.str("type", "transfer_completed")
            .str("task_id", e.task_id)
            .str("file_name", e.file_name)
            .str("path", e.path)
            .num("size", static_cast<long long>(e.size))
            .boolean("outgoing", e.outgoing);
        emit(j.done());
    }));
    subs.push_back(ev.subscribe<TransferFailed>([](const TransferFailed& e) {
        Json j;
        j.str("type", "transfer_failed")
            .str("task_id", e.task_id)
            .str("file_name", e.file_name)
            .str("reason", e.reason)
            .boolean("outgoing", e.outgoing);
        emit(j.done());
    }));
    subs.push_back(ev.subscribe<ClipboardReceived>([](const ClipboardReceived& e) {
        Json j;
        j.str("type", "clipboard_received")
            .str("from", e.from_name)
            .str("mime", e.mime)
            .num("size", static_cast<long long>(e.size))
            .boolean("applied", e.applied)
            .str("reason", e.reason);
        emit(j.done());
    }));
    subs.push_back(ev.subscribe<ClipboardSent>([](const ClipboardSent& e) {
        Json j;
        j.str("type", "clipboard_sent")
            .str("to", e.to_name)
            .str("mime", e.mime)
            .num("size", static_cast<long long>(e.size));
        emit(j.done());
    }));
    subs.push_back(ev.subscribe<ClipboardFailed>([](const ClipboardFailed& e) {
        Json j;
        j.str("type", "clipboard_failed").str("peer", e.peer_name).str("reason", e.reason);
        emit(j.done());
    }));
    subs.push_back(ev.subscribe<ClipboardSkipped>([](const ClipboardSkipped& e) {
        Json j;
        j.str("type", "clipboard_skipped")
            .str("mime", e.mime)
            .num("size", static_cast<long long>(e.size))
            .str("reason", e.reason);
        emit(j.done());
    }));
    subs.push_back(ev.subscribe<ScreenSessionStarted>([](const ScreenSessionStarted& e) {
        Json j;
        j.str("type", "screen_session_started").str("peer", e.peer_name);
        emit(j.done());
    }));
    subs.push_back(ev.subscribe<ScreenSessionEnded>([](const ScreenSessionEnded& e) {
        Json j;
        j.str("type", "screen_session_ended")
            .str("peer", e.peer_name)
            .str("reason", e.reason)
            .num("frames", static_cast<long long>(e.frames))
            .num("inputs", static_cast<long long>(e.inputs));
        emit(j.done());
    }));
    subs.push_back(ev.subscribe<ScreenFailed>([](const ScreenFailed& e) {
        Json j;
        j.str("type", "screen_failed").str("peer", e.peer_name).str("reason", e.reason);
        emit(j.done());
    }));
    subs.push_back(ev.subscribe<SyncStarted>([](const SyncStarted& e) {
        Json j;
        j.str("type", "sync_started").str("peer", e.peer).str("peer_name", e.peer_name);
        emit(j.done());
    }));
    subs.push_back(ev.subscribe<SyncCompleted>([](const SyncCompleted& e) {
        Json j;
        j.str("type", "sync_completed")
            .str("peer", e.peer)
            .str("peer_name", e.peer_name)
            .raw("stats", "{\"pushed\":" + std::to_string(e.stats.pushed) +
                              ",\"pulled\":" + std::to_string(e.stats.pulled) +
                              ",\"deleted\":" + std::to_string(e.stats.deleted) +
                              ",\"conflicts\":" + std::to_string(e.stats.conflicts) +
                              ",\"bytes\":" + std::to_string(e.stats.bytes) +
                              ",\"skipped\":" + std::to_string(e.stats.skipped) + "}");
        emit(j.done());
    }));
    subs.push_back(ev.subscribe<SyncFailed>([](const SyncFailed& e) {
        Json j;
        j.str("type", "sync_failed")
            .str("peer", e.peer)
            .str("peer_name", e.peer_name)
            .str("reason", e.reason);
        emit(j.done());
    }));
    subs.push_back(ev.subscribe<NotifyMirrored>([](const NotifyMirrored& e) {
        Json j;
        j.str("type", "notify_mirrored")
            .str("from", e.from)
            .str("from_name", e.from_name)
            .str("app", e.app)
            .str("app_name", e.app_name)
            .str("title", e.title)
            .boolean("applied", e.applied)
            .str("reason", e.reason);
        emit(j.done());
    }));
    subs.push_back(ev.subscribe<NotifyMirrorSent>([](const NotifyMirrorSent& e) {
        Json j;
        j.str("type", "notify_mirror_sent")
            .str("to", e.to)
            .str("to_name", e.to_name)
            .str("app", e.app)
            .str("title", e.title);
        emit(j.done());
    }));
    subs.push_back(ev.subscribe<NotifyFailed>([](const NotifyFailed& e) {
        Json j;
        j.str("type", "notify_failed")
            .str("peer", e.peer)
            .str("peer_name", e.peer_name)
            .str("reason", e.reason);
        emit(j.done());
    }));
    subs.push_back(ev.subscribe<NotifyActionReceived>([](const NotifyActionReceived& e) {
        Json j;
        j.str("type", "notify_action_received")
            .str("from", e.from)
            .str("from_name", e.from_name)
            .str("notification_id", e.notification_id)
            .str("mode", e.mode)
            .boolean("executed", e.executed)
            .str("reason", e.reason);
        emit(j.done());
    }));
    subs.push_back(ev.subscribe<NotifyActionDone>([](const NotifyActionDone& e) {
        Json j;
        j.str("type", "notify_action_done")
            .str("peer", e.peer)
            .str("notification_id", e.notification_id)
            .boolean("ok", e.ok)
            .str("reason", e.reason);
        emit(j.done());
    }));

    std::lock_guard<std::mutex> lk(g_mu);
    g_subs = std::move(subs);
}

std::string start_engine(const std::string& device_name) {
    std::lock_guard<std::mutex> start_lk(g_start_mu);
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_node && g_node->running()) return {};
    }
    if (lb::pal::data_dir() == ".") return "应用目录尚未注入（请先调用 setPaths）";
    if (auto err = ensure_config(); !err.empty()) return err;

    auto cfg = lb::load_config(lb::pal::default_config_path());
    if (!cfg) return "配置加载失败: " + cfg.error;

    lb::NodeOptions options;
    options.data_dir = lb::pal::data_dir();
    options.config = *cfg;
    options.device_name = device_name.empty() ? cfg->device_name : device_name;

    auto created = lb::Node::create(options);
    if (!created) {
        __android_log_print(ANDROID_LOG_ERROR, "lb_jni", "Node::create failed: %s",
                            created.error.c_str());
        return "初始化失败: " + created.error;
    }
    std::shared_ptr<lb::Node> node = std::move(*created);

    // 确认回调：交给 UI 决定（超时视为拒绝）
    node->pairing().set_confirm_handler(
        [](const lb::PeerInfo& peer, const std::string& fingerprint) {
            return request_confirm("pairing", peer.name, peer.device_id + " / " + peer.platform,
                                   fingerprint);
        });
    node->clipboard().set_confirm_handler(
        [](const lb::ClipboardContent& content, const std::string& from) {
            const std::string preview =
                content.mime == "text/plain"
                    ? std::string(content.data.begin(),
                                  content.data.begin() +
                                      static_cast<std::ptrdiff_t>(std::min<size_t>(content.data.size(), 200)))
                    : std::string();
            return request_confirm("clipboard", from, preview, content.mime);
        });
    node->screen().set_confirm_handler([](const std::string& from, const std::string& mode) {
        return request_confirm("screen", from, mode, "");
    });
    node->notify().set_action_confirm_handler(
        [](const std::string& from, const lb::NotifyMessage& msg) {
            return request_confirm("notify_action", from,
                                   msg.app_name + "：" + msg.title + "\n" + msg.text, "");
        });
    node->transfer().set_accept_handler([](const std::string& file_name, std::uint64_t size) {
        char detail[96];
        std::snprintf(detail, sizeof(detail), "文件 %.1f MB",
                      static_cast<double>(size) / (1024.0 * 1024.0));
        return request_confirm("transfer", file_name, detail, "");
    });

    if (auto r = node->start(); !r) {
        __android_log_print(ANDROID_LOG_ERROR, "lb_jni", "node->start failed: %s",
                            r.error.c_str());
        return "启动服务失败: " + r.error;
    }
    __android_log_print(ANDROID_LOG_INFO, "lb_jni", "engine started, device=%s",
                        node->identity().device_id().c_str());

    attach_event_forwarding(*node);
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_stopping.store(false);
        g_node = std::move(node);
    }
    return {};
}

void stop_engine() {
    std::lock_guard<std::mutex> start_lk(g_start_mu);
    g_stopping.store(true);
    cancel_all_confirms();
    std::shared_ptr<lb::Node> node;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        node = std::move(g_node);
        g_subs.clear(); // 先退订再销毁 Node，避免悬空的事件总线
    }
    if (node) node->stop();
    g_stopping.store(false);
    // node 离开作用域后析构，Node::~Node 会再次 stop（幂等）
}

std::optional<lb::TrustRecord> resolve_peer(const std::shared_ptr<lb::Node>& node,
                                            const std::string& key) {
    if (!node) return std::nullopt;
    auto lower = [](std::string s) {
        for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    const std::string want = lower(key);
    for (const auto& r : node->trust().all())
        if (lower(r.device_id) == want || lower(r.name) == want) return r;
    return std::nullopt;
}

} // namespace

extern "C" {

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*) {
    lb::android::init(vm);
    JNIEnv* e = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&e), JNI_VERSION_1_6) == JNI_OK) {
        if (!lb::android::ensure_classes(e))
            __android_log_print(ANDROID_LOG_WARN, kTag, "some JNI bridge classes missing");
    }
    return JNI_VERSION_1_6;
}

JNIEXPORT void JNICALL JNI_OnUnload(JavaVM* vm, void*) {
    stop_engine();
    JNIEnv* e = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&e), JNI_VERSION_1_6) == JNI_OK)
        lb::android::shutdown(e);
}

JNIEXPORT void JNICALL
Java_com_linkbridge_app_NativeBridge_nativeSetPaths(JNIEnv* e, jclass, jstring files, jstring cache) {
    const std::string files_dir = lb::android::to_string(e, files);
    const std::string cache_dir = lb::android::to_string(e, cache);
    lb::pal::android_set_paths(files_dir, cache_dir);
}

JNIEXPORT jstring JNICALL
Java_com_linkbridge_app_NativeBridge_nativeStart(JNIEnv* e, jclass, jstring device_name) {
    const std::string err = start_engine(lb::android::to_string(e, device_name));
    return lb::android::to_jstring(e, err);
}

JNIEXPORT void JNICALL Java_com_linkbridge_app_NativeBridge_nativeStop(JNIEnv*, jclass) {
    {
        // 先结束屏幕流，避免后台线程继续使用即将销毁的 Node
        std::lock_guard<std::mutex> lk(g_stream_mu);
        if (g_stream) {
            g_stream->stop.store(true);
            g_stream.reset();
        }
    }
    stop_engine();
}

JNIEXPORT jboolean JNICALL Java_com_linkbridge_app_NativeBridge_nativeIsRunning(JNIEnv*, jclass) {
    const auto node = snapshot();
    return node && node->running() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jstring JNICALL Java_com_linkbridge_app_NativeBridge_nativeIdentityJson(JNIEnv* e, jclass) {
    const auto node = snapshot();
    if (!node) return lb::android::to_jstring(e, R"({"error":"服务未启动"})");
    Json j;
    j.str("name", node->identity().name())
        .str("device_id", node->identity().device_id())
        .str("fingerprint", node->identity().fingerprint())
        .str("platform", lb::platform_name(node->identity().platform()))
        .num("pair_port", node->pair_port())
        .num("heartbeat_port", node->heartbeat_port())
        .num("transfer_port", node->transfer_port())
        .num("clipboard_port", node->clipboard_port())
        .num("screen_port", node->screen_port())
        .num("sync_port", node->sync_port());
    return lb::android::to_jstring(e, j.done());
}

JNIEXPORT jstring JNICALL Java_com_linkbridge_app_NativeBridge_nativeDevicesJson(JNIEnv* e, jclass) {
    const auto node = snapshot();
    if (!node) return lb::android::to_jstring(e, "[]");
    std::string out = "[";
    bool first = true;
    for (const auto& d : node->devices()) {
        if (!first) out += ",";
        first = false;
        const char* presence = d.presence == lb::Presence::Online
                                   ? "online"
                                   : (d.presence == lb::Presence::Offline ? "offline" : "unknown");
        Json j;
        j.str("device_id", d.device_id)
            .str("name", d.name)
            .str("platform", d.platform)
            .str("presence", presence)
            .str("ip", d.last_ip)
            .num("last_seen", static_cast<long long>(d.last_seen))
            .num("paired_at", static_cast<long long>(d.paired_at));
        if (const auto rec = node->trust().get(d.device_id))
            j.num("permissions", static_cast<long long>(rec->permissions))
                .str("permissions_text", lb::permission_mask_text(rec->permissions));
        else
            j.num("permissions", 0).str("permissions_text", "-");
        out += j.done();
    }
    out += "]";
    return lb::android::to_jstring(e, out);
}

JNIEXPORT jstring JNICALL
Java_com_linkbridge_app_NativeBridge_nativeRequestPairCode(JNIEnv* e, jclass, jint ttl_sec) {
    const auto node = snapshot();
    if (!node || !node->running()) return lb::android::to_jstring(e, R"({"error":"服务未启动"})");
    const lb::PairCode code = node->pairing().generate_code(ttl_sec <= 0 ? 300 : ttl_sec, 3);
    Json j;
    j.str("code", code.value).num("expires_at", static_cast<long long>(code.expires_at));
    return lb::android::to_jstring(e, j.done());
}

JNIEXPORT jstring JNICALL Java_com_linkbridge_app_NativeBridge_nativePair(
    JNIEnv* e, jclass, jstring ip, jstring code) {
    const auto node = snapshot();
    if (!node || !node->running()) return lb::android::to_jstring(e, "服务未启动");
    const lb::Result r = node->pairing().pair_with(lb::android::to_string(e, ip),
                                                   node->config().network.pair_port,
                                                   lb::android::to_string(e, code));
    return lb::android::to_jstring(e, r ? "" : r.error);
}

JNIEXPORT jstring JNICALL Java_com_linkbridge_app_NativeBridge_nativeSendFile(
    JNIEnv* e, jclass, jstring peer, jstring path, jstring remote_name) {
    const auto node = snapshot();
    if (!node || !node->running()) return lb::android::to_jstring(e, "服务未启动");
    const auto record = resolve_peer(node, lb::android::to_string(e, peer));
    if (!record) return lb::android::to_jstring(e, "未找到受信任设备");
    const lb::Result r =
        node->transfer().send_file(record->device_id, lb::android::to_string(e, path),
                                   lb::android::to_string(e, remote_name),
                                   node->config().network.connect_timeout_ms);
    return lb::android::to_jstring(e, r ? "" : r.error);
}

JNIEXPORT jstring JNICALL
Java_com_linkbridge_app_NativeBridge_nativeSyncNow(JNIEnv* e, jclass, jstring peer) {
    const auto node = snapshot();
    if (!node || !node->running()) return lb::android::to_jstring(e, "服务未启动");
    if (!node->config().sync.enabled)
        return lb::android::to_jstring(e, "文件夹同步未启用（请检查 linkbridge.toml）");
    const auto record = resolve_peer(node, lb::android::to_string(e, peer));
    if (!record) return lb::android::to_jstring(e, "未找到受信任设备");
    if (record->last_ip.empty()) return lb::android::to_jstring(e, "尚未学习到对端 IP，请稍候");

    lb::SyncStats stats;
    const lb::Result r =
        node->sync().sync_with(record->device_id, node->config().network.connect_timeout_ms, &stats);
    if (!r) return lb::android::to_jstring(e, r.error);
    Json j;
    j.num("pushed", static_cast<long long>(stats.pushed))
        .num("pulled", static_cast<long long>(stats.pulled))
        .num("deleted", static_cast<long long>(stats.deleted))
        .num("conflicts", static_cast<long long>(stats.conflicts))
        .num("bytes", static_cast<long long>(stats.bytes))
        .num("skipped", static_cast<long long>(stats.skipped));
    return lb::android::to_jstring(e, j.done());
}

// 启动实时屏幕流：后台线程持续拉帧，经 onScreenFrame 回传；seconds 为会话上限
JNIEXPORT jstring JNICALL Java_com_linkbridge_app_NativeBridge_nativeStartScreenStream(
    JNIEnv* e, jclass, jstring peer, jint seconds) {
    const auto node = snapshot();
    if (!node || !node->running()) return lb::android::to_jstring(e, "服务未启动");
    const auto record = resolve_peer(node, lb::android::to_string(e, peer));
    if (!record) return lb::android::to_jstring(e, "未找到受信任设备");
    if (record->last_ip.empty()) return lb::android::to_jstring(e, "尚未学习到对端 IP，请稍候");

    auto session = std::make_shared<ScreenStreamSession>();
    {
        std::lock_guard<std::mutex> lk(g_stream_mu);
        if (g_stream) g_stream->stop.store(true);
        g_stream = session;
    }
    const int ms = std::clamp(static_cast<int>(seconds), 5, 3600) * 1000;
    const std::string device = record->device_id;
    std::thread([node, session, device, ms]() {
        JNIEnv* env = lb::android::env();
        const auto r = node->screen().view(
            device, std::string(), ms, nullptr,
            [env, session](std::uint64_t seq, int w, int h, const lb::ByteBuf& dib) {
                {
                    std::lock_guard<std::mutex> lk(g_stream_mu);
                    if (g_stream.get() != session.get()) {
                        session->stop.store(true);
                        return;
                    }
                }
                if (env) deliver_screen_frame(env, seq, w, h, dib);
            },
            &session->stop);
        if (env) lb::android::release(env);

        Json j;
        j.str("type", "screen_stream_ended")
            .num("frames", r ? static_cast<long long>(r->frames) : 0)
            .str("reason", r ? std::string() : r.error);
        emit(j.done());
    }).detach();
    return lb::android::to_jstring(e, "");
}

JNIEXPORT void JNICALL
Java_com_linkbridge_app_NativeBridge_nativeStopScreenStream(JNIEnv*, jclass) {
    std::shared_ptr<ScreenStreamSession> session;
    {
        std::lock_guard<std::mutex> lk(g_stream_mu);
        session = g_stream;
        g_stream.reset();
    }
    if (session) session->stop.store(true);
}

JNIEXPORT jstring JNICALL Java_com_linkbridge_app_NativeBridge_nativeStatusJson(JNIEnv* e, jclass) {
    const auto node = snapshot();
    if (!node) return lb::android::to_jstring(e, R"({"running":false})");
    const auto& cfg = node->config();
    Json j;
    j.boolean("running", node->running())
        .str("name", node->identity().name())
        .str("device_id", node->identity().device_id())
        .str("fingerprint", node->identity().fingerprint())
        .num("pair_port", node->pair_port())
        .num("heartbeat_port", node->heartbeat_port())
        .num("transfer_port", node->transfer_port())
        .num("clipboard_port", node->clipboard_port())
        .num("screen_port", node->screen_port())
        .num("sync_port", node->sync_port())
        .num("notify_port", node->notify_port())
        .num("heartbeat_ok", static_cast<long long>(node->heartbeat().probes_ok()))
        .num("heartbeat_failed", static_cast<long long>(node->heartbeat().probes_failed()))
        .num("heartbeat_interval", node->heartbeat().current_interval_sec())
        .raw("sync", "{\"enabled\":" + std::string(cfg.sync.enabled ? "true" : "false") +
                         ",\"folder\":\"" + escape_json(cfg.sync.folder) +
                         "\",\"mode\":\"" + escape_json(cfg.sync.mode) +
                         "\",\"index\":" + std::to_string(node->sync().index_size()) +
                         ",\"conflicts\":" + std::to_string(node->sync().conflicts().size()) + "}")
        .raw("clipboard", "{\"enabled\":" +
                              std::string(cfg.clipboard.enabled ? "true" : "false") +
                              ",\"mode\":\"" + escape_json(cfg.clipboard.mode) + "\"}")
        .raw("screen", "{\"enabled\":" + std::string(cfg.screen.enabled ? "true" : "false") +
                           ",\"max_fps\":" + std::to_string(cfg.screen.max_fps) + "}")
        .raw("notify", "{\"enabled\":" + std::string(cfg.notify.enabled ? "true" : "false") +
                           ",\"runtime_enabled\":" +
                           std::string(node->notify().enabled() ? "true" : "false") +
                           ",\"dedupe_window_sec\":" + std::to_string(cfg.notify.dedupe_window_sec) +
                           ",\"action_mode\":\"" + escape_json(cfg.notify.action_mode) +
                           "\",\"auto_action\":" +
                           std::string(cfg.notify.auto_action ? "true" : "false") + "}");
    return lb::android::to_jstring(e, j.done());
}

JNIEXPORT jstring JNICALL Java_com_linkbridge_app_NativeBridge_nativeConflictsJson(JNIEnv* e, jclass) {
    const auto node = snapshot();
    if (!node) return lb::android::to_jstring(e, "[]");
    std::string out = "[";
    bool first = true;
    for (const auto& c : node->sync().conflicts()) {
        if (!first) out += ",";
        first = false;
        std::string copies = "[";
        for (std::size_t i = 0; i < c.copies.size(); ++i) {
            if (i) copies += ",";
            copies += "\"" + escape_json(c.copies[i]) + "\"";
        }
        copies += "]";
        Json j;
        j.str("path", c.path).raw("copies", copies);
        out += j.done();
    }
    out += "]";
    return lb::android::to_jstring(e, out);
}

JNIEXPORT jstring JNICALL Java_com_linkbridge_app_NativeBridge_nativeResolveConflict(
    JNIEnv* e, jclass, jstring path, jstring action) {
    const auto node = snapshot();
    if (!node) return lb::android::to_jstring(e, "服务未启动");
    const lb::Result r = node->sync().resolve_conflict(lb::android::to_string(e, path),
                                                       lb::android::to_string(e, action));
    return lb::android::to_jstring(e, r ? "" : r.error);
}

JNIEXPORT jstring JNICALL
Java_com_linkbridge_app_NativeBridge_nativeHistoryJson(JNIEnv* e, jclass, jstring path) {
    const auto node = snapshot();
    if (!node) return lb::android::to_jstring(e, "[]");
    std::string out = "[";
    bool first = true;
    for (const auto& v : node->sync().history(lb::android::to_string(e, path))) {
        if (!first) out += ",";
        first = false;
        Json j;
        j.str("id", v.id)
            .str("path", v.path)
            .num("size", static_cast<long long>(v.size))
            .num("archived_at", static_cast<long long>(v.archived_at));
        out += j.done();
    }
    out += "]";
    return lb::android::to_jstring(e, out);
}

JNIEXPORT jstring JNICALL Java_com_linkbridge_app_NativeBridge_nativeRestoreVersion(
    JNIEnv* e, jclass, jstring path, jstring version) {
    const auto node = snapshot();
    if (!node) return lb::android::to_jstring(e, "服务未启动");
    const lb::Result r = node->sync().restore_version(lb::android::to_string(e, path),
                                                      lb::android::to_string(e, version));
    return lb::android::to_jstring(e, r ? "" : r.error);
}

JNIEXPORT jstring JNICALL
Java_com_linkbridge_app_NativeBridge_nativePushClipboard(JNIEnv* e, jclass, jstring text) {
    const auto node = snapshot();
    if (!node || !node->running()) return lb::android::to_jstring(e, "服务未启动");
    const std::string value = lb::android::to_string(e, text);
    lb::ClipboardContent content{"text/plain", lb::ByteBuf(value.begin(), value.end())};
    const lb::Result r =
        node->clipboard().push_to_all(content, node->config().network.connect_timeout_ms);
    return lb::android::to_jstring(e, r ? "" : r.error);
}

JNIEXPORT jstring JNICALL Java_com_linkbridge_app_NativeBridge_nativeNotifyPublish(
    JNIEnv* e, jclass, jstring id, jstring app, jstring app_name, jstring title, jstring text,
    jlong posted_at) {
    const auto node = snapshot();
    if (!node || !node->running()) return lb::android::to_jstring(e, "服务未启动");
    if (!node->notify().enabled()) return lb::android::to_jstring(e, "通知镜像已停用");
    lb::NotifyMessage msg;
    msg.id = lb::android::to_string(e, id);
    msg.app = lb::android::to_string(e, app);
    msg.app_name = lb::android::to_string(e, app_name);
    msg.title = lb::android::to_string(e, title);
    msg.text = lb::android::to_string(e, text);
    msg.posted_at = static_cast<std::int64_t>(posted_at);
    const lb::Result r = node->notify().publish(msg, node->config().network.connect_timeout_ms);
    return lb::android::to_jstring(e, r ? "" : r.error);
}

JNIEXPORT jstring JNICALL Java_com_linkbridge_app_NativeBridge_nativeNotifyAction(
    JNIEnv* e, jclass, jstring source, jstring id, jstring app, jstring app_name, jstring title) {
    const auto node = snapshot();
    if (!node || !node->running()) return lb::android::to_jstring(e, "服务未启动");
    const auto record = resolve_peer(node, lb::android::to_string(e, source));
    if (!record) return lb::android::to_jstring(e, "未找到来源设备");
    const lb::Result r = node->notify().request_action(
        record->device_id, lb::android::to_string(e, id), lb::android::to_string(e, app),
        lb::android::to_string(e, app_name), lb::android::to_string(e, title),
        node->config().network.connect_timeout_ms);
    return lb::android::to_jstring(e, r ? "" : r.error);
}

JNIEXPORT jstring JNICALL
Java_com_linkbridge_app_NativeBridge_nativePermissionCatalogJson(JNIEnv* e, jclass) {
    std::string out = "[";
    bool first = true;
    for (const auto& p : lb::permission_catalog()) {
        if (!first) out += ",";
        first = false;
        Json j;
        j.str("key", p.key)
            .str("name", p.name)
            .str("description", p.description)
            .num("flag", static_cast<long long>(p.flag))
            .boolean("default_granted", p.default_granted);
        out += j.done();
    }
    out += "]";
    return lb::android::to_jstring(e, out);
}

JNIEXPORT jstring JNICALL Java_com_linkbridge_app_NativeBridge_nativeSetPermissions(
    JNIEnv* e, jclass, jstring device_id, jlong mask) {
    const auto node = snapshot();
    if (!node) return lb::android::to_jstring(e, "服务未启动");
    const std::string id = lb::android::to_string(e, device_id);
    const lb::Result r =
        node->trust().set_permissions(id, static_cast<std::uint32_t>(mask & 0xffffffffLL));
    return lb::android::to_jstring(e, r ? "" : r.error);
}

JNIEXPORT jstring JNICALL Java_com_linkbridge_app_NativeBridge_nativeRemoveDevice(
    JNIEnv* e, jclass, jstring device_id) {
    const auto node = snapshot();
    if (!node || !node->running()) return lb::android::to_jstring(e, "服务未启动");
    const lb::Result r = node->trust().remove(lb::android::to_string(e, device_id));
    return lb::android::to_jstring(e, r ? "" : r.error);
}

JNIEXPORT void JNICALL
Java_com_linkbridge_app_NativeBridge_nativeSetNotifyEnabled(JNIEnv*, jclass, jboolean value) {
    const auto node = snapshot();
    if (node) node->notify().set_enabled(value == JNI_TRUE);
}

JNIEXPORT void JNICALL Java_com_linkbridge_app_NativeBridge_nativeRespondConfirm(
    JNIEnv*, jclass, jint id, jboolean accept) {
    respond_confirm(static_cast<int>(id), accept == JNI_TRUE);
}

JNIEXPORT void JNICALL
Java_com_linkbridge_app_NativeBridge_nativeSetAutoAccept(JNIEnv*, jclass, jboolean value) {
    const bool v = value == JNI_TRUE;
    std::lock_guard<std::mutex> lk(g_allow_mu);
    for (const char* kind : {"pairing", "clipboard", "screen", "notify_action"}) {
        g_auto_allow[kind] = v;
    }
}

JNIEXPORT void JNICALL Java_com_linkbridge_app_NativeBridge_nativeSetAutoAllow(
    JNIEnv* e, jclass, jstring kind, jboolean value) {
    const std::string k = lb::android::to_string(e, kind);
    std::lock_guard<std::mutex> lk(g_allow_mu);
    g_auto_allow[k] = value == JNI_TRUE;
}

} // extern "C"
