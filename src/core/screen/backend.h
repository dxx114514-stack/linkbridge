#pragma once
// 屏幕采集平台抽象：核心层只依赖本接口，Win32/X11/Android 各自实现。
#include "core/common/types.h"

#include <memory>
#include <string>

namespace lb {

struct ScreenFrame {
    int width = 0;
    int height = 0;
    std::string mime; // "image/dib"
    ByteBuf data;
    bool empty() const { return width <= 0 || height <= 0 || data.empty(); }
};

class IScreenCapture {
public:
    virtual ~IScreenCapture() = default;
    virtual Expected<ScreenFrame> capture() = 0;
};

// 平台实现：Windows 使用 GDI BitBlt + GetDIBits（32 位 top-down DIB）
std::shared_ptr<IScreenCapture> make_system_screen_capture();

} // namespace lb
