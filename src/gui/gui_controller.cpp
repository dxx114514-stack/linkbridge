#include "gui/gui_controller.h"

#include "core/config/config.h"
#include "core/platform/platform.h"
#include "core/sync/version_store.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <sstream>

namespace lb::gui {
namespace {

std::string quote(const std::string& s) {
    std::string out = "\"";
    for (const char c : s) {
        if (c == '\\' || c == '"') out.push_back('\\');
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

// 生成与 core 默认文本一致的 TOML（GUI 设置页保存使用）
std::string config_to_text(const AppConfig& cfg) {
    std::ostringstream o;
    o << "[identity]\n# 设备名留空则使用系统主机名\ndevice_name = " << quote(cfg.device_name)
      << "\n\n";
    o << "[network]\npair_port = " << cfg.network.pair_port
      << "\nheartbeat_port = " << cfg.network.heartbeat_port
      << "\nssh_port = " << cfg.network.ssh_port
      << "\ntransfer_port = " << cfg.network.transfer_port
      << "\nclipboard_port = " << cfg.network.clipboard_port
      << "\nscreen_port = " << cfg.network.screen_port
      << "\nsync_port = " << cfg.network.sync_port << "\nbind_addr = " << quote(cfg.network.bind_addr)
      << "\nconnect_timeout_ms = " << cfg.network.connect_timeout_ms << "\n\n";
    o << "[heartbeat]\ninterval_sec = " << cfg.heartbeat.interval_sec
      << "\nmax_interval_sec = " << cfg.heartbeat.max_interval_sec
      << "\nmax_failures = " << cfg.heartbeat.max_failures
      << "\nadaptive = " << (cfg.heartbeat.adaptive ? "true" : "false")
      << "\noffline_probe_sec = " << cfg.heartbeat.offline_probe_sec << "\n\n";
    o << "[transfer]\nchunk_size = " << cfg.transfer.chunk_size
      << "\nmax_concurrent_tasks = " << cfg.transfer.max_concurrent_tasks
      << "\nrate_limit_kbps = " << cfg.transfer.rate_limit_kbps << "\n\n";
    o << "[clipboard]\nenabled = " << (cfg.clipboard.enabled ? "true" : "false")
      << "\nmode = " << quote(cfg.clipboard.mode) << "\nmax_bytes = " << cfg.clipboard.max_bytes
      << "\n\n";
    o << "[screen]\nenabled = " << (cfg.screen.enabled ? "true" : "false")
      << "\nrequire_confirm_each_session = " << (cfg.screen.require_confirm_each_session ? "true" : "false")
      << "\nmax_fps = " << cfg.screen.max_fps << "\nmax_bytes = " << cfg.screen.max_bytes << "\n\n";
    const auto& s = cfg.sync;
    o << "[sync]\nenabled = " << (s.enabled ? "true" : "false") << "\nfolder = " << quote(s.folder)
      << "\nmode = " << quote(s.mode) << "\nignore = " << quote(s.ignore)
      << "\nmax_file_size_mb = " << s.max_file_size_mb
      << "\nrescan_interval_sec = " << s.rescan_interval_sec
      << "\ndefault_conflict_policy = " << quote(s.default_conflict_policy)
      << "\nmax_concurrent_files = " << s.max_concurrent_files
      << "\ntrash_enabled = " << (s.trash_enabled ? "true" : "false")
      << "\nversion_keep_count = " << s.version_keep_count
      << "\nversion_keep_days = " << s.version_keep_days << "\ntrash_max_mb = " << s.trash_max_mb
      << "\nrate_limit_kbps = " << s.rate_limit_kbps << "\n";
    return o.str();
}

std::string peer_label(const TrustRecord& r) { return r.name + " (" + r.device_id + ")"; }

} // namespace

Controller::Controller() = default;

Controller::~Controller() {
    closing_ = true;
    detach_all();
    if (node_) node_->stop();
    wait_all_tasks();
}

void Controller::set_ui_post(std::function<void(std::function<void()>)> post) {
    post_ = std::move(post);
}

void Controller::set_ask_handler(std::function<bool(const std::string&, const std::string&)> fn) {
    ask_fn_ = std::move(fn);
}

void Controller::set_event_sink(UiEventFn fn) { event_sink_ = std::move(fn); }

bool Controller::initialize(const std::string& data_dir, const std::string& config_path,
                            const std::string& name, std::string* error) {
    data_dir_ = data_dir.empty() ? pal::data_dir() : data_dir;
    config_path_ = config_path.empty() ? pal::join_path(data_dir_, "linkbridge.toml") : config_path;

    bool created = false;
    auto cfg = load_config(config_path_, &created);
    if (!cfg) {
        if (error) *error = cfg.error;
        return false;
    }
    if (!name.empty()) cfg->device_name = name;
    device_name_ = cfg->device_name;

    NodeOptions options;
    options.data_dir = data_dir_;
    options.config = *cfg;
    options.device_name = cfg->device_name;
    auto result = Node::create(options);
    if (!result) {
        if (error) *error = result.error;
        return false;
    }
    node_ = std::move(*result);
    attach_subscriptions();
    install_confirm_handlers();
    return true;
}

void Controller::detach_all() {
    subs_.clear();
}

bool Controller::start(std::string* error) {
    if (!node_) {
        if (error) *error = "节点未初始化";
        return false;
    }
    if (node_->running()) return true;
    const Result r = node_->start();
    if (!r) {
        if (error) *error = r.error;
        return false;
    }
    return true;
}

void Controller::stop() {
    if (node_) node_->stop();
}

bool Controller::restart(std::string* error) {
    const std::string dir = data_dir_;
    const std::string cfg = config_path_;
    const std::string name = device_name_;
    detach_all();
    if (node_) node_->stop();
    node_.reset();
    if (!initialize(dir, cfg, name, error)) return false;
    return start(error);
}

bool Controller::running() const { return node_ && node_->running(); }

UiSnapshot Controller::snapshot() const {
    UiSnapshot s;
    s.data_dir = data_dir_;
    s.config_path = config_path_;
    if (!node_) {
        s.last_error = "节点未初始化";
        return s;
    }
    const auto& cfg = node_->config();
    s.running = node_->running();
    s.device_name = node_->identity().name();
    s.device_id = node_->identity().device_id();
    s.fingerprint = node_->identity().fingerprint();
    s.platform = platform_name(node_->identity().platform());
    s.inbox_dir = node_->transfer().inbox_dir();
    s.pair_port = node_->pair_port();
    s.heartbeat_port = node_->heartbeat_port();
    s.transfer_port = node_->transfer_port();
    s.clipboard_port = node_->clipboard_port();
    s.screen_port = node_->screen_port();
    s.sync_port = node_->sync_port();
    s.trusted = node_->trust().size();
    for (const auto& d : node_->devices())
        if (d.presence == Presence::Online) ++s.online;
    s.probes_ok = node_->heartbeat().probes_ok();
    s.probes_failed = node_->heartbeat().probes_failed();
    s.clipboard_enabled = cfg.clipboard.enabled;
    s.clipboard_mode = cfg.clipboard.mode;
    s.screen_enabled = cfg.screen.enabled;
    s.screen_fps = cfg.screen.max_fps;
    s.sync_enabled = cfg.sync.enabled;
    s.sync_folder = cfg.sync.folder;
    s.sync_mode = cfg.sync.mode;
    s.sync_index = node_->sync().index_size();
    s.sync_conflicts = node_->sync().conflicts().size();
    return s;
}

std::vector<DeviceStatus> Controller::devices() const {
    return node_ ? node_->devices() : std::vector<DeviceStatus>{};
}

std::vector<TrustRecord> Controller::trust() const {
    return node_ ? node_->trust().all() : std::vector<TrustRecord>{};
}

const AppConfig& Controller::config() const {
    static const AppConfig kFallback{};
    return node_ ? node_->config() : kFallback;
}

bool Controller::save_config(const AppConfig& cfg, std::string* error) {
    const Result v = validate_config(cfg);
    if (!v) {
        if (error) *error = v.error;
        return false;
    }
    const std::string text = config_to_text(cfg);
    const ByteBuf data(text.begin(), text.end());
    const Result w = pal::write_file_atomic(config_path_, data);
    if (!w) {
        if (error) *error = w.error;
        return false;
    }
    return true;
}

void Controller::set_auto_accept(bool value) {
    auto_accept_ = value;
    if (!node_) return;
    if (value) {
        node_->pairing().set_auto_accept(true);
        node_->clipboard().set_auto_accept(true);
        node_->screen().set_auto_accept(true);
    } else {
        install_confirm_handlers();
    }
}

void Controller::emit(UiEvent ev) {
    if (!post_) return;
    post_([this, ev = std::move(ev)] {
        if (event_sink_) event_sink_(ev);
    });
}

void Controller::notify_done(DoneFn done, bool ok, const std::string& message) {
    if (!done) return;
    if (!post_) {
        done(ok, message);
        return;
    }
    post_([done = std::move(done), ok, message] { done(ok, message); });
}

void Controller::run_async(std::function<void()> work) {
    std::lock_guard<std::mutex> lk(tasks_mu_);
    tasks_.erase(std::remove_if(tasks_.begin(), tasks_.end(),
                               [](std::future<void>& f) {
                                   return f.valid() &&
                                          f.wait_for(std::chrono::seconds(0)) ==
                                              std::future_status::ready;
                               }),
                 tasks_.end());
    tasks_.push_back(std::async(std::launch::async, std::move(work)));
}

void Controller::wait_all_tasks() {
    std::vector<std::future<void>> local;
    {
        std::lock_guard<std::mutex> lk(tasks_mu_);
        local = std::move(tasks_);
    }
    for (auto& f : local)
        if (f.valid()) f.wait();
}

bool Controller::ask_ui(const std::string& title, const std::string& message) {
    if (closing_ || !post_ || !ask_fn_) return false;
    auto promise = std::make_shared<std::promise<bool>>();
    auto future = promise->get_future();
    post_([this, title, message, promise] {
        bool ok = false;
        try {
            ok = ask_fn_ ? ask_fn_(title, message) : false;
        } catch (...) {
            ok = false;
        }
        promise->set_value(ok);
    });
    const auto status = future.wait_for(std::chrono::seconds(120));
    if (status != std::future_status::ready) return false;
    return future.get();
}

void Controller::install_confirm_handlers() {
    if (!node_) return;
    node_->pairing().set_auto_accept(auto_accept_);
    node_->clipboard().set_auto_accept(auto_accept_);
    node_->screen().set_auto_accept(auto_accept_);
    if (auto_accept_) {
        node_->pairing().set_confirm_handler({});
        node_->clipboard().set_confirm_handler({});
        node_->screen().set_confirm_handler({});
        return;
    }
    node_->pairing().set_confirm_handler([this](const PeerInfo& peer, const std::string& fp) {
        std::ostringstream o;
        o << "设备名：" << peer.name << "\n设备 ID：" << peer.device_id << "\n平台：" << peer.platform
          << "\n来源 IP：" << peer.ip << "\n\n指纹：" << fp
          << "\n\n请与对方屏幕上的指纹逐位比对，一致才接受配对。";
        return ask_ui("配对请求", o.str());
    });
    node_->clipboard().set_confirm_handler(
        [this](const ClipboardContent& content, const std::string& from) {
            std::string preview;
            if (content.mime == "text/plain") {
                preview = std::string(content.data.begin(), content.data.end());
                if (preview.size() > 300) preview = preview.substr(0, 300) + "...";
            } else {
                preview = "[图片]";
            }
            std::ostringstream o;
            o << "来自 " << from << " 的" << (content.mime == "text/plain" ? "文本" : "图片")
              << "（" << content.data.size() << " 字节）\n\n预览：\n" << preview
              << "\n\n接受并覆盖本地剪贴板？";
            return ask_ui("剪贴板同步", o.str());
        });
    node_->screen().set_confirm_handler([this](const std::string& from, const std::string& mode) {
        std::ostringstream o;
        o << "设备：" << from << "\n模式：" << (mode == "view" ? "只读查看" : mode)
          << "\n\n接受对方查看你的屏幕？";
        return ask_ui("屏幕共享请求", o.str());
    });
}

void Controller::attach_subscriptions() {
    if (!node_) return;
    subs_.push_back(node_->events().subscribe<PairingCompleted>([this](const PairingCompleted& ev) {
        emit({"pairing_ok", ev.peer.name, "指纹 " + ev.fingerprint, 0, 0, ev.initiator});
    }));
    subs_.push_back(node_->events().subscribe<PairingStateChanged>(
        [this](const PairingStateChanged& ev) {
            if (ev.state == PairState::Failed || ev.state == PairState::Done)
                emit({"pairing_state", pair_state_name(ev.state), ev.message, 0, 0, false});
        }));
    subs_.push_back(node_->events().subscribe<DeviceOnline>([this](const DeviceOnline& ev) {
        emit({"device_online", ev.name, ev.device_id, 0, 0, false});
    }));
    subs_.push_back(node_->events().subscribe<DeviceOffline>([this](const DeviceOffline& ev) {
        emit({"device_offline", ev.name, ev.device_id, 0, 0, false});
    }));
    subs_.push_back(node_->events().subscribe<TransferProgress>([this](const TransferProgress& ev) {
        emit({"transfer_progress", ev.file_name, ev.task_id, ev.transferred, ev.total,
              ev.outgoing});
    }));
    subs_.push_back(node_->events().subscribe<TransferCompleted>([this](const TransferCompleted& ev) {
        emit({"transfer_done", ev.file_name, ev.path, ev.size, 0, ev.outgoing});
    }));
    subs_.push_back(node_->events().subscribe<TransferFailed>([this](const TransferFailed& ev) {
        emit({"transfer_failed", ev.file_name, ev.reason, 0, 0, ev.outgoing});
    }));
    subs_.push_back(node_->events().subscribe<ClipboardReceived>([this](const ClipboardReceived& ev) {
        emit({"clipboard_in", ev.from_name,
              ev.applied ? "已写入本地剪贴板" : ("未应用：" + ev.reason), ev.size, 0, false});
    }));
    subs_.push_back(node_->events().subscribe<ClipboardSent>([this](const ClipboardSent& ev) {
        emit({"clipboard_out", ev.to_name, "已推送", ev.size, 0, true});
    }));
    subs_.push_back(node_->events().subscribe<ClipboardFailed>([this](const ClipboardFailed& ev) {
        emit({"clipboard_fail", ev.peer_name, ev.reason, 0, 0, false});
    }));
    subs_.push_back(node_->events().subscribe<ClipboardSkipped>([this](const ClipboardSkipped& ev) {
        emit({"clipboard_skipped", ev.mime, ev.reason, ev.size, 0, false});
    }));
    subs_.push_back(node_->events().subscribe<ScreenSessionStarted>([this](const ScreenSessionStarted& ev) {
        emit({"screen_started", ev.peer_name, ev.peer, 0, 0, false});
    }));
    subs_.push_back(node_->events().subscribe<ScreenFrameReceived>([this](const ScreenFrameReceived& ev) {
        emit({"screen_frame", ev.peer, std::to_string(ev.width) + "x" + std::to_string(ev.height),
              ev.size, ev.seq, false});
    }));
    subs_.push_back(node_->events().subscribe<ScreenSessionEnded>([this](const ScreenSessionEnded& ev) {
        emit({"screen_ended", ev.peer_name, ev.reason, ev.frames, 0, false});
    }));
    subs_.push_back(node_->events().subscribe<ScreenFailed>([this](const ScreenFailed& ev) {
        emit({"screen_failed", ev.peer_name, ev.reason, 0, 0, false});
    }));
    subs_.push_back(node_->events().subscribe<SyncStarted>([this](const SyncStarted& ev) {
        emit({"sync_started", ev.peer_name, ev.peer, 0, 0, false});
    }));
    subs_.push_back(node_->events().subscribe<SyncCompleted>([this](const SyncCompleted& ev) {
        emit({"sync_done", ev.peer_name, "", ev.stats.pushed, ev.stats.pulled, false});
    }));
    subs_.push_back(node_->events().subscribe<SyncFailed>([this](const SyncFailed& ev) {
        emit({"sync_failed", ev.peer_name.empty() ? ev.peer : ev.peer_name, ev.reason, 0, 0, false});
    }));
}

void Controller::pair_with(const std::string& ip, const std::string& code, DoneFn done) {
    if (!node_) return;
    const int port = node_->config().network.pair_port;
    run_async([this, ip, code, port, done] {
        const Result r = node_->pairing().pair_with(ip, port, code);
        notify_done(done, r.success, r.success ? "配对成功，对方已加入受信任设备" : r.error);
    });
}

void Controller::send_file(const std::string& device_id, const std::string& local_path,
                           const std::string& remote_name, DoneFn done) {
    if (!node_) return;
    const int timeout = node_->config().network.connect_timeout_ms;
    run_async([this, device_id, local_path, remote_name, timeout, done] {
        const Result r = node_->transfer().send_file(device_id, local_path, remote_name, timeout);
        notify_done(done, r.success, r.success ? "发送完成" : r.error);
    });
}

void Controller::push_clipboard(const std::string& target, const std::string& text, DoneFn done) {
    if (!node_) return;
    const int timeout = node_->config().network.connect_timeout_ms;
    run_async([this, target, text, timeout, done] {
        ClipboardContent content{"text/plain", ByteBuf(text.begin(), text.end())};
        if (target == "all") {
            int ok = 0;
            for (const auto& r : node_->trust().all()) {
                if ((r.permissions & kPermClipboard) == 0 || r.last_ip.empty()) continue;
                if (node_->clipboard().push_to(r.device_id, content, timeout)) ++ok;
            }
            notify_done(done, ok > 0, "已推送到 " + std::to_string(ok) + " 台设备");
            return;
        }
        const Result r = node_->clipboard().push_to(target, content, timeout);
        notify_done(done, r.success, r.success ? "已推送" : r.error);
    });
}

void Controller::read_clipboard(
    std::function<void(bool, const std::string&, const std::string&, std::uint64_t)> done) {
    if (!node_) return;
    run_async([this, done] {
        auto content = node_->clipboard().read_local();
        bool ok = static_cast<bool>(content);
        std::string mime, text;
        std::uint64_t size = 0;
        if (ok && !content->empty()) {
            mime = content->mime;
            size = content->data.size();
            if (content->mime == "text/plain")
                text.assign(content->data.begin(), content->data.end());
        }
        if (!post_) return;
        post_([done, ok, mime, text, size] { done(ok, mime, text, size); });
    });
}

void Controller::sync_once(const std::string& target, DoneFn done) {
    if (!node_) return;
    if (!node_->config().sync.enabled) {
        notify_done(done, false, "文件夹同步未启用，请到「设置」启用并选择同步文件夹");
        return;
    }
    const int timeout = node_->config().network.connect_timeout_ms;
    run_async([this, target, timeout, done] {
        std::vector<TrustRecord> targets;
        if (target == "all") {
            for (const auto& r : node_->trust().all())
                if ((r.permissions & kPermSync) != 0 && !r.last_ip.empty()) targets.push_back(r);
        } else if (auto peer = node_->trust().get(target)) {
            targets.push_back(*peer);
        }
        if (targets.empty()) {
            notify_done(done, false, "没有可同步的设备（需已配对并学习到 IP，先启动服务）");
            return;
        }
        int ok = 0;
        std::string last_error;
        for (const auto& r : targets) {
            SyncStats stats;
            const Result res = node_->sync().sync_with(r.device_id, timeout, &stats);
            if (!res) {
                last_error = res.error;
                continue;
            }
            ++ok;
            std::ostringstream o;
            o << r.name << "：发出 " << stats.pushed << "、接收 " << stats.pulled << "、冲突 "
              << stats.conflicts << "，共 " << stats.bytes << " 字节";
            emit({"sync_summary", r.name, o.str(), 0, 0, false});
        }
        notify_done(done, ok > 0, ok > 0 ? "同步完成" : ("同步失败：" + last_error));
    });
}

void Controller::view_screen(const std::string& device_id, int seconds,
                             std::shared_ptr<lb::ScreenInputQueue> input, DoneFn done) {
    if (!node_) return;
    const std::string out_dir = pal::join_path(data_dir_, "screen");
    run_async([this, device_id, seconds, out_dir, input, done] {
        auto result = node_->screen().view(device_id, out_dir, seconds * 1000, input.get());
        if (!result) {
            notify_done(done, false, result.error);
            return;
        }
        std::ostringstream o;
        o << "共接收 " << result->frames << " 帧，累计 " << result->bytes << " 字节";
        if (result->inputs > 0) o << "，已发送 " << result->inputs << " 次控制操作";
        o << "，最新帧 " << result->last_frame_path;
        notify_done(done, result->frames > 0, o.str());
    });
}

std::vector<SyncConflictInfo> Controller::conflicts() const {
    return node_ ? node_->sync().conflicts() : std::vector<SyncConflictInfo>{};
}

void Controller::resolve_conflict(const std::string& path, const std::string& action, DoneFn done) {
    if (!node_) return;
    run_async([this, path, action, done] {
        const Result r = node_->sync().resolve_conflict(path, action);
        notify_done(done, r.success, r.success ? "冲突已解决" : r.error);
    });
}

std::vector<VersionInfo> Controller::history(const std::string& path) const {
    return node_ ? node_->sync().history(path) : std::vector<VersionInfo>{};
}

void Controller::restore_version(const std::string& path, const std::string& version_id,
                                 DoneFn done) {
    if (!node_) return;
    run_async([this, path, version_id, done] {
        const Result r = node_->sync().restore_version(path, version_id);
        notify_done(done, r.success, r.success ? "已恢复历史版本" : r.error);
    });
}

PairCode Controller::generate_code(int ttl_sec) {
    if (!node_) return PairCode{};
    return node_->pairing().generate_code(ttl_sec, 3);
}

std::string Controller::pairing_state_text() const {
    return node_ ? pair_state_name(node_->pairing().state()) : "Idle";
}

} // namespace lb::gui
