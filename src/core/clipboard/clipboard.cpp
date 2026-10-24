#include "core/clipboard/clipboard.h"

#include "core/common/bytes.h"
#include "core/crypto/crypto.h"
#include "net/secure_channel.h"

#include <algorithm>
#include <chrono>
#include <vector>

namespace lb {
namespace {

constexpr std::uint8_t kPush = 0x41;
constexpr std::uint8_t kOk = 0x42;
constexpr std::uint8_t kFailMsg = 0x43;

constexpr int kDebounceMs = 500;             // 连续变化只取最后一次
constexpr int kLoopTtlMs = 5000;             // 远端写入后的回环抑制窗口
constexpr int kFallbackPollMs = 2000;        // 事件驱动的 2s 兜底轮询
constexpr size_t kFrameSlack = 4096;

std::string mime_of(const ClipboardContent& c) { return c.mime; }

} // namespace

const char* clipboard_decision_name(ClipboardDecision decision) {
    switch (decision) {
        case ClipboardDecision::Accept: return "accept";
        case ClipboardDecision::Duplicate: return "duplicate";
        case ClipboardDecision::LoopEcho: return "loop-echo";
        case ClipboardDecision::TooLarge: return "too-large";
        case ClipboardDecision::UnsupportedType: return "unsupported-type";
    }
    return "unknown";
}

ClipboardService::ClipboardService(const Identity& identity, TrustStore& trust, EventBus& events,
                                   IDialer& dialer, const ClipboardConfig& config,
                                   std::shared_ptr<IClipboardBackend> backend,
                                   int connect_timeout_ms)
    : identity_(identity),
      trust_(trust),
      events_(events),
      dialer_(dialer),
      config_(config),
      backend_(std::move(backend)),
      connect_timeout_ms_(connect_timeout_ms) {}

ClipboardService::~ClipboardService() { stop(); }

bool ClipboardService::supported_mime(const std::string& mime) {
    return mime == "text/plain" || mime == "image/dib";
}

ClipboardDecision ClipboardService::evaluate(const ClipboardConfig& cfg, const std::string& mime,
                                             std::size_t size, const ByteBuf& digest,
                                             const ByteBuf& last_digest,
                                             const ByteBuf& remote_digest,
                                             bool within_loop_window) {
    if (!supported_mime(mime)) return ClipboardDecision::UnsupportedType;
    if (size == 0) return ClipboardDecision::UnsupportedType;
    if (cfg.max_bytes > 0 && size > static_cast<std::size_t>(cfg.max_bytes))
        return ClipboardDecision::TooLarge;
    if (within_loop_window && !digest.empty() && digest == remote_digest)
        return ClipboardDecision::LoopEcho;
    if (!digest.empty() && digest == last_digest) return ClipboardDecision::Duplicate;
    return ClipboardDecision::Accept;
}

void ClipboardService::set_presence_provider(std::function<bool(const DeviceId&)> fn) {
    presence_ = std::move(fn);
}

void ClipboardService::set_confirm_handler(
    std::function<bool(const ClipboardContent&, const std::string&)> fn) {
    confirm_ = std::move(fn);
}

void ClipboardService::remember_remote(const ByteBuf& digest) {
    std::lock_guard<std::mutex> lk(mu_);
    last_remote_digest_ = digest;
    last_local_digest_ = digest; // 防止本端 watcher 立刻回推
    loop_digest_ = digest;
    loop_until_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(kLoopTtlMs);
}

bool ClipboardService::target_allowed(const TrustRecord& record) const {
    if ((record.permissions & kPermClipboard) == 0) return false;
    if (record.last_ip.empty()) return false;
    if (presence_ && !presence_(record.device_id)) return false;
    return true;
}

void ClipboardService::start() {
    if (running_.exchange(true)) return;
    if (!config_.enabled || !backend_) {
        running_.store(false);
        return;
    }
    worker_ = std::thread([this] { loop(); });
}

void ClipboardService::stop() {
    if (!running_.exchange(false)) return;
    if (backend_) backend_->wait_for_change(0); // 唤醒等待（实现可忽略）
    if (worker_.joinable()) worker_.join();
}

void ClipboardService::loop() {
    while (running_.load()) {
        const bool signaled = backend_->wait_for_change(kFallbackPollMs);

        if (signaled) {
            // 去抖：等待 500ms 内不再变化
            for (int i = 0; i < 10 && running_.load(); ++i) {
                const std::uint64_t seq = backend_->sequence();
                std::this_thread::sleep_for(std::chrono::milliseconds(kDebounceMs));
                if (backend_->sequence() == seq) break;
            }
        }
        if (!running_.load()) return;

        auto content = backend_->read();
        if (!content || content->empty()) continue;

        const ByteBuf digest = crypto::sha256(content->data);
        if (digest.empty()) continue;

        ByteBuf last_digest;
        ByteBuf remote_digest;
        bool within_window = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            last_digest = last_local_digest_;
            remote_digest = loop_digest_;
            within_window = std::chrono::steady_clock::now() < loop_until_;
        }

        const ClipboardDecision decision =
            evaluate(config_, content->mime, content->data.size(), digest, last_digest,
                     remote_digest, within_window);
        if (decision == ClipboardDecision::Duplicate ||
            decision == ClipboardDecision::LoopEcho) {
            continue;
        }
        if (decision == ClipboardDecision::TooLarge ||
            decision == ClipboardDecision::UnsupportedType) {
            events_.publish(ClipboardSkipped{content->mime, content->data.size(),
                                             clipboard_decision_name(decision)});
            continue;
        }

        {
            std::lock_guard<std::mutex> lk(mu_);
            last_local_digest_ = digest;
        }
        push_to_all(*content, connect_timeout_ms_);
    }
}

