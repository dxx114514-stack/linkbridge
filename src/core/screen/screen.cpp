#include "core/screen/screen.h"

#include "core/common/bytes.h"
#include "core/crypto/crypto.h"
#include "core/platform/platform.h"
#include "net/secure_channel.h"

#include <algorithm>
#include <chrono>
#include <thread>
#include <vector>

namespace lb {
namespace {

constexpr std::uint8_t kView = 0x61;
constexpr std::uint8_t kAccept = 0x62;
constexpr std::uint8_t kFrameRequest = 0x63;
constexpr std::uint8_t kFrame = 0x64;
constexpr std::uint8_t kStop = 0x65;
constexpr std::uint8_t kFailMsg = 0x66;
constexpr std::uint8_t kInput = 0x67;

constexpr size_t kFrameSlack = 4096;
constexpr size_t kMaxQueuedInputs = 32;
constexpr int kTokenBytes = 8; // 16 个 hex 字符
constexpr int kDefaultSwipeMs = 200;

std::uint32_t read_le32(const ByteBuf& b, size_t off) {
    return static_cast<std::uint32_t>(b[off]) | (static_cast<std::uint32_t>(b[off + 1]) << 8) |
           (static_cast<std::uint32_t>(b[off + 2]) << 16) |
           (static_cast<std::uint32_t>(b[off + 3]) << 24);
}

std::int32_t read_le_i32(const ByteBuf& b, size_t off) {
    return static_cast<std::int32_t>(read_le32(b, off));
}

std::string peer_name_of(const TrustRecord& record) {
    return record.name.empty() ? record.device_id : record.name;
}

// 把输入事件写入控制指令（token 之后的部分）。
void write_input(ByteWriter& w, const ScreenInput& in) {
    w.u8(static_cast<std::uint8_t>(in.type));
    switch (in.type) {
        case ScreenInputType::Tap:
            w.u32(static_cast<std::uint32_t>(in.x1));
            w.u32(static_cast<std::uint32_t>(in.y1));
            break;
        case ScreenInputType::Swipe:
            w.u32(static_cast<std::uint32_t>(in.x1));
            w.u32(static_cast<std::uint32_t>(in.y1));
            w.u32(static_cast<std::uint32_t>(in.x2));
            w.u32(static_cast<std::uint32_t>(in.y2));
            w.u32(static_cast<std::uint32_t>(in.duration_ms > 0 ? in.duration_ms
                                                               : kDefaultSwipeMs));
            break;
        case ScreenInputType::Key:
            w.u8(static_cast<std::uint8_t>(in.key));
            break;
    }
}

// 读取控制指令；格式非法时返回 false。
bool read_input(ByteReader& r, ScreenInput& out) {
    const auto raw = r.u8();
    if (!r.ok()) return false;
    switch (static_cast<ScreenInputType>(raw)) {
        case ScreenInputType::Tap:
            out.type = ScreenInputType::Tap;
            out.x1 = static_cast<int>(r.u32());
            out.y1 = static_cast<int>(r.u32());
            break;
        case ScreenInputType::Swipe:
            out.type = ScreenInputType::Swipe;
            out.x1 = static_cast<int>(r.u32());
            out.y1 = static_cast<int>(r.u32());
            out.x2 = static_cast<int>(r.u32());
            out.y2 = static_cast<int>(r.u32());
            out.duration_ms = static_cast<int>(r.u32());
            break;
        case ScreenInputType::Key: {
            out.type = ScreenInputType::Key;
            const auto key = static_cast<ScreenKey>(r.u8());
            if (key != ScreenKey::Back && key != ScreenKey::Home && key != ScreenKey::Recents)
                return false;
            out.key = key;
            break;
        }
        default:
            return false;
    }
    return r.ok();
}

} // namespace

void ScreenInputQueue::push(const ScreenInput& input) {
    std::lock_guard<std::mutex> lk(mu_);
    if (items_.size() >= kMaxQueuedInputs) items_.pop_front();
    items_.push_back(input);
}

bool ScreenInputQueue::pop(ScreenInput& out) {
    std::lock_guard<std::mutex> lk(mu_);
    if (items_.empty()) return false;
    out = items_.front();
    items_.pop_front();
    return true;
}

void ScreenInputQueue::clear() {
    std::lock_guard<std::mutex> lk(mu_);
    items_.clear();
}

bool ScreenInputQueue::empty() const {
    std::lock_guard<std::mutex> lk(mu_);
    return items_.empty();
}

std::size_t ScreenInputQueue::size() const {
    std::lock_guard<std::mutex> lk(mu_);
    return items_.size();
}

ScreenService::ScreenService(const Identity& identity, TrustStore& trust, EventBus& events,
                             IDialer& dialer, const ScreenConfig& config,
                             std::shared_ptr<IScreenCapture> capture,
                             std::shared_ptr<IScreenInput> input, int connect_timeout_ms)
    : identity_(identity),
      trust_(trust),
      events_(events),
      dialer_(dialer),
      config_(config),
      capture_(std::move(capture)),
      input_(std::move(input)),
      connect_timeout_ms_(connect_timeout_ms) {}

ScreenService::~ScreenService() { stop(); }

void ScreenService::start() { running_.store(true); }
void ScreenService::stop() { running_.store(false); }

void ScreenService::set_confirm_handler(
    std::function<bool(const std::string&, const std::string&)> fn) {
    confirm_ = std::move(fn);
}

bool ScreenService::dib_info(const ByteBuf& dib, int& width, int& height) {
    if (dib.size() < 12) return false;
    const std::uint32_t header_size = read_le32(dib, 0);
    if (header_size < 12 || 4u + header_size > dib.size()) return false;
    const std::int32_t w = read_le_i32(dib, 4);
    const std::int32_t h = read_le_i32(dib, 8);
    if (w <= 0 || h == 0) return false;
    width = static_cast<int>(w);
    height = h < 0 ? -h : h;
    return true;
}

ByteBuf ScreenService::dib_to_bmp(const ByteBuf& dib) {
    if (dib.size() < 14) return {};
    const std::uint32_t header_size = read_le32(dib, 0);
    if (header_size < 12 || 4u + header_size > dib.size()) return {};
    const std::uint64_t file_size = 14ull + dib.size();
    if (file_size > 0xFFFFFFFFull) return {};

    ByteBuf bmp(static_cast<std::size_t>(file_size), 0);
    bmp[0] = 'B';
    bmp[1] = 'M';
    const auto fs = static_cast<std::uint32_t>(file_size);
    bmp[2] = static_cast<std::uint8_t>(fs & 0xff);
    bmp[3] = static_cast<std::uint8_t>((fs >> 8) & 0xff);
    bmp[4] = static_cast<std::uint8_t>((fs >> 16) & 0xff);
    bmp[5] = static_cast<std::uint8_t>((fs >> 24) & 0xff);
    // bmp[6..9] 为保留字段，已初始化为 0
    const auto offset = static_cast<std::uint32_t>(14u + header_size);
    bmp[10] = static_cast<std::uint8_t>(offset & 0xff);
    bmp[11] = static_cast<std::uint8_t>((offset >> 8) & 0xff);
    bmp[12] = static_cast<std::uint8_t>((offset >> 16) & 0xff);
    bmp[13] = static_cast<std::uint8_t>((offset >> 24) & 0xff);
    std::copy(dib.begin(), dib.end(), bmp.begin() + 14);
    return bmp;
}

void ScreenService::serve(std::unique_ptr<ByteStream> stream) {
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
        w.u8(kFailMsg);
        w.str(reason);
        seal_send(*stream, keys, seq, w.buf());
        events_.publish(ScreenFailed{peer, name, reason});
    };

