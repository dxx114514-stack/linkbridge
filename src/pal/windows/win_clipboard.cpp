#include "core/clipboard/backend.h"

#include <windows.h>

#include <cstring>
#include <cwchar>
#include <string>
#include <thread>

namespace lb {
namespace {

std::wstring to_wide(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                                      nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), w.data(), n);
    return w;
}

std::string to_utf8(const wchar_t* text, int len) {
    if (!text || len <= 0) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, text, len, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, len, s.data(), n, nullptr, nullptr);
    return s;
}

bool has_sensitive_marker() {
    const UINT exclude = RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing");
    if (exclude != 0 && IsClipboardFormatAvailable(exclude)) return true;

    const UINT can_history = RegisterClipboardFormatW(L"CanIncludeInClipboardHistory");
    if (can_history != 0 && IsClipboardFormatAvailable(can_history)) {
        HANDLE h = GetClipboardData(can_history);
        if (h) {
            const auto* value = static_cast<const DWORD*>(GlobalLock(h));
            const bool allowed = value != nullptr && *value != 0;
            GlobalUnlock(h);
            if (!allowed) return true;
        }
    }
    return false;
}

class SystemClipboard final : public IClipboardBackend {
public:
    SystemClipboard() {
        change_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        created_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        thread_ = std::thread([this] { pump(); });
        WaitForSingleObject(created_, 3000);
    }

    ~SystemClipboard() override {
        if (thread_.joinable()) {
            if (thread_id_ != 0) PostThreadMessageW(thread_id_, WM_QUIT, 0, 0);
            thread_.join();
        }
        if (change_) CloseHandle(change_);
        if (created_) CloseHandle(created_);
    }

    bool wait_for_change(int timeout_ms) override {
        if (!change_) return false;
        return WaitForSingleObject(change_, static_cast<DWORD>(timeout_ms)) == WAIT_OBJECT_0;
    }

    std::uint64_t sequence() override { return GetClipboardSequenceNumber(); }

    Expected<ClipboardContent> read() override {
        if (!OpenClipboard(nullptr)) return Expected<ClipboardContent>::fail("打开剪贴板失败");

        ClipboardContent content;
        if (has_sensitive_marker()) {
            CloseClipboard();
            return Expected<ClipboardContent>::success(std::move(content));
        }

        if (IsClipboardFormatAvailable(CF_UNICODETEXT)) {
            HANDLE h = GetClipboardData(CF_UNICODETEXT);
            if (h) {
                const auto* text = static_cast<const wchar_t*>(GlobalLock(h));
                if (text) {
                    const std::string utf8 = to_utf8(text, static_cast<int>(std::wcslen(text)));
                    GlobalUnlock(h);
                    if (!utf8.empty()) {
                        content.mime = "text/plain";
                        content.data.assign(utf8.begin(), utf8.end());
                    }
                }
            }
        } else if (IsClipboardFormatAvailable(CF_DIBV5)) {
            if (auto dib = read_dib(CF_DIBV5)) content = std::move(*dib);
        } else if (IsClipboardFormatAvailable(CF_DIB)) {
            if (auto dib = read_dib(CF_DIB)) content = std::move(*dib);
        }

        CloseClipboard();
        return Expected<ClipboardContent>::success(std::move(content));
    }

    Result write(const ClipboardContent& content) override {
        if (content.mime != "text/plain" && content.mime != "image/dib")
            return Result::fail("不支持的剪贴板类型: " + content.mime);
        if (content.data.empty()) return Result::fail("剪贴板内容为空");

        if (!OpenClipboard(nullptr)) return Result::fail("打开剪贴板失败");
        if (!EmptyClipboard()) {
            CloseClipboard();
            return Result::fail("清空剪贴板失败");
        }

        Result result = Result::ok();
        if (content.mime == "text/plain") {
            const std::string utf8(content.data.begin(), content.data.end());
            const std::wstring wide = to_wide(utf8);
            const std::size_t bytes = (wide.size() + 1) * sizeof(wchar_t);
            HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
            if (!mem) {
                CloseClipboard();
                return Result::fail("分配剪贴板内存失败");
            }
            auto* dst = static_cast<wchar_t*>(GlobalLock(mem));
            if (!dst) {
                GlobalFree(mem);
                CloseClipboard();
                return Result::fail("锁定剪贴板内存失败");
            }
            std::wmemcpy(dst, wide.c_str(), wide.size() + 1);
            GlobalUnlock(mem);
            if (!SetClipboardData(CF_UNICODETEXT, mem)) {
                GlobalFree(mem);
                result = Result::fail("写入文本剪贴板失败");
            }
        } else {
            std::uint32_t header_size = 0;
            if (content.data.size() >= 4) {
                header_size = static_cast<std::uint32_t>(content.data[0]) |
                              (static_cast<std::uint32_t>(content.data[1]) << 8) |
                              (static_cast<std::uint32_t>(content.data[2]) << 16) |
                              (static_cast<std::uint32_t>(content.data[3]) << 24);
            }
            const UINT format = header_size == 124 ? CF_DIBV5 : CF_DIB;
            HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, content.data.size());
            if (!mem) {
                CloseClipboard();
                return Result::fail("分配剪贴板内存失败");
            }
            void* dst = GlobalLock(mem);
            if (!dst) {
                GlobalFree(mem);
                CloseClipboard();
                return Result::fail("锁定剪贴板内存失败");
            }
            std::memcpy(dst, content.data.data(), content.data.size());
            GlobalUnlock(mem);
            if (!SetClipboardData(format, mem)) {
                GlobalFree(mem);
                result = Result::fail("写入图片剪贴板失败");
            }
        }

        CloseClipboard();
        return result;
    }

private:
    void pump() {
        thread_id_ = GetCurrentThreadId();
        HWND hwnd = CreateWindowExW(0, L"STATIC", L"LinkBridgeClipboard", 0, 0, 0, 0, 0,
                                    HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (hwnd) {
            AddClipboardFormatListener(hwnd);
            hwnd_ = hwnd;
        }
        SetEvent(created_);

        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (msg.message == WM_CLIPBOARDUPDATE && change_) SetEvent(change_);
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        if (hwnd_) RemoveClipboardFormatListener(hwnd_);
        if (hwnd_) DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }

    Expected<ClipboardContent> read_dib(UINT format) {
        ClipboardContent content;
        HANDLE h = GetClipboardData(format);
        if (!h) return Expected<ClipboardContent>::fail("读取图片剪贴板失败");
        const SIZE_T size = GlobalSize(h);
        const void* src = GlobalLock(h);
        if (!src || size == 0) {
            if (src) GlobalUnlock(h);
            return Expected<ClipboardContent>::fail("图片剪贴板为空");
        }
        content.mime = "image/dib";
        content.data.assign(static_cast<const std::uint8_t*>(src),
                            static_cast<const std::uint8_t*>(src) + size);
        GlobalUnlock(h);
        return Expected<ClipboardContent>::success(std::move(content));
    }

    HANDLE change_ = nullptr;
    HANDLE created_ = nullptr;
    DWORD thread_id_ = 0;
    HWND hwnd_ = nullptr;
    std::thread thread_;
};

} // namespace

std::shared_ptr<IClipboardBackend> make_system_clipboard() {
    return std::make_shared<SystemClipboard>();
}

} // namespace lb
