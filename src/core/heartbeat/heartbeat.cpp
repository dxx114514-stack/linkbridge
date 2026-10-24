#include "core/heartbeat/heartbeat.h"

#include "core/common/bytes.h"
#include "core/platform/platform.h"
#include "net/secure_channel.h"

#include <algorithm>

namespace lb {
namespace {

constexpr std::uint8_t kPing = 0x21;
constexpr std::uint8_t kPong = 0x22;

ByteBuf build_ping(std::uint32_t seq) {
    ByteWriter w;
    w.u8(kPing);
    w.u32(seq);
    w.i64(now_secs());
    w.u8(1); // 协议版本
    return w.buf();
}

} // namespace

// ---------------- HeartbeatPolicy ----------------

int HeartbeatPolicy::after_success(int current_interval, int consecutive_ok) const {
    if (consecutive_ok >= stable_probes && current_interval < max_interval_sec)
        return std::min(max_interval_sec, current_interval * 2);
    return current_interval;
}

int HeartbeatPolicy::after_failure() const { return base_interval_sec; }

bool HeartbeatPolicy::should_mark_offline(int consecutive_failures) const {
    return consecutive_failures >= max_failures;
}

// ---------------- HeartbeatService ----------------

HeartbeatService::HeartbeatService(const Identity& identity, TrustStore& trust, EventBus& events,
                                   IDialer& dialer, const HeartbeatConfig& config, int port,
                                   int connect_timeout_ms)
    : identity_(identity),
      trust_(trust),
      events_(events),
      dialer_(dialer),
      config_(config),
      port_(port),
      connect_timeout_ms_(connect_timeout_ms) {}

HeartbeatService::~HeartbeatService() { stop(); }

HeartbeatPolicy HeartbeatService::policy() const {
    HeartbeatPolicy p;
    p.base_interval_sec = config_.interval_sec;
    p.max_interval_sec = config_.max_interval_sec;
    p.max_failures = config_.max_failures;
    return p;
}

void HeartbeatService::start() {
    if (running_.exchange(true)) return;
    {
        std::lock_guard<std::mutex> lk(mu_);
        states_.clear();
    }
    worker_ = std::thread([this] { loop(); });
}

void HeartbeatService::stop() {
    if (!running_.exchange(false)) return;
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void HeartbeatService::set_interval_sec(int seconds) {
    if (seconds < 5) seconds = 5;
    std::lock_guard<std::mutex> lk(mu_);
    config_.interval_sec = seconds;
    config_.offline_probe_sec = std::max(config_.offline_probe_sec, seconds);
    for (auto& kv : states_) kv.second.interval = seconds;
}

int HeartbeatService::current_interval_sec() const {
    std::lock_guard<std::mutex> lk(mu_);
    return config_.interval_sec;
}

Presence HeartbeatService::presence(const DeviceId& id) const {
    std::lock_guard<std::mutex> lk(mu_);
    const auto it = states_.find(id);
    return it == states_.end() ? Presence::Unknown : it->second.presence;
}

std::uint64_t HeartbeatService::probes_ok() const { return ok_count_.load(); }
std::uint64_t HeartbeatService::probes_failed() const { return failed_count_.load(); }

std::vector<DeviceStatus> HeartbeatService::statuses() const {
    std::vector<DeviceStatus> out;
    const auto records = trust_.all();
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& r : records) {
        DeviceStatus s;
        s.device_id = r.device_id;
        s.name = r.name;
        s.platform = platform_name(r.platform);
        s.last_ip = r.last_ip;
        s.paired_at = r.paired_at;
        const auto it = states_.find(r.device_id);
        if (it != states_.end()) {
            s.presence = it->second.presence;
            s.last_seen = it->second.last_seen;
        }
        out.push_back(std::move(s));
    }
    return out;
}

void HeartbeatService::probe_now(const DeviceId& id) {
    const auto record = trust_.get(id);
    if (!record) return;
    probe_once(*record);
}

void HeartbeatService::loop() {
    while (running_.load()) {
        const std::int64_t now = now_secs();
        const auto records = trust_.all();

        for (const auto& record : records) {
            if (!running_.load()) return;
            bool due = false;
            {
                std::lock_guard<std::mutex> lk(mu_);
                auto& st = states_[record.device_id];
                if (st.interval == 0) st.interval = config_.interval_sec;
                if (st.next_probe_at == 0) {
                    st.next_probe_at = now; // 首轮立即探测
                    // 已有在线/离线判定时不重置为未知
                    if (st.presence == Presence::Unknown) st.presence = Presence::Offline;
                }
                due = now >= st.next_probe_at;
            }
            if (!due) continue;

            probe_once(record);

            std::lock_guard<std::mutex> lk(mu_);
            auto& st = states_[record.device_id];
            st.next_probe_at = now_secs() + (st.presence == Presence::Offline
                                                 ? config_.offline_probe_sec
                                                 : st.interval);
        }

        std::unique_lock<std::mutex> lk(cv_mu_);
        cv_.wait_for(lk, std::chrono::milliseconds(300), [this] { return !running_.load(); });
    }
}

bool HeartbeatService::probe_once(const TrustRecord& record) {
    bool success = false;
    if (!record.last_ip.empty()) {
        const int target_port = record.heartbeat_port > 0 ? record.heartbeat_port : port_;
        auto dialed = dialer_.dial(record.last_ip, target_port, connect_timeout_ms_);
        if (dialed) {
            std::unique_ptr<ByteStream> stream = std::move(*dialed);
            auto session = sts_client(*stream, identity_, trust_, record.device_id);
            if (session) {
                std::uint64_t seq = 0;
                const std::uint32_t ping_seq =
                    static_cast<std::uint32_t>(now_secs() & 0x7fffffff);
                const Result sent = seal_send(*stream, session->keys, seq, build_ping(ping_seq));
                if (sent) {
                    auto pong = seal_recv(*stream, session->keys, seq);
                    if (pong) {
                        ByteReader r(*pong);
                        const std::uint8_t type = r.u8();
                        const std::uint32_t echoed = r.u32();
                        r.i64();
                        r.u8();
                        if (r.ok() && type == kPong && echoed == ping_seq) success = true;
                    }
                }
            }
            stream->close();
        }
    }

    if (success) ++ok_count_;
    else ++failed_count_;
    update_state(record.device_id, success);
    return success;
}

void HeartbeatService::update_state(const DeviceId& id, bool success) {
    bool became_online = false;
    bool became_offline = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto& st = states_[id];
        if (st.interval == 0) st.interval = config_.interval_sec;
        if (success) {
            st.failures = 0;
            ++st.consecutive_ok;
            st.interval = policy().after_success(st.interval, st.consecutive_ok);
            st.last_seen = now_secs();
            if (st.presence != Presence::Online) {
                st.presence = Presence::Online;
                became_online = true;
            }
        } else {
            ++st.failures;
            st.consecutive_ok = 0;
            st.interval = policy().after_failure();
            if (policy().should_mark_offline(st.failures) && st.presence != Presence::Offline) {
                st.presence = Presence::Offline;
                became_offline = true;
            }
        }
    }