    if (!record || (record->permissions & kPermScreenShare) == 0) {
        send_fail("未授予屏幕共享权限");
        stream->close();
        return;
    }
    if (!config_.enabled) {
        send_fail("对端已停用屏幕共享");
        stream->close();
        return;
    }
    if (!running_.load()) {
        send_fail("屏幕共享服务未启动");
        stream->close();
        return;
    }

    auto msg = seal_recv(*stream, keys, seq, kFrameSlack);
    if (!msg) {
        stream->close();
        return;
    }
    std::string mode;
    {
        ByteReader r(*msg);
        if (r.u8() != kView) {
            send_fail("协议错误");
            stream->close();
            return;
        }
        mode = r.str();
        if (!r.ok()) {
            send_fail("协议错误");
            stream->close();
            return;
        }
    }
    if (mode != "view" && mode != "control") {
        send_fail("不支持的屏幕会话模式");
        stream->close();
        return;
    }
    const bool control = mode == "control";
    if (control && !input_) {
        send_fail("本机未启用屏幕控制（Android 需开启无障碍服务）");
        stream->close();
        return;
    }
    // 自动接受（无人工确认）时，控制模式必须已显式授予“屏幕控制”权限。
    if (control && auto_accept_ && (record->permissions & kPermScreenControl) == 0) {
        send_fail("未授予屏幕控制权限");
        stream->close();
        return;
    }

