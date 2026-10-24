#pragma once
// 心跳服务：独立通道、独立线程，绝不与业务通道共享锁或连接。
// 每次探测都是一次 STS 双向认证 + 加密的 PING/PONG，未配对设备无法响应。
#include "core/common/event_bus.h"
#include "core/common/stream.h"
#include "core/common/types.h"
#include "core/config/config.h"
#include "core/identity/identity.h"
#include "core/trust/trust_store.h"

#include <atomic>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

namespace lb {

struct DeviceOnline {
    DeviceId device_id;
    std::string name;
};

struct DeviceOffline {
    DeviceId device_id;
    std::string name;
};

struct DeviceStatus {
    DeviceId device_id;
    std::string name;
    std::string platform;
    Presence presence = Presence::Unknown;
    std::string last_ip;
    std::int64_t last_seen = 0;
    std::int64_t paired_at = 0;
};

// 心跳间隔自适应策略（纯函数，便于单测）
struct HeartbeatPolicy {
    int base_interval_sec = 30;
    int max_interval_sec = 300;
    int max_failures = 3;
    int stable_probes = 10;

    int after_success(int current_interval, int consecutive_ok) const;
    int after_failure() const;
    bool should_mark_offline(int consecutive_failures) const;
};

class HeartbeatService {
public:
    HeartbeatService(const Identity& identity, TrustStore& trust, EventBus& events,
                     IDialer& dialer, const HeartbeatConfig& config, int port,
                     int connect_timeout_ms);
    ~HeartbeatService();

    void start();
    void stop();

    // 服务端：处理入站心跳连接（监听线程调用）
    void handle_connection(std::unique_ptr<ByteStream> stream);

    Presence presence(const DeviceId& id) const;
    std::vector<DeviceStatus> statuses() const;
    void probe_now(const DeviceId& id);

    int current_interval_sec() const;
    void set_interval_sec(int seconds);
    void set_port(int port) { port_ = port; }
    HeartbeatPolicy policy() const;

    std::uint64_t probes_ok() const;
    std::uint64_t probes_failed() const;

private:
    struct DeviceState {
        Presence presence = Presence::Unknown;
        int failures = 0;
        int consecutive_ok = 0;
        int interval = 30;
        std::int64_t next_probe_at = 0;
        std::int64_t last_seen = 0;
    };

    void loop();
    bool probe_once(const TrustRecord& record);
    void update_state(const DeviceId& id, bool success);

    const Identity& identity_;
    TrustStore& trust_;
    EventBus& events_;
    IDialer& dialer_;
    HeartbeatConfig config_;
    int port_ = 45711;
    int connect_timeout_ms_ = 5000;

    mutable std::mutex mu_;
    std::map<DeviceId, DeviceState> states_;

    std::atomic<bool> running_{false};
    std::thread worker_;
    std::condition_variable cv_;
    std::mutex cv_mu_;

    std::atomic<std::uint64_t> ok_count_{0};
    std::atomic<std::uint64_t> failed_count_{0};
};

} // namespace lb
