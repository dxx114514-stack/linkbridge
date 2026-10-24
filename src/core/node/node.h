#pragma once
// 节点：把身份、信任库、配对、心跳、监听器装配为一个可用实例。
// 这是 UI/CLI 唯一依赖的门面；核心模块之间只通过 EventBus 通信。
#include "core/clipboard/clipboard.h"
#include "core/common/event_bus.h"
#include "core/config/config.h"
#include "core/heartbeat/heartbeat.h"
#include "core/identity/identity.h"
#include "core/pairing/pairing.h"
#include "core/screen/screen.h"
#include "core/sync/sync.h"
#include "core/transfer/transfer.h"
#include "core/trust/trust_store.h"
#include "net/tcp.h"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace lb {

struct NodeOptions {
    std::string data_dir;
    AppConfig config;
    std::string device_name;
    // 测试/UI 可注入剪贴板与屏幕采集后端；为空时使用系统实现
    std::shared_ptr<IClipboardBackend> clipboard_backend;
    std::shared_ptr<IScreenCapture> screen_capture;
};

class Node {
public:
    static Expected<std::unique_ptr<Node>> create(const NodeOptions& options);
    ~Node();

    Identity& identity() { return *identity_; }
    TrustStore& trust() { return *trust_store_; }
    EventBus& events() { return events_; }
    PairingService& pairing() { return *pairing_; }
    HeartbeatService& heartbeat() { return *heartbeat_; }
    TransferService& transfer() { return *transfer_; }
    ClipboardService& clipboard() { return *clipboard_; }
    ScreenService& screen() { return *screen_; }
    SyncService& sync() { return *sync_; }
    const AppConfig& config() const { return config_; }

    int pair_port() const { return pair_listen_ ? pair_listen_->port() : config_.network.pair_port; }
    int heartbeat_port() const {
        return heartbeat_listen_ ? heartbeat_listen_->port() : config_.network.heartbeat_port;
    }
    int transfer_port() const {
        return transfer_listen_ ? transfer_listen_->port() : config_.network.transfer_port;
    }
    int clipboard_port() const {
        return clipboard_listen_ ? clipboard_listen_->port() : config_.network.clipboard_port;
    }
    int screen_port() const {
        return screen_listen_ ? screen_listen_->port() : config_.network.screen_port;
    }
    int sync_port() const {
        return sync_listen_ ? sync_listen_->port() : config_.network.sync_port;
    }

    // 启动监听与心跳调度
    Result start();
    void stop();
    bool running() const { return running_.load(); }

    std::vector<DeviceStatus> devices() const;
    std::string identity_summary() const;

private:
    Node() = default;
    enum class Channel { Pairing, Heartbeat, Transfer, Clipboard, Screen, Sync };
    void accept_loop(Channel channel);

    std::string data_dir_;
    AppConfig config_;

    std::unique_ptr<Identity> identity_;
    std::unique_ptr<TrustStore> trust_store_;
    std::unique_ptr<net::TcpDialer> dialer_;
    std::unique_ptr<PairingService> pairing_;
    std::unique_ptr<HeartbeatService> heartbeat_;
    std::unique_ptr<TransferService> transfer_;
    std::unique_ptr<ClipboardService> clipboard_;
    std::unique_ptr<ScreenService> screen_;
    std::unique_ptr<SyncService> sync_;
    EventBus events_;

    std::unique_ptr<net::Listener> pair_listen_;
    std::unique_ptr<net::Listener> heartbeat_listen_;
    std::unique_ptr<net::Listener> transfer_listen_;
    std::unique_ptr<net::Listener> clipboard_listen_;
    std::unique_ptr<net::Listener> screen_listen_;
    std::unique_ptr<net::Listener> sync_listen_;

    std::atomic<bool> running_{false};
    std::thread pair_thread_;
    std::thread heartbeat_thread_;
    std::thread transfer_thread_;
    std::thread clipboard_thread_;
    std::thread screen_thread_;
    std::thread sync_thread_;

    std::mutex handlers_mu_;
    std::condition_variable handlers_cv_;
    int active_handlers_ = 0;
};

} // namespace lb