    if (!auto_accept_ && config_.require_confirm_each_session &&
        (!confirm_ || !confirm_(name, control ? "control" : "view"))) {
        send_fail("用户拒绝或未配置确认处理器");
        stream->close();
        return;
    }

    std::string token = to_hex(crypto::random_bytes(kTokenBytes));
    {
        ByteWriter w;
        w.u8(kAccept);
        w.str(token);
        if (auto r = seal_send(*stream, keys, seq, w.buf()); !r) {
            stream->close();
            return;
        }
    }

    events_.publish(ScreenSessionStarted{peer, name});

    std::uint64_t frames = 0;
    std::uint64_t inputs = 0;
    std::string reason = "对端断开";
    while (running_.load()) {
        auto req = seal_recv(*stream, keys, seq, kFrameSlack);
        if (!req) break;

        std::uint8_t type = 0;
        ScreenInput input_event;
        {
            ByteReader r(*req);
            type = r.u8();
            const std::string req_token = r.str();
            if (!r.ok() || req_token != token) {
                reason = "会话令牌无效";
                send_fail(reason);
                break;
            }
            if (type == kInput && !read_input(r, input_event)) {
                reason = "控制指令格式非法";
                send_fail(reason);
                break;
            }
        }
        if (type == kStop) {
            reason = "对端停止";
            break;
        }
        if (type == kInput) {
            if (!control) {
                reason = "会话模式不支持输入";
                send_fail(reason);
                break;
            }
            if (auto r = input_->inject(input_event); !r) {
                reason = "输入注入失败: " + r.error;
                send_fail(reason);
                break;
            }
            ++inputs;
            continue;
        }
        if (type != kFrameRequest) {
            reason = "协议错误";
            send_fail(reason);
            break;
        }

        auto frame = capture_->capture();
        if (!frame || frame->empty()) {
            reason = "屏幕采集失败";
            send_fail(reason);
            break;
        }
        if (config_.max_bytes > 0 &&
            frame->data.size() > static_cast<std::size_t>(config_.max_bytes)) {
            reason = "单帧超过 max_bytes 限制";
            send_fail(reason);
            break;
        }

        ByteWriter w;
        w.u8(kFrame);
        w.u64(frames);
        w.u32(static_cast<std::uint32_t>(frame->width));
        w.u32(static_cast<std::uint32_t>(frame->height));
        w.str(frame->mime);
        w.bytes(frame->data);
        w.u64(static_cast<std::uint64_t>(now_secs()));
        if (auto r = seal_send(*stream, keys, seq, w.buf()); !r) {
            reason = "发送帧失败";
            break;
        }
        ++frames;
    }

    std::fill(token.begin(), token.end(), '\0');
    events_.publish(ScreenSessionEnded{peer, name, reason, frames, inputs});
    stream->close();
}

