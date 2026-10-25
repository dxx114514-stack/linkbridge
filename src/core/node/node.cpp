#include "core/node/node.h"

#include "core/platform/platform.h"

#include <sstream>

namespace lb {

Expected<std::unique_ptr<Node>> Node::create(const NodeOptions& options) {
    if (auto r = pal::ensure_dir(options.data_dir); !r)
        return Expected<std::unique_ptr<Node>>::fail(r.error);

    auto node = std::unique_ptr<Node>(new Node());
    node->data_dir_ = options.data_dir;
    node->config_ = options.config;
    if (node->config_.device_name.empty()) node->config_.device_name = options.device_name;

    auto identity = Identity::load_or_create(options.data_dir, options.device_name);
    if (!identity) return Expected<std::unique_ptr<Node>>::fail(identity.error);
    node->identity_ = std::make_unique<Identity>(std::move(*identity));

    node->trust_store_ = std::make_unique<TrustStore>(options.data_dir);
    if (auto r = node->trust_store_->load(); !r)
        return Expected<std::unique_ptr<Node>>::fail(r.error);

    node->dialer_ = std::make_unique<net::TcpDialer>();
    node->pairing_ = std::make_unique<PairingService>(*node->identity_, *node->trust_store_,
                                                      node->events_, *node->dialer_);
    node->heartbeat_ = std::make_unique<HeartbeatService>(
        *node->identity_, *node->trust_store_, node->events_, *node->dialer_,
        node->config_.heartbeat, node->config_.network.heartbeat_port,
        node->config_.network.connect_timeout_ms);

    const std::string inbox = pal::join_path(options.data_dir, "inbox");
    if (auto r = pal::ensure_dir(inbox); !r)
        return Expected<std::unique_ptr<Node>>::fail("创建接收目录失败: " + r.error);
    node->transfer_ = std::make_unique<TransferService>(
        *node->identity_, *node->trust_store_, node->events_, *node->dialer_,
        node->config_.transfer, inbox);

    std::shared_ptr<IClipboardBackend> clipboard_backend =
        options.clipboard_backend ? options.clipboard_backend : make_system_clipboard();
    if (!clipboard_backend)
        return Expected<std::unique_ptr<Node>>::fail("剪贴板后端初始化失败");
    node->clipboard_ = std::make_unique<ClipboardService>(
        *node->identity_, *node->trust_store_, node->events_, *node->dialer_,
        node->config_.clipboard, clipboard_backend, node->config_.network.connect_timeout_ms);
    Node* raw = node.get();
    node->clipboard_->set_presence_provider([raw](const DeviceId& id) {
        return raw->heartbeat_->presence(id) == Presence::Online;
    });

    std::shared_ptr<IScreenCapture> screen_capture =
        options.screen_capture ? options.screen_capture : make_system_screen_capture();
    if (!screen_capture)
        return Expected<std::unique_ptr<Node>>::fail("屏幕采集后端初始化失败");
    std::shared_ptr<IScreenInput> screen_input =
        options.screen_input ? options.screen_input : make_system_screen_input();
    node->screen_ = std::make_unique<ScreenService>(
        *node->identity_, *node->trust_store_, node->events_, *node->dialer_,
        node->config_.screen, screen_capture, screen_input,
        node->config_.network.connect_timeout_ms);

    node->sync_ = std::make_unique<SyncService>(*node->identity_, *node->trust_store_,
                                                node->events_, *node->dialer_,
                                                node->config_.sync, options.data_dir);

    return Expected<std::unique_ptr<Node>>::success(std::move(node));
}

Node::~Node() { stop(); }

Result Node::start() {
    if (running_.load()) return Result::ok();

    auto pair_listener = net::Listener::bind(config_.network.bind_addr, config_.network.pair_port);
    if (!pair_listener) return Result::fail("配对端口监听失败: " + pair_listener.error);
    pair_listen_ = std::move(*pair_listener);

    auto hb_listener =
        net::Listener::bind(config_.network.bind_addr, config_.network.heartbeat_port);
    if (!hb_listener) return Result::fail("心跳端口监听失败: " + hb_listener.error);
    heartbeat_listen_ = std::move(*hb_listener);

    auto transfer_listener =
        net::Listener::bind(config_.network.bind_addr, config_.network.transfer_port);
    if (!transfer_listener) return Result::fail("传输端口监听失败: " + transfer_listener.error);
    transfer_listen_ = std::move(*transfer_listener);

    auto clipboard_listener =
        net::Listener::bind(config_.network.bind_addr, config_.network.clipboard_port);
    if (!clipboard_listener) return Result::fail("剪贴板端口监听失败: " + clipboard_listener.error);
    clipboard_listen_ = std::move(*clipboard_listener);

    auto screen_listener =
        net::Listener::bind(config_.network.bind_addr, config_.network.screen_port);
    if (!screen_listener) return Result::fail("屏幕共享端口监听失败: " + screen_listener.error);
    screen_listen_ = std::move(*screen_listener);

    auto sync_listener = net::Listener::bind(config_.network.bind_addr, config_.network.sync_port);
    if (!sync_listener) return Result::fail("文件夹同步端口监听失败: " + sync_listener.error);
    sync_listen_ = std::move(*sync_listener);

    // 监听实际端口可能由系统分配（配置为 0），据此更新通告端口与探测端口
    pairing_->set_advertised_ports(heartbeat_listen_->port(), config_.network.ssh_port,
                                   transfer_listen_->port(), clipboard_listen_->port(),
                                   screen_listen_->port(), sync_listen_->port());
    heartbeat_->set_port(heartbeat_listen_->port());

    running_.store(true);
    pair_thread_ = std::thread([this] { accept_loop(Channel::Pairing); });
    heartbeat_thread_ = std::thread([this] { accept_loop(Channel::Heartbeat); });
    transfer_thread_ = std::thread([this] { accept_loop(Channel::Transfer); });
    clipboard_thread_ = std::thread([this] { accept_loop(Channel::Clipboard); });
    screen_thread_ = std::thread([this] { accept_loop(Channel::Screen); });
    sync_thread_ = std::thread([this] { accept_loop(Channel::Sync); });
    heartbeat_->start();
    clipboard_->start();
    screen_->start();
    sync_->start_auto();
    return Result::ok();
}

void Node::stop() {
    if (!running_.exchange(false)) return;
    if (pair_listen_) pair_listen_->close();
    if (heartbeat_listen_) heartbeat_listen_->close();
    if (transfer_listen_) transfer_listen_->close();
    if (clipboard_listen_) clipboard_listen_->close();
    if (screen_listen_) screen_listen_->close();
    if (sync_listen_) sync_listen_->close();
    if (pair_thread_.joinable()) pair_thread_.join();
    if (heartbeat_thread_.joinable()) heartbeat_thread_.join();
    if (transfer_thread_.joinable()) transfer_thread_.join();
    if (clipboard_thread_.joinable()) clipboard_thread_.join();
    if (screen_thread_.joinable()) screen_thread_.join();
    if (sync_thread_.joinable()) sync_thread_.join();
    heartbeat_->stop();
    clipboard_->stop();
    screen_->stop();
    sync_->stop_auto();

    // 等待处理线程结束（它们会在 socket 超时后自然退出）
    std::unique_lock<std::mutex> lk(handlers_mu_);
    handlers_cv_.wait_for(lk, std::chrono::seconds(20), [this] { return active_handlers_ == 0; });
}

void Node::accept_loop(Channel channel) {
    while (running_.load()) {
        net::Listener* listener = nullptr;
        switch (channel) {
            case Channel::Pairing: listener = pair_listen_.get(); break;
            case Channel::Heartbeat: listener = heartbeat_listen_.get(); break;
            case Channel::Transfer: listener = transfer_listen_.get(); break;
            case Channel::Clipboard: listener = clipboard_listen_.get(); break;
            case Channel::Screen: listener = screen_listen_.get(); break;
            case Channel::Sync: listener = sync_listen_.get(); break;
        }
        if (!listener) return;
        auto accepted = listener->accept(300);
        if (!accepted) {
            if (!running_.load()) return;
            continue;
        }
        if (!*accepted) continue; // 超时，继续轮询

        std::unique_ptr<ByteStream> stream = std::move(*accepted);
        {
            std::lock_guard<std::mutex> lk(handlers_mu_);
            ++active_handlers_;
        }
        std::thread([this, s = std::move(stream), channel]() mutable {
            switch (channel) {
                case Channel::Pairing: pairing_->serve(std::move(s)); break;
                case Channel::Heartbeat: heartbeat_->handle_connection(std::move(s)); break;
                case Channel::Transfer: transfer_->serve(std::move(s)); break;
                case Channel::Clipboard: clipboard_->serve(std::move(s)); break;
                case Channel::Screen: screen_->serve(std::move(s)); break;
                case Channel::Sync: sync_->serve(std::move(s)); break;
            }
            std::lock_guard<std::mutex> lk(handlers_mu_);
            --active_handlers_;
            handlers_cv_.notify_all();
        }).detach();
    }
}

std::vector<DeviceStatus> Node::devices() const { return heartbeat_->statuses(); }

std::string Node::identity_summary() const {
    std::ostringstream os;
    os << "设备名: " << identity_->name() << "\n"
       << "设备 ID: " << identity_->device_id() << "\n"
       << "身份指纹: " << identity_->fingerprint() << "\n"
       << "平台: " << platform_name(identity_->platform()) << "\n"
       << "数据目录: " << data_dir_ << "\n"
       << "配对端口: " << pair_port() << "  心跳端口: " << heartbeat_port()
       << "  传输端口: " << transfer_port()
       << "  剪贴板端口: " << clipboard_port()
       << "  屏幕共享端口: " << screen_port()
       << "  文件夹同步端口: " << sync_port();
    return os.str();
}

} // namespace lb
