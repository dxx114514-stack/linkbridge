#pragma once
// GUI 控制器：持有 Node 实例，把核心事件与阻塞操作桥接到 UI 线程。
// 面板只依赖本类，不直接触碰核心模块，保证 GUI 与现有业务逻辑解耦。
#include "core/node/node.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace lb::gui {

// 转发给界面的统一事件（由核心事件转换而来）
struct UiEvent {
    std::string type;   // pairing_ok / pairing_fail / device_online / ... / sync_failed
    std::string title;  // 主文案（设备名、文件名等）
    std::string detail; // 附加说明
    std::uint64_t a = 0;
    std::uint64_t b = 0;
    bool outgoing = false;
};

// 界面快照：一次取齐，避免面板多次加锁
struct UiSnapshot {
    bool running = false;
    std::string device_name;
    std::string device_id;
    std::string fingerprint;
    std::string platform;
    std::string data_dir;
    std::string config_path;
    std::string inbox_dir;
    int pair_port = 0;
    int heartbeat_port = 0;
    int transfer_port = 0;
    int clipboard_port = 0;
    int screen_port = 0;
    int sync_port = 0;
    std::size_t trusted = 0;
    std::size_t online = 0;
    std::uint64_t probes_ok = 0;
    std::uint64_t probes_failed = 0;
    bool clipboard_enabled = false;
    std::string clipboard_mode;
    bool screen_enabled = false;
    int screen_fps = 0;
    bool sync_enabled = false;
    std::string sync_folder;
    std::string sync_mode;
    std::size_t sync_index = 0;
    std::size_t sync_conflicts = 0;
    std::string last_error;
};

using UiEventFn = std::function<void(const UiEvent&)>;
using DoneFn = std::function<void(bool ok, const std::string& message)>;

class Controller {
public:
    Controller();
    ~Controller();

    // 创建 Node；data_dir/config_path 为空时使用平台默认目录
    bool initialize(const std::string& data_dir, const std::string& config_path,
                    const std::string& name, std::string* error);
    bool start(std::string* error);
    void stop();
    // 停止后用新配置重建 Node 并重新启动（端口/开关类配置生效）
    bool restart(std::string* error);
    bool running() const;

    UiSnapshot snapshot() const;
    std::vector<lb::DeviceStatus> devices() const;
    std::vector<lb::TrustRecord> trust() const;
    std::string data_dir() const { return data_dir_; }
    std::string config_path() const { return config_path_; }

    // UI 线程投递器（由 wxApp 注入，内部使用 CallAfter）
    void set_ui_post(std::function<void(std::function<void()>)> post);
    // 模态确认对话框实现（在 UI 线程执行，返回 true 表示接受）
    void set_ask_handler(std::function<bool(const std::string&, const std::string&)> fn);
    void set_event_sink(UiEventFn fn);

    // ---- 操作（除特别说明外均在后台线程执行，完成回调经 UI 线程投递）----
    void pair_with(const std::string& ip, const std::string& code, DoneFn done);
    void send_file(const std::string& device_id, const std::string& local_path,
                   const std::string& remote_name, DoneFn done);
    void push_clipboard(const std::string& device_id_or_all, const std::string& text, DoneFn done);
    void read_clipboard(std::function<void(bool ok, const std::string& mime,
                                            const std::string& text, std::uint64_t size)> done);
    void sync_once(const std::string& device_id_or_all, DoneFn done);
    // input 非空时以控制模式建立会话，并在会话期间转发队列中的输入事件
    void view_screen(const std::string& device_id, int seconds,
                     std::shared_ptr<lb::ScreenInputQueue> input, DoneFn done);

    // 同步冲突与版本库（快速调用，UI 线程可用）
    std::vector<lb::SyncConflictInfo> conflicts() const;
    void resolve_conflict(const std::string& path, const std::string& action, DoneFn done);
    std::vector<lb::VersionInfo> history(const std::string& path) const;
    void restore_version(const std::string& path, const std::string& version_id, DoneFn done);

    lb::PairCode generate_code(int ttl_sec);
    std::string pairing_state_text() const;

    // 配置：读取当前 AppConfig、写回 TOML（保留注释由调用方决定）
    const lb::AppConfig& config() const;
    bool save_config(const lb::AppConfig& cfg, std::string* error);

    // 自动接受（跳过确认对话框，测试用）
    void set_auto_accept(bool value);

    // 后台任务数量（用于退出时等待）
    void wait_all_tasks();
    void set_closing(bool value) { closing_ = value; }

private:
    void attach_subscriptions();
    void install_confirm_handlers();
    void detach_all();
    void run_async(std::function<void()> work);
    // 在 UI 线程同步询问（阻塞调用线程，默认 120 秒超时按拒绝处理）
    bool ask_ui(const std::string& title, const std::string& message);
    void emit(UiEvent ev);
    void notify_done(DoneFn done, bool ok, const std::string& message);

    std::unique_ptr<lb::Node> node_;
    std::vector<lb::Subscription> subs_;
    std::string data_dir_;
    std::string config_path_;
    std::string device_name_;

    std::function<void(std::function<void()>)> post_;
    std::function<bool(const std::string&, const std::string&)> ask_fn_;
    UiEventFn event_sink_;
    std::vector<std::future<void>> tasks_;
    std::mutex tasks_mu_;

    bool auto_accept_ = false;
    std::atomic<bool> closing_{false};
    std::atomic<bool> asking_{false};
};

} // namespace lb::gui