Expected<ScreenViewResult> ScreenService::view(const DeviceId& peer,
                                              const std::string& output_dir,
                                              int duration_ms, ScreenInputQueue* input) {
    const auto record = trust_.get(peer);
    if (!record) return Expected<ScreenViewResult>::fail("目标设备不在信任库中");
    if ((record->permissions & kPermScreenShare) == 0)
        return Expected<ScreenViewResult>::fail("未授予屏幕共享权限");
    if (record->last_ip.empty())
        return Expected<ScreenViewResult>::fail("尚未学习到对端 IP，请先完成心跳");
    if (!capture_) return Expected<ScreenViewResult>::fail("屏幕采集后端未初始化");

    const int port = record->screen_port > 0 ? record->screen_port : 45715;
    auto dialed = dialer_.dial(record->last_ip, port, connect_timeout_ms_);
    if (!dialed) {
        events_.publish(ScreenFailed{peer, record->name, "连接失败: " + dialed.error});
        return Expected<ScreenViewResult>::fail("连接对端失败: " + dialed.error);
    }
    std::unique_ptr<ByteStream> stream = std::move(*dialed);

    auto sts = sts_client(*stream, identity_, trust_, peer);
    if (!sts) {
        stream->close();
        events_.publish(ScreenFailed{peer, record->name, sts.error});
        return Expected<ScreenViewResult>::fail(sts.error);
    }
    SessionKeys keys = std::move(sts->keys);
    std::uint64_t seq = 0;

    {
        ByteWriter w;
        w.u8(kView);
        w.str(input ? "control" : "view");
        if (auto r = seal_send(*stream, keys, seq, w.buf()); !r) {
            stream->close();
            return Expected<ScreenViewResult>::fail(r.error);
        }
    }

    auto accepted = seal_recv(*stream, keys, seq, kFrameSlack);
    if (!accepted) {
        stream->close();
        return Expected<ScreenViewResult>::fail("等待对端接受会话失败");
    }
    std::string token;
    {
        ByteReader r(*accepted);
        const std::uint8_t type = r.u8();
        const std::string note = r.str();
        if (type == kFailMsg) {
            const std::string why = note.empty() ? "对端拒绝屏幕查看" : note;
            stream->close();
            events_.publish(ScreenFailed{peer, record->name, why});
            return Expected<ScreenViewResult>::fail(why);
        }
        if (type != kAccept || !r.ok()) {
            stream->close();
            return Expected<ScreenViewResult>::fail("对端应答类型错误");
        }
        token = note;
    }

    if (auto r = pal::ensure_dir(output_dir); !r) {
        stream->close();
        return Expected<ScreenViewResult>::fail("创建输出目录失败: " + r.error);
    }
    const std::string bmp_path = pal::join_path(output_dir, "latest.bmp");

    ScreenViewResult stats;
    const int fps = std::max(1, config_.max_fps);
    const auto interval = std::chrono::milliseconds(1000 / fps);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(duration_ms);

    while (std::chrono::steady_clock::now() < deadline) {
        if (input) {
            ScreenInput event;
            while (input->pop(event)) {
                ByteWriter w;
                w.u8(kInput);
                w.str(token);
                write_input(w, event);
                if (auto r = seal_send(*stream, keys, seq, w.buf()); !r) {
                    stream->close();
                    return Expected<ScreenViewResult>::fail(r.error);
                }
                ++stats.inputs;
            }
        }
        {
            ByteWriter w;
            w.u8(kFrameRequest);
            w.str(token);
            if (auto r = seal_send(*stream, keys, seq, w.buf()); !r) {
                stream->close();
                return Expected<ScreenViewResult>::fail(r.error);
            }
        }

        auto resp = seal_recv(*stream, keys, seq,
                              static_cast<size_t>(config_.max_bytes) + kFrameSlack);
        if (!resp) {
            stream->close();
            events_.publish(ScreenFailed{peer, record->name, "接收帧失败"});
            return Expected<ScreenViewResult>::fail("接收帧失败");
        }

        std::uint8_t type = 0;
        std::uint64_t frame_seq = 0;
        int width = 0;
        int height = 0;
        std::string mime;
        ByteBuf data;
        {
            ByteReader r(*resp);
            type = r.u8();
            if (type == kFailMsg) {
                const std::string why = r.str();
                stream->close();
                events_.publish(ScreenFailed{peer, record->name, why});
                return Expected<ScreenViewResult>::fail(why.empty() ? "对端失败" : why);
            }
            if (type != kFrame) {
                stream->close();
                return Expected<ScreenViewResult>::fail("对端应答类型错误");
            }
            frame_seq = r.u64();
            width = static_cast<int>(r.u32());
            height = static_cast<int>(r.u32());
            mime = r.str();
            data = r.bytes();
            (void)r.u64(); // ts
            if (!r.ok() || mime != "image/dib" || data.empty() || width <= 0 || height <= 0) {
                stream->close();
                return Expected<ScreenViewResult>::fail("帧格式非法");
            }
            if (config_.max_bytes > 0 &&
                data.size() > static_cast<std::size_t>(config_.max_bytes)) {
                stream->close();
                return Expected<ScreenViewResult>::fail("帧超过 max_bytes 限制");
            }
        }

        const ByteBuf bmp = dib_to_bmp(data);
        if (bmp.empty()) {
            stream->close();
            return Expected<ScreenViewResult>::fail("DIB 转 BMP 失败");
        }
        if (auto w = pal::write_file_atomic(bmp_path, bmp); !w) {
            stream->close();
            return Expected<ScreenViewResult>::fail("写入帧文件失败: " + w.error);
        }

        ++stats.frames;
        stats.bytes += data.size();
        stats.last_frame_path = bmp_path;
        events_.publish(ScreenFrameReceived{peer, frame_seq, width, height,
                                            static_cast<std::uint64_t>(data.size())});

        std::this_thread::sleep_for(interval);
    }

    {
        ByteWriter w;
        w.u8(kStop);
        w.str(token);
        seal_send(*stream, keys, seq, w.buf());
    }
    std::fill(token.begin(), token.end(), '\0');
    stream->close();
    return Expected<ScreenViewResult>::success(std::move(stats));
}

} // namespace lb
