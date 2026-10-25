// Linux 屏幕采集：X11 XGetImage 抓取根窗口，输出 32 位 top-down DIB（与 Windows 一致，
// 便于复用核心的 DIB→BMP 与对端解码逻辑）。Wayland 会话下若 XGetImage 失败会返回错误。
#include "core/screen/backend.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <cstdint>
#include <cstring>
#include <mutex>

namespace lb {
namespace {

#pragma pack(push, 1)
// 与 Windows BITMAPINFOHEADER 完全一致的 40 字节 DIB 头
struct DibHeader {
    std::uint32_t biSize;
    std::int32_t biWidth;
    std::int32_t biHeight;
    std::uint16_t biPlanes;
    std::uint16_t biBitCount;
    std::uint32_t biCompression;
    std::uint32_t biSizeImage;
    std::int32_t biXPelsPerMeter;
    std::int32_t biYPelsPerMeter;
    std::uint32_t biClrUsed;
    std::uint32_t biClrImportant;
};
#pragma pack(pop)
static_assert(sizeof(DibHeader) == 40, "DIB header must be 40 bytes");

class X11ScreenCapture final : public IScreenCapture {
public:
    X11ScreenCapture() {
        static std::once_flag once;
        std::call_once(once, [] { XInitThreads(); });
        dpy_ = XOpenDisplay(nullptr);
    }

    ~X11ScreenCapture() override {
        if (dpy_) {
            XCloseDisplay(dpy_);
            dpy_ = nullptr;
        }
    }

    Expected<ScreenFrame> capture() override {
        if (!dpy_) return Expected<ScreenFrame>::fail("X11 不可用（缺少 DISPLAY）");
        std::lock_guard<std::mutex> lk(mu_);

        const Window root = DefaultRootWindow(dpy_);
        XWindowAttributes attr{};
        if (!XGetWindowAttributes(dpy_, root, &attr) || attr.width <= 0 || attr.height <= 0)
            return Expected<ScreenFrame>::fail("读取根窗口尺寸失败");

        const int width = attr.width;
        const int height = attr.height;
        XImage* image = XGetImage(dpy_, root, 0, 0, static_cast<unsigned>(width),
                                  static_cast<unsigned>(height), AllPlanes, ZPixmap);
        if (!image) return Expected<ScreenFrame>::fail("抓取屏幕失败（Wayland 会话请使用 XWayland）");

        const std::size_t pixels = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
        ByteBuf data(sizeof(DibHeader) + pixels * 4);

        DibHeader header{};
        header.biSize = sizeof(DibHeader);
        header.biWidth = width;
        header.biHeight = -height; // 负值 = top-down
        header.biPlanes = 1;
        header.biBitCount = 32;
        header.biCompression = 0; // BI_RGB
        header.biSizeImage = static_cast<std::uint32_t>(pixels * 4);

        auto* dst = data.data() + sizeof(DibHeader);
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const unsigned long pixel = XGetPixel(image, x, y);
                std::uint8_t* p = dst + (static_cast<std::size_t>(y) * width + x) * 4;
                p[0] = static_cast<std::uint8_t>((pixel >> 0) & 0xFF);  // B
                p[1] = static_cast<std::uint8_t>((pixel >> 8) & 0xFF);  // G
                p[2] = static_cast<std::uint8_t>((pixel >> 16) & 0xFF); // R
                p[3] = 0xFF;
            }
        }
        XDestroyImage(image);
        std::memcpy(data.data(), &header, sizeof(header));

        ScreenFrame frame;
        frame.width = width;
        frame.height = height;
        frame.mime = "image/dib";
        frame.data = std::move(data);
        return Expected<ScreenFrame>::success(std::move(frame));
    }

private:
    Display* dpy_ = nullptr;
    std::mutex mu_;
};

} // namespace

std::shared_ptr<IScreenCapture> make_system_screen_capture() {
    return std::make_shared<X11ScreenCapture>();
}

// Linux 侧暂未实现输入注入（XTest 需要额外链接库），控制模式返回不可用。
std::shared_ptr<IScreenInput> make_system_screen_input() { return nullptr; }

} // namespace lb
