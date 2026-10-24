#pragma once
// 剪贴板同步：本地变化监听（去抖/去重/体积限制）→ 加密通道推送 →
// 接收端 ACL 校验 + auto/confirm 策略 + 回环抑制。
#include "core/clipboard/backend.h"
#include "core/common/event_bus.h"
#include "core/common/stream.h"
#include "core/common/types.h"
#include "core/config/config.h"
#include "core/identity/identity.h"
#include "core/trust/trust_store.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace lb {

struct ClipboardReceived {
    DeviceId from;
    std::string from_name;
    std::string mime;
    std::uint64_t size = 0;
    bool applied = false;
    std::string reason;
};

struct ClipboardSent {
    DeviceId to;
    std::string to_name;
    std::string mime;
    std::uint64_t size = 0;
};

struct ClipboardFailed {
    DeviceId peer;
    std::string peer_name;
    std::string reason;
};

struct ClipboardSkipped {
    std::string mime;
    std::uint64_t size = 0;
    std::string reason;
};

enum class ClipboardDecision {
    Accept,
    Duplicate,
    LoopEcho,
    TooLarge,
    UnsupportedType,
};

const char* clipboard_decision_name(ClipboardDecision decision);

class ClipboardService {
public:
    ClipboardService(const Identity& identity, TrustStore& trust, EventBus& events,
                     IDialer& dialer, const ClipboardConfig& config,
                     std::shared_ptr<IClipboardBackend> backend, int connect_timeout_ms);
    ~ClipboardService();

    void start();
    void stop();
    bool running() const { return running_.load(); }

    // 读取本地剪贴板并推送到全部目标设备
    Result push_current(int timeout_ms);
    Expected<ClipboardContent> read_local();
    Result push_to_all(const ClipboardContent& content, int timeout_ms);
    Result push_to(const DeviceId& peer, const ClipboardContent& content, int timeout_ms);

    // 响应方：处理一条已建立的入站连接（由监听线程调用）
    void serve(std::unique_ptr<ByteStream> stream);

    void set_presence_provider(std::function<bool(const DeviceId&)> fn);
    void set_confirm_handler(
        std::function<bool(const ClipboardContent&, const std::string& from_name)> fn);
    void set_auto_accept(bool value) { auto_accept_ = value; }

    // 纯策略：类型/体积/去重/回环
    static ClipboardDecision evaluate(const ClipboardConfig& cfg, const std::string& mime,
                                      std::size_t size, const ByteBuf& digest,
                                      const ByteBuf& last_digest, const ByteBuf& remote_digest,
                                      bool within_loop_window);
    static bool supported_mime(const std::string& mime);

private:
    void loop();
    void remember_remote(const ByteBuf& digest);
    bool target_allowed(const TrustRecord& record) const;

    const Identity& identity_;
    TrustStore& trust_;
    EventBus& events_;
    IDialer& dialer_;
    ClipboardConfig config_;
    std::shared_ptr<IClipboardBackend> backend_;
    int connect_timeout_ms_ = 5000;

    std::function<bool(const DeviceId&)> presence_;
    std::function<bool(const ClipboardContent&, const std::string&)> confirm_;
    bool auto_accept_ = false;

    std::atomic<bool> running_{false};
    std::thread worker_;

    std::mutex mu_;
    ByteBuf last_local_digest_;
    ByteBuf last_remote_digest_;
    ByteBuf loop_digest_;
    std::chrono::steady_clock::time_point loop_until_{};
};

} // namespace lb
