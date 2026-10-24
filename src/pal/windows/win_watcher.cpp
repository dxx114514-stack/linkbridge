// Windows 文件监听：ReadDirectoryChangesW 递归监听，事件回调交给同步引擎做防抖。
#include "core/sync/watcher.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

namespace lb {
namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (len <= 0) return {};
    std::wstring w(static_cast<size_t>(len), 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), len);
    return w;
}

class WinFileWatcher : public IFileWatcher {
public:
    ~WinFileWatcher() override { stop(); }

    Result start(const std::string& root, std::function<void()> on_change) override {
        if (thread_.joinable()) return Result::fail("文件监听已启动");
        const std::wstring wroot = widen(root);
        if (wroot.empty()) return Result::fail("同步文件夹路径为空");
        HANDLE dir = CreateFileW(wroot.c_str(), FILE_LIST_DIRECTORY,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                 OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
                                 nullptr);
        if (dir == INVALID_HANDLE_VALUE) return Result::fail("打开同步文件夹失败");
        dir_ = dir;
        on_change_ = std::move(on_change);
        stop_.store(false);
        thread_ = std::thread([this] { loop(); });
        return Result::ok();
    }

    void stop() override {
        stop_.store(true);
        if (thread_.joinable()) thread_.join();
        if (dir_ != INVALID_HANDLE_VALUE) {
            CloseHandle(dir_);
            dir_ = INVALID_HANDLE_VALUE;
        }
        on_change_ = nullptr;
    }

private:
    void loop() {
        // 有事件时合并上报：250ms 内的连续变化只通知一次，避免写入风暴。
        constexpr DWORD kFilter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                                  FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE;
        std::vector<unsigned char> buf(64 * 1024);
        HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ev) return;
        OVERLAPPED ov{};
        ov.hEvent = ev;

        bool pending = false;
        while (!stop_.load()) {
            if (!pending) {
                ResetEvent(ev);
                if (!ReadDirectoryChangesW(dir_, buf.data(), static_cast<DWORD>(buf.size()), TRUE,
                                           kFilter, nullptr, &ov, nullptr)) {
                    if (GetLastError() != ERROR_IO_PENDING) {
                        if (stop_.load()) break;
                        std::this_thread::sleep_for(std::chrono::milliseconds(300));
                        continue;
                    }
                }
                pending = true;
            }

            const DWORD wait = WaitForSingleObject(ev, 250);
            if (stop_.load()) {
                CancelIoEx(dir_, &ov);
                DWORD ignored = 0;
                GetOverlappedResult(dir_, &ov, &ignored, TRUE);
                break;
            }
            if (wait != WAIT_OBJECT_0) continue; // 未产生事件，继续等待同一次监听

            DWORD transferred = 0;
            if (!GetOverlappedResult(dir_, &ov, &transferred, FALSE)) {
                // 缓冲区溢出时 Windows 报 ERROR_NOTIFY_ENUM_DIR，仍需通知上层做全量扫描
                if (GetLastError() != ERROR_NOTIFY_ENUM_DIR) {
                    pending = false;
                    std::this_thread::sleep_for(std::chrono::milliseconds(300));
                    continue;
                }
            }
            pending = false;
            if (transferred > 0 && on_change_) on_change_();
        }

        CloseHandle(ev);
    }

    HANDLE dir_ = INVALID_HANDLE_VALUE;
    std::atomic<bool> stop_{false};
    std::function<void()> on_change_;
    std::thread thread_;
};

} // namespace

std::shared_ptr<IFileWatcher> make_file_watcher() { return std::make_shared<WinFileWatcher>(); }

} // namespace lb
