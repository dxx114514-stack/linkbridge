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

// ---- 远程控制输入（被控端注入）----

enum class ScreenInputType : std::uint8_t {
    Tap = 1,   // 单击
    Swipe = 2, // 滑动
    Key = 3,   // 系统按键（返回/主页/最近任务）
};

enum class ScreenKey : std::uint8_t {
    Back = 1,
    Home = 2,
    Recents = 3,
};

// 坐标使用被控端画面像素（与 ScreenFrame 宽高同一坐标系）
struct ScreenInput {
    ScreenInputType type = ScreenInputType::Tap;
    int x1 = 0;
    int y1 = 0;
    int x2 = 0;
    int y2 = 0;
    int duration_ms = 0;             // Swipe 用；0 视为 200ms
    ScreenKey key = ScreenKey::Back; // Key 用
};

class IScreenInput {
public:
    virtual ~IScreenInput() = default;
    // 注入一次输入事件；失败表示本端无法执行（如无障碍服务未启用）
    virtual Result inject(const ScreenInput& input) = 0;
};

// 平台实现：Android 经无障碍服务 dispatchGesture / performGlobalAction；
// Windows/Linux 暂无输入注入实现，返回 nullptr（此时控制模式不可用）。
std::shared_ptr<IScreenInput> make_system_screen_input();

} // namespace lb
