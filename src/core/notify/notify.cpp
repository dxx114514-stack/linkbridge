#include "core/notify/notify.h"

#include "core/common/bytes.h"
#include "net/secure_channel.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <thread>

namespace lb {
namespace {

constexpr std::uint8_t kNotifyPush = 0x71;
constexpr std::uint8_t kNotifyAct = 0x72;
constexpr std::uint8_t kNotifyOk = 0x73;
constexpr std::uint8_t kNotifyFail = 0x74;

constexpr size_t kReplySlack = 4096;
constexpr int kActionDedupeSec = 3; // 双击/重试的幂等窗口
constexpr int kTitleMaxBytes = 512;

std::string peer_name_of(const TrustRecord& record) {
    return record.name.empty() ? record.device_id : record.name;
}

const char* action_mode_name(NotifyActionMode mode) {
    switch (mode) {
        case NotifyActionMode::OpenApp: return "open_app";
        case NotifyActionMode::Tap: return "tap";
        case NotifyActionMode::None: return "none";
    }
    return "none";
}

NotifyActionMode parse_action_mode(const std::string& value) {
    if (value == "tap") return NotifyActionMode::Tap;
    if (value == "open_app") return NotifyActionMode::OpenApp;
    return NotifyActionMode::None;
}

std::vector<std::string> split_csv(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream in(text);
    std::string item;
    while (std::getline(in, item, ',')) {
        const auto begin = item.find_first_not_of(" \t");
        const auto end = item.find_last_not_of(" \t");
        if (begin == std::string::npos) continue;
        out.push_back(item.substr(begin, end - begin + 1));
    }
    return out;
}

void write_message(ByteWriter& w, const NotifyMessage& msg) {
    w.str(msg.id);
    w.str(msg.app);
    w.str(msg.app_name);
    w.str(msg.title);
    w.str(msg.text);
    w.i64(msg.posted_at);
    w.u8(static_cast<std::uint8_t>(msg.priority < 0 ? 0 : (msg.priority > 2 ? 2 : msg.priority)));
}

bool read_message(ByteReader& r, NotifyMessage& msg) {
    msg.id = r.str();
    msg.app = r.str();
    msg.app_name = r.str();
    msg.title = r.str();
    msg.text = r.str();
    msg.posted_at = r.i64();
    msg.priority = static_cast<int>(r.u8());
    return r.ok();
}

} // namespace

NotifyService::NotifyService(const Identity& identity, TrustStore& trust, EventBus& events,
                             IDialer& dialer, const NotifyConfig& config,
                             std::shared_ptr<INotifyBackend> backend, int connect_timeout_ms)
    : identity_(identity),
      trust_(trust),
      events_(events),
      dialer_(dialer),
      config_(config),
      backend_(std::move(backend)),
      connect_timeout_ms_(connect_timeout_ms),
      enabled_(config.enabled),
      ignore_apps_(split_csv(config.ignore_apps)) {}

NotifyService::~NotifyService() { stop(); }

void NotifyService::start() { running_.store(true); }
void NotifyService::stop() { running_.store(false); }

void NotifyService::set_presence_provider(std::function<bool(const DeviceId&)> fn) {
    presence_ = std::move(fn);
}

void NotifyService::set_action_confirm_handler(
    std::function<bool(const std::string&, const NotifyMessage&)> fn) {
    std::lock_guard<std::mutex> lk(mu_);
    confirm_ = std::move(fn);
}

bool NotifyService::dedupe_accept(std::map<std::string, std::int64_t>& seen,
                                  const std::string& key, std::int64_t now, int window_sec) {
    const int window = std::max(1, window_sec);
    const auto it = seen.find(key);
    if (it != seen.end() && now - it->second < window) return false;
    seen[key] = now;
    return true;
}

std::string NotifyService::clamp_text(const std::string& text, int max_bytes) {
    if (max_bytes <= 0) return {};
    if (text.size() <= static_cast<std::size_t>(max_bytes)) return text;
    return text.substr(0, static_cast<std::size_t>(max_bytes));
}

bool NotifyService::valid_message(const NotifyMessage& msg, int max_text_bytes) {
    if (msg.id.empty() || msg.app.empty()) return false;
    if (msg.title.empty() && msg.text.empty()) return false;
    const std::size_t total = msg.title.size() + msg.text.size();
    return max_text_bytes > 0 && total <= static_cast<std::size_t>(max_text_bytes) * 2u;
}

bool NotifyService::app_ignored(const std::string& app) const {
    for (const auto& item : ignore_apps_)
        if (item == app) return true;
    return false;
}

bool NotifyService::target_allowed(const TrustRecord& record) const {
    if ((record.permissions & kPermNotifyMirror) == 0) return false;
    if (record.last_ip.empty()) return false;
    if (presence_ && !presence_(record.device_id)) return false;
    return true;
}

NotifyAction NotifyService::configured_action() const {
    NotifyAction action;
    action.mode = parse_action_mode(config_.action_mode);
    action.x = std::max(0, config_.action_x);
    action.y = std::max(0, config_.action_y);
    return action;
}

void NotifyService::purge_locked(std::int64_t now) {
    const int window = std::max(1, config_.dedupe_window_sec);
    const std::int64_t keep = std::max<std::int64_t>(window * 8, 120);
    auto prune = [&](std::map<std::string, std::int64_t>& map) {
        for (auto it = map.begin(); it != map.end();) {
            if (now - it->second > keep) it = map.erase(it);
            else ++it;
        }
    };
    prune(seen_inbound_);
    prune(seen_outbound_);
    prune(seen_actions_);
}

Result NotifyService::publish(const NotifyMessage& input, int timeout_ms) {
    if (!enabled_.load()) return Result::fail("通知镜像已停用");
    if (!valid_message(input, config_.max_text_bytes)) return Result::fail("通知内容为空或超长");
    if (app_ignored(input.app)) return Result::ok();

    NotifyMessage msg = input;
    msg.title = clamp_text(msg.title, kTitleMaxBytes);
    msg.text = clamp_text(msg.text, config_.max_text_bytes);
    if (msg.posted_at <= 0) msg.posted_at = now_secs();

    const std::int64_t now = now_secs();
    std::vector<TrustRecord> targets;
    {
        std::lock_guard<std::mutex> lk(mu_);
        purge_locked(now);
        const std::string key = msg.id + "|" + std::to_string(msg.posted_at);
        if (!dedupe_accept(seen_outbound_, key, now, config_.dedupe_window_sec)) {
            return Result::ok(); // 同一通知已推送过，避免重复
        }
    }
    for (const auto& record : trust_.all())
        if (target_allowed(record)) targets.push_back(record);
    if (targets.empty()) return Result::ok();

    std::mutex result_mu;
    std::string first_error;
    int failures = 0;
    std::vector<std::thread> workers;
    workers.reserve(targets.size());
    for (const auto& target : targets) {
        workers.emplace_back([&, target] {
            const Result r = push_to(target.device_id, msg, timeout_ms);
            if (!r) {
                std::lock_guard<std::mutex> lk(result_mu);
                if (first_error.empty()) first_error = r.error;
                ++failures;
            }
        });
    }
    for (auto& worker : workers) worker.join();

    if (failures == static_cast<int>(targets.size()) && failures > 0)
        return Result::fail(first_error.empty() ? "全部目标设备推送失败" : first_error);
    return Result::ok();
}

Result NotifyService::push_to(const DeviceId& peer, const NotifyMessage& msg, int timeout_ms) {
    const auto record = trust_.get(peer);
    if (!record) {
        events_.publish(NotifyFailed{peer, {}, "目标设备不在信任库中"});
        return Result::fail("目标设备不在信任库中");
    }
    if ((record->permissions & kPermNotifyMirror) == 0) {
        events_.publish(NotifyFailed{peer, record->name, "未授予通知镜像权限"});
        return Result::fail("未授予通知镜像权限");
    }
    if (record->last_ip.empty()) {
        const std::string reason = "尚未学习到对端 IP，请先完成心跳";
        events_.publish(NotifyFailed{peer, record->name, reason});
        return Result::fail(reason);
    }
    const int port = record->notify_port > 0 ? record->notify_port : 45717;

    auto dialed = dialer_.dial(record->last_ip, port, timeout_ms);
    if (!dialed) {
        events_.publish(NotifyFailed{peer, record->name, "连接失败: " + dialed.error});
        return Result::fail("连接对端失败: " + dialed.error);
    }
    std::unique_ptr<ByteStream> stream = std::move(*dialed);

    auto sts = sts_client(*stream, identity_, trust_, peer);
    if (!sts) {
        stream->close();
        events_.publish(NotifyFailed{peer, record->name, sts.error});
        return Result::fail(sts.error);
    }
    SessionKeys keys = std::move(sts->keys);
    std::uint64_t seq = 0;

    ByteWriter w;
    w.u8(kNotifyPush);
    write_message(w, msg);
    if (auto r = seal_send(*stream, keys, seq, w.buf()); !r) {
        stream->close();
        events_.publish(NotifyFailed{peer, record->name, r.error});
        return Result::fail(r.error);
    }

    auto resp = seal_recv(*stream, keys, seq, kReplySlack);
    if (!resp) {
        stream->close();
        events_.publish(NotifyFailed{peer, record->name, "等待对端应答失败"});
        return Result::fail("等待对端应答失败");
    }
    ByteReader r(*resp);
    const std::uint8_t type = r.u8();
    if (type == kNotifyFail) {
        const std::string why = r.str();
        stream->close();
        const std::string reason = why.empty() ? "对端拒绝镜像" : why;
        events_.publish(NotifyFailed{peer, record->name, reason});
        return Result::fail(reason);
    }
    if (type != kNotifyOk) {
        stream->close();
        events_.publish(NotifyFailed{peer, record->name, "对端应答类型错误"});
        return Result::fail("对端应答类型错误");
    }

    events_.publish(NotifyMirrorSent{peer, record->name, msg.app, msg.title});
    stream->close();
    return Result::ok();
}

Result NotifyService::request_action(const DeviceId& source, const std::string& notification_id,
                                     const std::string& app, const std::string& app_name,
                                     const std::string& title, int timeout_ms) {
    if (!enabled_.load()) return Result::fail("通知镜像已停用");
    const auto record = trust_.get(source);
    if (!record) return Result::fail("来源设备不在信任库中");
    if (record->last_ip.empty()) return Result::fail("来源设备尚未在线（缺少 IP）");
    if (config_.action_mode == "none")
        return Result::fail("来源设备未启用通知动作（notify.action_mode=none）");

    {
        std::lock_guard<std::mutex> lk(mu_);
        const std::int64_t now = now_secs();
        purge_locked(now);
        const std::string key = source + "|" + notification_id;
        if (!dedupe_accept(seen_actions_, key, now, kActionDedupeSec))
            return Result::ok(); // 同一通知的连续点击只执行一次
    }

    const int port = record->notify_port > 0 ? record->notify_port : 45717;
    auto dialed = dialer_.dial(record->last_ip, port, timeout_ms);
    if (!dialed) {
        events_.publish(NotifyActionDone{source, notification_id, false,
                                         "连接来源设备失败: " + dialed.error});
        return Result::fail("连接来源设备失败: " + dialed.error);
    }
    std::unique_ptr<ByteStream> stream = std::move(*dialed);

    auto sts = sts_client(*stream, identity_, trust_, source);
    if (!sts) {
        stream->close();
        events_.publish(NotifyActionDone{source, notification_id, false, sts.error});
        return Result::fail(sts.error);
    }
    SessionKeys keys = std::move(sts->keys);
    std::uint64_t seq = 0;
    const NotifyAction action = configured_action();

    ByteWriter w;
    w.u8(kNotifyAct);
    w.str(notification_id);
    w.str(app);
    w.str(app_name);
    w.str(title);
    w.str(action_mode_name(action.mode));
    w.u32(static_cast<std::uint32_t>(action.x));
    w.u32(static_cast<std::uint32_t>(action.y));
    if (auto r = seal_send(*stream, keys, seq, w.buf()); !r) {
        stream->close();
        events_.publish(NotifyActionDone{source, notification_id, false, r.error});
        return Result::fail(r.error);
    }

    auto resp = seal_recv(*stream, keys, seq, kReplySlack);
    if (!resp) {
        stream->close();
        events_.publish(NotifyActionDone{source, notification_id, false, "等待来源设备应答失败"});
        return Result::fail("等待来源设备应答失败");
    }
    ByteReader r(*resp);
    const std::uint8_t type = r.u8();
    if (type == kNotifyFail) {
        const std::string why = r.str();
        stream->close();
        const std::string reason = why.empty() ? "来源设备拒绝执行" : why;
        events_.publish(NotifyActionDone{source, notification_id, false, reason});
        return Result::fail(reason);
    }
    if (type != kNotifyOk) {
        stream->close();
        events_.publish(NotifyActionDone{source, notification_id, false, "应答类型错误"});
        return Result::fail("来源设备应答类型错误");
    }
    stream->close();
    events_.publish(NotifyActionDone{source, notification_id, true, "来源设备已执行预设动作"});
    return Result::ok();
}

void NotifyService::serve(std::unique_ptr<ByteStream> stream) {
    if (!stream) return;

    auto sts = sts_server(*stream, identity_, trust_);
    if (!sts) {
        stream->close();
        return;
    }
    SessionKeys keys = std::move(sts->keys);
    std::uint64_t seq = 0;
    const DeviceId peer = sts->peer_id;
    const auto record = trust_.get(peer);
    const std::string name = record ? peer_name_of(*record) : peer;

    auto send_fail = [&](const std::string& reason) {
        ByteWriter w;
        w.u8(kNotifyFail);
        w.str(reason);
        seal_send(*stream, keys, seq, w.buf());
        stream->close();
    };

    if (!record) {
        send_fail("设备不在信任库中");
        return;
    }
    if (!running_.load()) {
        send_fail("通知镜像服务未启动");
        return;
    }
    if (!enabled_.load()) {
        send_fail("本机已停用通知镜像");
        return;
    }

    auto msg = seal_recv(*stream, keys, seq, 65536);
    if (!msg) {
        stream->close();
        return;
    }
    ByteReader r(*msg);
    const std::uint8_t type = r.u8();

    if (type == kNotifyPush) {
        if ((record->permissions & kPermNotifyMirror) == 0) {
            send_fail("未授予通知镜像权限");
            return;
        }
        NotifyMessage note;
        if (!read_message(r, note)) {
            send_fail("通知格式非法");
            return;
        }
        note.title = clamp_text(note.title, kTitleMaxBytes);
        note.text = clamp_text(note.text, config_.max_text_bytes);
        if (note.id.empty() || note.app.empty()) {
            send_fail("通知缺少标识");
            return;
        }
        if (app_ignored(note.app)) {
            send_fail("该应用在忽略列表中");
            return;
        }

        const std::int64_t now = now_secs();
        {
            std::lock_guard<std::mutex> lk(mu_);
            purge_locked(now);
            const std::string key = peer + "|" + note.id + "|" + std::to_string(note.posted_at);
            if (!dedupe_accept(seen_inbound_, key, now, config_.dedupe_window_sec)) {
                // 重复镜像不再弹窗，但回 OK 让来源端停止重试
                ByteWriter w;
                w.u8(kNotifyOk);
                w.str("");
                seal_send(*stream, keys, seq, w.buf());
                stream->close();
                return;
            }
        }

        if (!backend_ || !backend_->available()) {
            const std::string why = backend_ ? backend_->unavailable_reason()
                                             : std::string("当前平台不支持通知展示");
            events_.publish(NotifyMirrored{peer, name, note.app, note.app_name, note.title, false,
                                           why});
            send_fail(why);
            return;
        }
        const Result shown = backend_->show(note, peer, name);
        if (!shown) {
            events_.publish(NotifyMirrored{peer, name, note.app, note.app_name, note.title, false,
                                           shown.error});
            send_fail(shown.error);
            return;
        }
        events_.publish(NotifyMirrored{peer, name, note.app, note.app_name, note.title, true, ""});
        ByteWriter w;
        w.u8(kNotifyOk);
        w.str("");
        seal_send(*stream, keys, seq, w.buf());
        stream->close();
        return;
    }

    if (type == kNotifyAct) {
        const std::string notification_id = r.str();
        NotifyMessage note;
        note.id = notification_id;
        note.app = r.str();
        note.app_name = r.str();
        note.title = r.str();
        const std::string requested_mode = r.str();
        const int req_x = static_cast<int>(r.u32());
        const int req_y = static_cast<int>(r.u32());
        if (!r.ok() || notification_id.empty()) {
            send_fail("动作请求格式非法");
            return;
        }
        note.title = clamp_text(note.title, kTitleMaxBytes);
        (void)requested_mode;
        (void)req_x;
        (void)req_y;

        if ((record->permissions & kPermNotifyAction) == 0) {
            events_.publish(NotifyActionReceived{peer, name, notification_id, "denied", false,
                                                 "未授予通知动作权限"});
            send_fail("未授予通知动作权限");
            return;
        }
        const NotifyAction action = configured_action();
        if (action.mode == NotifyActionMode::None) {
            events_.publish(NotifyActionReceived{peer, name, notification_id, "none", false,
                                                 "本机未启用通知动作"});
            send_fail("本机未启用通知动作（notify.action_mode=none）");
            return;
        }
        if (!backend_ || !backend_->available()) {
            const std::string why = backend_ ? backend_->unavailable_reason()
                                             : std::string("当前平台不支持通知动作");
            events_.publish(NotifyActionReceived{peer, name, notification_id,
                                                 action_mode_name(action.mode), false, why});
            send_fail(why);
            return;
        }

        {
            std::lock_guard<std::mutex> lk(mu_);
            const std::int64_t now = now_secs();
            purge_locked(now);
            const std::string key = peer + "|" + notification_id;
            if (!dedupe_accept(seen_actions_, key, now, kActionDedupeSec)) {
                ByteWriter w;
                w.u8(kNotifyOk);
                w.str("重复请求已忽略");
                seal_send(*stream, keys, seq, w.buf());
                stream->close();
                return;
            }
        }

        if (!config_.auto_action) {
            bool allowed = false;
            {
                std::lock_guard<std::mutex> lk(mu_);
                if (confirm_) allowed = confirm_(name, note);
            }
            if (!allowed) {
                events_.publish(NotifyActionReceived{peer, name, notification_id,
                                                     action_mode_name(action.mode), false,
                                                     "用户拒绝或无人确认"});
                send_fail("用户拒绝或未配置确认处理器");
                return;
            }
        }

        if (config_.action_delay_ms > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(config_.action_delay_ms));
        const Result done = backend_->act(note, action);
        const bool ok = done.success;
        events_.publish(NotifyActionReceived{peer, name, notification_id,
                                             action_mode_name(action.mode), ok,
                                             ok ? std::string() : done.error});
        if (!done) {
            send_fail(done.error);
            return;
        }
        ByteWriter w;
        w.u8(kNotifyOk);
        w.str("");
        seal_send(*stream, keys, seq, w.buf());
        stream->close();
        return;
    }

    send_fail("协议错误");
}

} // namespace lb
