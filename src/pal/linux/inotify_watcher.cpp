// Linux 文件监听：inotify 递归监听（自动为新目录补挂 watch），事件直接交给同步引擎防抖。
#include "core/sync/watcher.h"

#include <poll.h>
#include <sys/inotify.h>
#include <unistd.h>

#include <atomic>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace lb {
namespace {

constexpr std::uint32_t kWatchMask = IN_CREATE | IN_MODIFY | IN_CLOSE_WRITE | IN_DELETE |
                                     IN_MOVED_FROM | IN_MOVED_TO | IN_ATTRIB | IN_DELETE_SELF |
                                     IN_MOVE_SELF;

class InotifyFileWatcher final : public IFileWatcher {
public:
    ~InotifyFileWatcher() override { stop(); }

    Result start(const std::string& root, std::function<void()> on_change) override {
        if (thread_.joinable()) return Result::fail("文件监听已启动");
        std::error_code ec;
        if (!std::filesystem::is_directory(std::filesystem::path(root), ec))
            return Result::fail("同步文件夹不存在: " + root);

        fd_ = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (fd_ < 0) return Result::fail("inotify_init1 失败");

        {
            std::lock_guard<std::mutex> lk(mu_);
            add_recursive_locked(root);
        }
        if (wd_path_.empty()) {
            ::close(fd_);
            fd_ = -1;
            return Result::fail("inotify 添加监听失败: " + root);
        }

        on_change_ = std::move(on_change);
        stop_.store(false);
        thread_ = std::thread([this] { loop(); });
        return Result::ok();
    }

    void stop() override {
        stop_.store(true);
        if (thread_.joinable()) thread_.join();
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (fd_ >= 0) {
                ::close(fd_);
                fd_ = -1;
            }
            wd_path_.clear();
        }
        on_change_ = nullptr;
    }

private:
    void add_watch_locked(const std::string& path) {
        if (fd_ < 0) return;
        const int wd = ::inotify_add_watch(fd_, path.c_str(), kWatchMask);
        if (wd >= 0) wd_path_[wd] = path;
    }

    void add_recursive_locked(const std::string& root) {
        add_watch_locked(root);
        std::error_code ec;
        for (auto it = std::filesystem::recursive_directory_iterator(
                 std::filesystem::path(root), ec);
             !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
            std::error_code entry_ec;
            if (it->is_directory(entry_ec) && !entry_ec) add_watch_locked(it->path().string());
        }
    }

    void loop() {
        std::vector<char> buffer(64 * 1024);
        while (!stop_.load()) {
            pollfd pfd{fd_, POLLIN, 0};
            const int rc = ::poll(&pfd, 1, 200);
            if (rc <= 0) continue;

            const ssize_t n = ::read(fd_, buffer.data(), buffer.size());
            if (n <= 0) continue;

            bool changed = false;
            {
                std::lock_guard<std::mutex> lk(mu_);
                ssize_t offset = 0;
                while (offset + static_cast<ssize_t>(sizeof(inotify_event)) <= n) {
                    const auto* ev = reinterpret_cast<const inotify_event*>(buffer.data() + offset);
                    offset += static_cast<ssize_t>(sizeof(inotify_event) + ev->len);
                    changed = true;

                    if ((ev->mask & IN_ISDIR) && (ev->mask & IN_CREATE) && ev->len > 0) {
                        const auto it = wd_path_.find(ev->wd);
                        if (it != wd_path_.end())
                            add_recursive_locked(it->second + "/" + std::string(ev->name));
                    }
                    if (ev->mask & IN_IGNORED) wd_path_.erase(ev->wd);
                }
            }
            if (changed && on_change_ && !stop_.load()) on_change_();
        }
    }

    int fd_ = -1;
    std::atomic<bool> stop_{false};
    std::mutex mu_;
    std::unordered_map<int, std::string> wd_path_;
    std::function<void()> on_change_;
    std::thread thread_;
};

} // namespace

std::shared_ptr<IFileWatcher> make_file_watcher() {
    return std::make_shared<InotifyFileWatcher>();
}

} // namespace lb