Result ClipboardService::push_current(int timeout_ms) {
    auto content = read_local();
    if (!content) return Result::fail("读取本地剪贴板失败: " + content.error);
    if (content->empty()) return Result::ok();
    return push_to_all(*content, timeout_ms);
}

Expected<ClipboardContent> ClipboardService::read_local() {
    if (!backend_) return Expected<ClipboardContent>::fail("剪贴板后端未初始化");
    return backend_->read();
}

Result ClipboardService::push_to_all(const ClipboardContent& content, int timeout_ms) {
    if (!supported_mime(content.mime))
        return Result::fail("不支持的剪贴板类型: " + content.mime);
    if (config_.max_bytes > 0 &&
        content.data.size() > static_cast<std::size_t>(config_.max_bytes))
        return Result::fail("剪贴板内容超过本机 max_bytes 限制");

    std::vector<TrustRecord> targets;
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
            const Result r = push_to(target.device_id, content, timeout_ms);
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

Result ClipboardService::push_to(const DeviceId& peer, const ClipboardContent& content,
                                 int timeout_ms) {
    const auto record = trust_.get(peer);
    if (!record) {
        events_.publish(ClipboardFailed{peer, {}, "目标设备不在信任库中"});
        return Result::fail("目标设备不在信任库中");
    }
    if (record->last_ip.empty()) {
        const std::string reason = "尚未学习到对端 IP，请先完成心跳";
        events_.publish(ClipboardFailed{peer, record->name, reason});
        return Result::fail(reason);
    }
    const int port = record->clipboard_port > 0 ? record->clipboard_port : 45714;
    const ByteBuf digest = crypto::sha256(content.data);

    auto dialed = dialer_.dial(record->last_ip, port, timeout_ms);
    if (!dialed) {
        events_.publish(ClipboardFailed{peer, record->name, "连接失败: " + dialed.error});
        return Result::fail("连接对端失败: " + dialed.error);
    }
    std::unique_ptr<ByteStream> stream = std::move(*dialed);

    auto sts = sts_client(*stream, identity_, trust_, peer);
    if (!sts) {
        stream->close();
        events_.publish(ClipboardFailed{peer, record->name, sts.error});
        return Result::fail(sts.error);
    }

    SessionKeys keys = std::move(sts->keys);
    std::uint64_t seq = 0;
    ByteWriter w;
    w.u8(kPush);
    w.str(mime_of(content));
    w.bytes(digest);
    w.bytes(content.data);
    w.u64(now_secs());
    if (auto r = seal_send(*stream, keys, seq, w.buf()); !r) {
        stream->close();
        events_.publish(ClipboardFailed{peer, record->name, r.error});
        return Result::fail(r.error);
    }

    auto resp = seal_recv(*stream, keys, seq, 65536);
    if (!resp) {
        stream->close();
        events_.publish(ClipboardFailed{peer, record->name, "等待对端应答失败"});
        return Result::fail("等待对端应答失败");
    }

    ByteReader r(*resp);
    const std::uint8_t type = r.u8();
    if (type == kFailMsg) {
        const std::string reason = r.str();
        stream->close();
        const std::string why = reason.empty() ? "对端拒绝" : reason;
        events_.publish(ClipboardFailed{peer, record->name, why});
        return Result::fail(why);
    }
    if (type != kOk) {
        stream->close();
        events_.publish(ClipboardFailed{peer, record->name, "对端应答类型错误"});
        return Result::fail("对端应答类型错误");
    }

    events_.publish(ClipboardSent{peer, record->name, content.mime,
                                  static_cast<std::uint64_t>(content.data.size())});
    stream->close();
    return Result::ok();
}

void ClipboardService::serve(std::unique_ptr<ByteStream> stream) {
    if (!stream) return;

    auto sts = sts_server(*stream, identity_, trust_);
    if (!sts) {
        stream->close();
        return;
    }
    SessionKeys keys = std::move(sts->keys);
    std::uint64_t seq = 0;
    ByteWriter fail;
    fail.u8(kFailMsg);
    fail.str("未授予剪贴板权限");

    const auto record = trust_.get(sts->peer_id);
    if (!record || (record->permissions & kPermClipboard) == 0) {
        seal_send(*stream, keys, seq, fail.buf());
        stream->close();
        return;
    }
    if (!config_.enabled) {
        ByteWriter disabled;
        disabled.u8(kFailMsg);
        disabled.str("对端已停用剪贴板同步");
        seal_send(*stream, keys, seq, disabled.buf());
        stream->close();
        return;
    }
    const std::string from_name = record->name.empty() ? sts->peer_id : record->name;

    auto msg = seal_recv(*stream, keys, seq,
                         static_cast<size_t>(config_.max_bytes) + kFrameSlack);
    if (!msg) {
        stream->close();
        return;
    }

    std::string mime;
    ByteBuf digest;
    ByteBuf data;
    {
        ByteReader r(*msg);
        if (r.u8() != kPush) {
            ByteWriter f;
            f.u8(kFailMsg);
            f.str("协议错误");
            seal_send(*stream, keys, seq, f.buf());
            stream->close();
            return;
        }
        mime = r.str();
        digest = r.bytes();
        data = r.bytes();
        (void)r.u64(); // ts：v0 仅保留字段，不做时钟窗口校验（消息已加密且连接双向认证）
        if (!r.ok() || digest.size() != crypto::kSha256Len) {
            ByteWriter f;
            f.u8(kFailMsg);
            f.str("剪贴板消息格式非法");
            seal_send(*stream, keys, seq, f.buf());
            stream->close();
            return;
        }
    }

    const std::string reject = [&] {
        if (!supported_mime(mime)) return std::string("不支持的剪贴板类型");
        if (data.empty()) return std::string("剪贴板内容为空");
        if (config_.max_bytes > 0 &&
            data.size() > static_cast<std::size_t>(config_.max_bytes))
            return std::string("剪贴板内容超过 max_bytes 限制");
        if (crypto::sha256(data) != digest) return std::string("剪贴板内容哈希不匹配");
        return std::string();
    }();

    auto reply = [&](bool ok, const std::string& note) {
        ByteWriter w;
        w.u8(ok ? kOk : kFailMsg);
        w.str(note);
        seal_send(*stream, keys, seq, w.buf());
    };

    if (!reject.empty()) {
        reply(false, reject);
        events_.publish(ClipboardReceived{sts->peer_id, from_name, mime, data.size(), false,
                                          reject});
        stream->close();
        return;
    }

    bool duplicate = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        duplicate = !last_remote_digest_.empty() && last_remote_digest_ == digest;
    }
    if (duplicate) {
        // 幂等：重复推送不重复覆盖本地剪贴板
        reply(true, "duplicate");
        stream->close();
        return;
    }

    if (!auto_accept_ && config_.mode != "auto" &&
        (!confirm_ || !confirm_(ClipboardContent{mime, data}, from_name))) {
        const std::string reason = "用户拒绝或未配置确认处理器";
        reply(false, reason);
        events_.publish(ClipboardReceived{sts->peer_id, from_name, mime, data.size(), false,
                                          reason});
        stream->close();
        return;
    }

    ClipboardContent content{mime, data};
    if (auto w = backend_->write(content); !w) {
        reply(false, "写入本地剪贴板失败: " + w.error);
        events_.publish(ClipboardReceived{sts->peer_id, from_name, mime, data.size(), false,
                                          w.error});
        stream->close();
        return;
    }

    remember_remote(digest);
    reply(true, "");
    events_.publish(ClipboardReceived{sts->peer_id, from_name, mime, data.size(), true, ""});
    stream->close();
}

} // namespace lb
