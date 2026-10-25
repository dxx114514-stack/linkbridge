// Linux 剪贴板实现：X11 Selection 协议。后台线程持有隐藏窗口响应 CLIPBOARD 请求。
// 文本以 UTF8_STRING 交换；图片剪贴板（image/dib）在 X11 下暂不支持，写入会返回错误。
#include "core/clipboard/backend.h"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <sys/select.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>

namespace lb {
namespace {

void ensure_x11_threads() {
    static std::once_flag once;
    std::call_once(once, [] { XInitThreads(); });
}

class X11Clipboard final : public IClipboardBackend {
public:
    X11Clipboard() {
        ensure_x11_threads();
        dpy_ = XOpenDisplay(nullptr);
        if (!dpy_) return;
        win_ = XCreateSimpleWindow(dpy_, DefaultRootWindow(dpy_), -10, -10, 1, 1, 0, 0, 0);
        if (!win_) {
            XCloseDisplay(dpy_);
            dpy_ = nullptr;
            return;
        }
        clipboard_ = XInternAtom(dpy_, "CLIPBOARD", False);
        utf8_ = XInternAtom(dpy_, "UTF8_STRING", False);
        text_ = XInternAtom(dpy_, "STRING", False);
        targets_ = XInternAtom(dpy_, "TARGETS", False);
        property_ = XInternAtom(dpy_, "LINKBRIDGE_CLIP", False);
        last_owner_ = XGetSelectionOwner(dpy_, clipboard_);
        thread_ = std::thread([this] { loop(); });
    }

    ~X11Clipboard() override {
        stop_.store(true);
        if (thread_.joinable()) thread_.join();
        if (dpy_) {
            if (win_) XDestroyWindow(dpy_, win_);
            XCloseDisplay(dpy_);
            dpy_ = nullptr;
        }
    }

