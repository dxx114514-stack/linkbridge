#pragma once
// 屏幕共享（v0 只读查看）：被查看端采集桌面帧，经 STS 加密通道按请求下发；
// 查看端落盘为 BMP 并由 CLI 呈现。远程控制（键鼠注入）不在本阶段范围。
#include "core/common/event_bus.h"
#include "core/common/stream.h"
#include "core/common/types.h"
#include "core/config/config.h"
#include "core/identity/identity.h"
#include "core/screen/backend.h"
#include "core/trust/trust_store.h"

#include <atomic>
#include <functional>
#include <memory>
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
    std::string last_frame_path;
};

class ScreenService {
public:
    ScreenService(const Identity& identity, TrustStore& trust, EventBus& events,
                  IDialer& dialer, const ScreenConfig& config,
                  std::shared_ptr<IScreenCapture> capture, int connect_timeout_ms);
    ~ScreenService();

    // 被查看端：由监听线程调用，处理一条已建立的入站连接
    void serve(std::unique_ptr<ByteStream> stream);

    // 查看端：阻塞拉取 duration_ms 的桌面帧，期间将最新帧写为 BMP
    Expected<ScreenViewResult> view(const DeviceId& peer, const std::string& output_dir,
                                    int duration_ms);

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
    int connect_timeout_ms_ = 5000;

    std::function<bool(const std::string&, const std::string&)> confirm_;
    bool auto_accept_ = false;
    std::atomic<bool> running_{false};
};

} // namespace lb
