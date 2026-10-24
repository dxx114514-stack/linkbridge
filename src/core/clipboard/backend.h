#pragma once
// 剪贴板平台抽象：核心层只依赖本接口，Win32/X11/Android 各自实现。
#include "core/common/types.h"

#include <cstdint>
#include <memory>
#include <string>

namespace lb {

struct ClipboardContent {
    std::string mime; // "text/plain" | "image/dib"
    ByteBuf data;
    bool empty() const { return mime.empty() || data.empty(); }
};

class IClipboardBackend {
public:
    virtual ~IClipboardBackend() = default;

    // 等待一次剪贴板变化；超时返回 false（调用方据此做兜底轮询）
    virtual bool wait_for_change(int timeout_ms) = 0;
    virtual std::uint64_t sequence() = 0;
    virtual Expected<ClipboardContent> read() = 0;
    virtual Result write(const ClipboardContent& content) = 0;
};

// 平台实现：Windows 使用 Win32 Clipboard API + WM_CLIPBOARDUPDATE
std::shared_ptr<IClipboardBackend> make_system_clipboard();

} // namespace lb
