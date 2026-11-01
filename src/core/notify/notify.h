#pragma once
// 跨设备通知流转：A 设备把本地通知镜像到已授权设备；
// 用户点击镜像通知时，向来源设备发起动作请求，由来源设备执行预设操作
// （打开来源应用 / 按预设坐标点击）。带来源去重窗口与动作幂等保护，
// 避免重复触发；失败时明确返回原因，不影响其他通道。
#include "core/common/event_bus.h"
#include "core/common/stream.h"
#include "core/common/types.h"
#include "core/config/config.h"
#include "core/identity/identity.h"
#include "core/notify/backend.h"
#include "core/trust/trust_store.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace lb {

struct NotifyMirrored {
    DeviceId from;
    std::string from_name;
    std::string app;
    std::string app_name;
    std::string title;
    bool applied = false;
    std::string reason;
};

struct NotifyMirrorSent {
    DeviceId to;
    std::string to_name;
    std::string app;
    std::string title;
};

struct NotifyFailed {
    DeviceId peer;
    std::string peer_name;
    std::string reason;
};

struct NotifyActionReceived {
    DeviceId from;
    std::string from_name;
    std::string notification_id;
    std::string mode;
    bool executed = false;
    std::string reason;
};

struct NotifyActionDone {
    DeviceId peer;
    std::string notification_id;
    bool ok = false;
    std::string reason;
};

class NotifyService {
public:
    NotifyService(const Identity& identity, TrustStore& trust, EventBus& events, IDialer& dialer,
                  const NotifyConfig& config, std::shared_ptr<INotifyBackend> backend,
                  int connect_timeout_ms);
    ~NotifyService();

    void start();
    void stop();
    bool running() const { return running_.load(); }

    // 运行时开关（Android UI 可直接切换，不写配置）
    void set_enabled(bool value) { enabled_.store(value); }
    bool enabled() const { return enabled_.load(); }

    // 本机产生一条通知 → 推送到所有已授权镜像的在线设备
    Result publish(const NotifyMessage& msg, int timeout_ms);
    Result push_to(const DeviceId& peer, const NotifyMessage& msg, int timeout_ms);

    // 镜像通知被点击 → 请求来源设备执行预设动作
    Result request_action(const DeviceId& source, const std::string& notification_id,
                          const std::string& app, const std::string& app_name,
                          const std::string& title, int timeout_ms);

    // 响应方：处理一条已建立的入站连接（由监听线程调用）
    void serve(std::unique_ptr<ByteStream> stream);

    void set_presence_provider(std::function<bool(const DeviceId&)> fn);
    // 动作请求需要人工确认时回调；未设置且 auto_action=false 时一律拒绝
    void set_action_confirm_handler(
        std::function<bool(const std::string& from_name, const NotifyMessage& msg)> fn);

    // 纯函数：同一 key 在窗口内只接受一次（便于单测）
    static bool dedupe_accept(std::map<std::string, std::int64_t>& seen, const std::string& key,
                              std::int64_t now, int window_sec);
    // 截断过长文本，保证传输与展示稳定
    static std::string clamp_text(const std::string& text, int max_bytes);

private:
    void purge_locked(std::int64_t now);
    bool target_allowed(const TrustRecord& record) const;
    bool app_ignored(const std::string& app) const;
    NotifyAction configured_action() const;
    static bool valid_message(const NotifyMessage& msg, int max_text_bytes);

    const Identity& identity_;
    TrustStore& trust_;
    EventBus& events_;
    IDialer& dialer_;
    NotifyConfig config_;
    std::shared_ptr<INotifyBackend> backend_;
    int connect_timeout_ms_ = 5000;

    std::function<bool(const DeviceId&)> presence_;
    std::function<bool(const std::string&, const NotifyMessage&)> confirm_;
    std::atomic<bool> running_{false};
    std::atomic<bool> enabled_{true};

    mutable std::mutex mu_;
    std::map<std::string, std::int64_t> seen_inbound_; // peer|id|posted_at
    std::map<std::string, std::int64_t> seen_outbound_;
    std::map<std::string, std::int64_t> seen_actions_; // peer|id，短窗口幂等
    std::vector<std::string> ignore_apps_;
};

} // namespace lb