    if (!became_online && !became_offline) return;
    std::string name = id;
    if (const auto rec = trust_.get(id)) name = rec->name;
    if (became_online) events_.publish(DeviceOnline{id, name});
    else events_.publish(DeviceOffline{id, name});
}

void HeartbeatService::handle_connection(std::unique_ptr<ByteStream> stream) {
    if (!stream) return;
    auto session = sts_server(*stream, identity_, trust_);
    if (!session) return; // 未配对设备：握手阶段即中断，不泄露任何状态

    std::uint64_t seq = 0;
    for (int i = 0; i < 8; ++i) { // 单连接最多处理 8 次 ping，避免长连接占资源
        auto frame = seal_recv(*stream, session->keys, seq);
        if (!frame) break;
        ByteReader r(*frame);
        const std::uint8_t type = r.u8();
        const std::uint32_t ping_seq = r.u32();
        const std::int64_t peer_ts = r.i64();
        r.u8();
        if (!r.ok() || type != kPing) break;

        ByteWriter w;
        w.u8(kPong);
        w.u32(ping_seq);
        w.i64(now_secs());
        w.u32(0); // capabilities 位图，v0 保留
        if (auto res = seal_send(*stream, session->keys, seq, w.buf()); !res) break;

        (void)peer_ts;
        update_state(session->peer_id, true); // 对端能主动探测说明其在线
    }
    stream->close();
}

} // namespace lb
