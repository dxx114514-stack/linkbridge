// Windows 屏幕采集：GDI BitBlt 抓取虚拟桌面，GetDIBits 输出 32 位 top-down DIB。
#include "core/screen/backend.h"

#include <windows.h>

#include <cstring>

namespace lb {
namespace {

class SystemScreenCapture : public IScreenCapture {
public:
    Expected<ScreenFrame> capture() override {
        HDC screen = GetDC(nullptr);
        if (!screen) return Expected<ScreenFrame>::fail("获取屏幕 DC 失败");

        const int x = GetSystemMetrics(SM_XVIRTUALSCREEN);
        const int y = GetSystemMetrics(SM_YVIRTUALSCREEN);
        const int width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        const int height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        if (width <= 0 || height <= 0) {
            ReleaseDC(nullptr, screen);
            return Expected<ScreenFrame>::fail("桌面尺寸非法");
        }

        HDC mem = CreateCompatibleDC(screen);
        HBITMAP bitmap = CreateCompatibleBitmap(screen, width, height);
        if (!mem || !bitmap) {
            if (bitmap) DeleteObject(bitmap);
            if (mem) DeleteDC(mem);
            ReleaseDC(nullptr, screen);
            return Expected<ScreenFrame>::fail("创建兼容位图失败");
        }
        HGDIOBJ old = SelectObject(mem, bitmap);

        bool ok = BitBlt(mem, 0, 0, width, height, screen, x, y, SRCCOPY | CAPTUREBLT) != FALSE;

        const std::size_t pixel_bytes = static_cast<std::size_t>(width) *
                                        static_cast<std::size_t>(height) * 4u;
        ByteBuf data(sizeof(BITMAPINFOHEADER) + pixel_bytes);

        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height; // 负值 = top-down
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        info.bmiHeader.biSizeImage = static_cast<DWORD>(pixel_bytes);

        if (ok) {
            ok = GetDIBits(mem, bitmap, 0, static_cast<UINT>(height),
                           data.data() + sizeof(BITMAPINFOHEADER), &info,
                           DIB_RGB_COLORS) != 0;
        }

        SelectObject(mem, old);
        DeleteObject(bitmap);
        DeleteDC(mem);
        ReleaseDC(nullptr, screen);

        if (!ok) return Expected<ScreenFrame>::fail("抓取桌面像素失败");

        std::memcpy(data.data(), &info.bmiHeader, sizeof(BITMAPINFOHEADER));
        ScreenFrame frame;
        frame.width = width;
        frame.height = height;
        frame.mime = "image/dib";
        frame.data = std::move(data);
        return Expected<ScreenFrame>::success(std::move(frame));
    }
};

} // namespace

std::shared_ptr<IScreenCapture> make_system_screen_capture() {
    return std::make_shared<SystemScreenCapture>();
}

} // namespace lb