    bool wait_for_change(int timeout_ms) override {
        if (!dpy_ || timeout_ms <= 0) return false;
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (!stop_.load() && std::chrono::steady_clock::now() < deadline) {
            const Window current = XGetSelectionOwner(dpy_, clipboard_);
            bool changed = false;
            {
                std::lock_guard<std::mutex> lk(mu_);
                if (current != last_owner_) {
                    last_owner_ = current;
                    changed = true;
                }
            }
            if (changed) {
                ++seq_;
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
        }
        return false;
    }

    std::uint64_t sequence() override { return seq_.load(); }

    Expected<ClipboardContent> read() override {
        if (!dpy_) return Expected<ClipboardContent>::fail("X11 不可用（缺少 DISPLAY）");
        std::lock_guard<std::mutex> read_lock(read_mu_);

        {
            std::lock_guard<std::mutex> lk(mu_);
            if (owning_ && !own_.empty()) return Expected<ClipboardContent>::success(own_);
        }
        if (XGetSelectionOwner(dpy_, clipboard_) == None)
            return Expected<ClipboardContent>::success(ClipboardContent{});

        {
            std::lock_guard<std::mutex> lk(req_mu_);
            req_done_ = false;
            req_result_ = ClipboardContent{};
        }
        XConvertSelection(dpy_, clipboard_, utf8_, property_, win_, CurrentTime);
        XFlush(dpy_);

        std::unique_lock<std::mutex> lk(req_mu_);
        if (!req_cv_.wait_for(lk, std::chrono::milliseconds(1200),
                              [this] { return req_done_; }))
            return Expected<ClipboardContent>::fail("读取 X11 剪贴板超时");
        return Expected<ClipboardContent>::success(req_result_);
    }

    Result write(const ClipboardContent& content) override {
        if (!dpy_) return Result::fail("X11 不可用（缺少 DISPLAY）");
        if (content.mime != "text/plain")
            return Result::fail("X11 剪贴板当前仅支持文本内容: " + content.mime);
        if (content.data.empty()) return Result::fail("剪贴板内容为空");

        {
            std::lock_guard<std::mutex> lk(mu_);
            own_ = content;
            owning_ = true;
        }
        XSetSelectionOwner(dpy_, clipboard_, win_, CurrentTime);
        XFlush(dpy_);
        if (XGetSelectionOwner(dpy_, clipboard_) != win_)
            return Result::fail("获取剪贴板所有权失败");
        ++seq_;
        return Result::ok();
    }

private:
    void loop() {
        const int fd = ConnectionNumber(dpy_);
        while (!stop_.load()) {
            while (XPending(dpy_) > 0) {
                if (stop_.load()) break;
                XEvent ev;
                XNextEvent(dpy_, &ev);
                handle(ev);
            }
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(fd, &fds);
            timeval tv{};
            tv.tv_sec = 0;
            tv.tv_usec = 50000;
            ::select(fd + 1, &fds, nullptr, nullptr, &tv);
        }
    }

    void handle(XEvent& ev) {
        switch (ev.type) {
            case SelectionRequest:
                serve(ev.xselectionrequest);
                break;
            case SelectionClear:
                if (ev.xselectionclear.selection == clipboard_) {
                    std::lock_guard<std::mutex> lk(mu_);
                    owning_ = false;
                }
                ++seq_;
                break;
            case SelectionNotify: {
                ClipboardContent content = fetch(ev.xselection);
                {
                    std::lock_guard<std::mutex> lk(req_mu_);
                    req_result_ = std::move(content);
                    req_done_ = true;
                }
                req_cv_.notify_all();
                break;
            }
            default:
                break;
        }
    }

    void serve(const XSelectionRequestEvent& req) {
        XSelectionEvent notify{};
        notify.type = SelectionNotify;
        notify.display = dpy_;
        notify.requestor = req.requestor;
        notify.selection = req.selection;
        notify.target = req.target;
        notify.time = req.time;
        notify.property = None;

        const Atom prop = req.property != None ? req.property : req.target;
        if (req.target == targets_) {
            Atom supported[] = {targets_, utf8_, text_};
            XChangeProperty(dpy_, req.requestor, prop, XA_ATOM, 32, PropModeReplace,
                            reinterpret_cast<unsigned char*>(supported), 3);
            notify.property = prop;
        } else if (req.target == utf8_ || req.target == text_) {
            ByteBuf copy;
            {
                std::lock_guard<std::mutex> lk(mu_);
                copy = own_.data;
            }
            XChangeProperty(dpy_, req.requestor, prop, req.target, 8, PropModeReplace,
                            copy.data(), static_cast<int>(copy.size()));
            notify.property = prop;
        }
        XSendEvent(dpy_, req.requestor, False, 0, reinterpret_cast<XEvent*>(&notify));
        XFlush(dpy_);
    }

    ClipboardContent fetch(const XSelectionEvent& ev) {
        ClipboardContent content;
        if (ev.property == None) return content;

        Atom type = None;
        int format = 0;
        unsigned long items = 0;
        unsigned long after = 0;
        unsigned char* data = nullptr;
        const int rc = XGetWindowProperty(dpy_, win_, ev.property, 0, 0x1FFFFFFF, False,
                                          AnyPropertyType, &type, &format, &items, &after, &data);
        if (rc != Success || !data) return content;
        if (format == 8 && items > 0 && (type == utf8_ || type == text_ || type == XA_STRING)) {
            content.mime = "text/plain";
            content.data.assign(data, data + items);
        }
        XFree(data);
        XDeleteProperty(dpy_, win_, ev.property);
        return content;
    }

    Display* dpy_ = nullptr;
    Window win_ = 0;
    Atom clipboard_ = None;
    Atom utf8_ = None;
    Atom text_ = None;
    Atom targets_ = None;
    Atom property_ = None;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<std::uint64_t> seq_{0};

    std::mutex mu_;      // own_ / owning_ / last_owner_
    ClipboardContent own_;
    bool owning_ = false;
    Window last_owner_ = 0;

    std::mutex read_mu_; // 串行化 read()
    std::mutex req_mu_;
    std::condition_variable req_cv_;
    bool req_done_ = false;
    ClipboardContent req_result_;
};

} // namespace

std::shared_ptr<IClipboardBackend> make_system_clipboard() {
    return std::make_shared<X11Clipboard>();
}

} // namespace lb
