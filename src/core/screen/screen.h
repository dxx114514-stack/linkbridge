#pragma once
// 屏幕共享：被查看端采集帧按请求下发；查看端落盘为 BMP 并呈现。
// 控制模式（mode=control）额外支持向被控端注入触摸/按键，被控端需具备输入后端
// （Android 无障碍服务），且每次会话仍需本端确认。
#include "core/common/event_bus.h"
#include "core/common/stream.h"
#include "core/common/types.h"
#include "core/config/config.h"
#include "core/identity/identity.h"
#include "core/screen/backend.h"
#include "core/trust/trust_store.h"

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace lb {

struct ScreenSessionStarted {
    DeviceId peer;
    std::string peer_name;
};

struct ScreenSessionEnded {
    DeviceId peer;
    std::string peer_name;
    std::string reason;
    std::uint64_t frames = 0;
    std::uint64_t inputs = 0;
};

struct ScreenFrameReceived {
    DeviceId peer;
    std::uint64_t seq = 0;
    int width = 0;
    int height = 0;
    std::uint64_t size = 0;
};

struct ScreenFailed {
    DeviceId peer;
    std::string peer_name;
    std::string reason;
};

struct ScreenViewResult {
    std::uint64_t frames = 0;
    std::uint64_t bytes = 0;
    std::uint64_t inputs = 0; // control 模式下已发送的输入事件数
    std::string last_frame_path;
};

// 查看端的输入队列：UI 线程 push，会话线程按帧节奏取出发送。
// 线程安全；队列过长时丢弃最旧事件，避免网络拥塞时无界堆积。
class ScreenInputQueue {
public:
    void push(const ScreenInput& input);
    bool pop(ScreenInput& out);
    void clear();
    bool empty() const;
    std::size_t size() const;

private:
    mutable std::mutex mu_;
    std::deque<ScreenInput> items_;
};

class ScreenService {
public:
    ScreenService(const Identity& identity, TrustStore& trust, EventBus& events,
                  IDialer& dialer, const ScreenConfig& config,
                  std::shared_ptr<IScreenCapture> capture,
                  std::shared_ptr<IScreenInput> input, int connect_timeout_ms);
    ~ScreenService();

    // 被查看端：由监听线程调用，处理一条已建立的入站连接
    void serve(std::unique_ptr<ByteStream> stream);

    // 查看端：阻塞拉取 duration_ms 的桌面帧，期间将最新帧写为 BMP。
    // input 非空时以 control 模式建立会话，并在循环中发送队列内的输入事件。
    Expected<ScreenViewResult> view(const DeviceId& peer, const std::string& output_dir,
                                    int duration_ms, ScreenInputQueue* input = nullptr);

    void start();
    void stop();
    bool running() const { return running_.load(); }

    void set_confirm_handler(
        std::function<bool(const std::string& peer_name, const std::string& mode)> fn);
    void set_auto_accept(bool value) { auto_accept_ = value; }

    // DIB → BMP 文件字节（前置 14 字节文件头）；输入非法时返回空
    static ByteBuf dib_to_bmp(const ByteBuf& dib);
    // 读取 DIB 宽高（biHeight 为负表示 top-down）
    static bool dib_info(const ByteBuf& dib, int& width, int& height);

private:
    const Identity& identity_;
    TrustStore& trust_;
    EventBus& events_;
    IDialer& dialer_;
    ScreenConfig config_;
    std::shared_ptr<IScreenCapture> capture_;
    std::shared_ptr<IScreenInput> input_;
    int connect_timeout_ms_ = 5000;

    std::function<bool(const std::string&, const std::string&)> confirm_;
    bool auto_accept_ = false;
    std::atomic<bool> running_{false};
};

} // namespace lb
