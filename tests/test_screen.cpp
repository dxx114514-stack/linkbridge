#include "core/config/config.h"
#include "core/screen/screen.h"

#include "test.h"

#include <cstdint>
#include <string>

namespace {

void put_le32(lb::ByteBuf& b, std::size_t off, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b[off + i] = static_cast<std::uint8_t>(v >> (i * 8));
}

std::uint32_t get_le32(const lb::ByteBuf& b, std::size_t off) {
    return static_cast<std::uint32_t>(b[off]) | (static_cast<std::uint32_t>(b[off + 1]) << 8) |
           (static_cast<std::uint32_t>(b[off + 2]) << 16) |
           (static_cast<std::uint32_t>(b[off + 3]) << 24);
}

lb::ByteBuf make_dib(int width, int height, bool top_down) {
    const std::size_t pixels = static_cast<std::size_t>(width) * height * 4u;
    lb::ByteBuf dib(40 + pixels, 0x20);
    put_le32(dib, 0, 40); // biSize
    put_le32(dib, 4, static_cast<std::uint32_t>(width));
    put_le32(dib, 8, static_cast<std::uint32_t>(top_down ? -height : height));
    dib[12] = 1; // biPlanes
    dib[14] = 32; // biBitCount
    put_le32(dib, 20, static_cast<std::uint32_t>(pixels)); // biSizeImage
    return dib;
}

} // namespace

TEST(screen_dib_info) {
    int width = 0;
    int height = 0;
    CHECK(lb::ScreenService::dib_info(make_dib(4, 3, true), width, height));
    CHECK(width == 4);
    CHECK(height == 3);

    CHECK(lb::ScreenService::dib_info(make_dib(4, 3, false), width, height));
    CHECK(width == 4);
    CHECK(height == 3);

    CHECK(!lb::ScreenService::dib_info({}, width, height));
    CHECK(!lb::ScreenService::dib_info(lb::ByteBuf(12, 0), width, height));

    lb::ByteBuf bad = make_dib(4, 3, true);
    put_le32(bad, 0, 8); // 头部过小
    CHECK(!lb::ScreenService::dib_info(bad, width, height));

    lb::ByteBuf zero_width = make_dib(4, 3, true);
    put_le32(zero_width, 4, 0);
    CHECK(!lb::ScreenService::dib_info(zero_width, width, height));
}

TEST(screen_dib_to_bmp) {
    const lb::ByteBuf dib = make_dib(2, 2, true);
    const lb::ByteBuf bmp = lb::ScreenService::dib_to_bmp(dib);
    CHECK(bmp.size() == 14 + dib.size());
    CHECK(bmp[0] == 'B' && bmp[1] == 'M');
    CHECK(get_le32(bmp, 2) == 14 + dib.size()); // 文件大小
    CHECK(get_le32(bmp, 10) == 14 + 40);        // 像素数据偏移
    CHECK(lb::ByteBuf(bmp.begin() + 14, bmp.end()) == dib);

    CHECK(lb::ScreenService::dib_to_bmp({}).empty());
    lb::ByteBuf truncated(10, 0);
    CHECK(lb::ScreenService::dib_to_bmp(truncated).empty());
}

TEST(screen_config_validation) {
    lb::AppConfig cfg;
    cfg.device_name = "test";
    CHECK(lb::validate_config(cfg).success);

    cfg.screen.max_fps = 0;
    CHECK(!lb::validate_config(cfg).success);

    cfg.screen.max_fps = 2;
    cfg.screen.max_bytes = 100;
    CHECK(!lb::validate_config(cfg).success);

    cfg.screen.max_bytes = 16777216;
    cfg.network.screen_port = cfg.network.clipboard_port;
    CHECK(!lb::validate_config(cfg).success);

    cfg.network.screen_port = 45715;
    cfg.network.pair_port = cfg.network.screen_port;
    CHECK(!lb::validate_config(cfg).success);
}
