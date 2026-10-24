// 端到端测试：两个真实节点进程内互连，走真实 TCP + 加密握手。
// 覆盖：配对成功、双向信任落库、心跳在线判定、错误配对码被拒且作废。
#include "core/clipboard/backend.h"
#include "core/node/node.h"
#include "core/platform/platform.h"
#include "core/screen/backend.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <atomic>

namespace {

int g_failures = 0;

void check(bool cond, const char* what) {
    if (cond) {
        std::printf("  [OK]   %s\n", what);
    } else {
        std::printf("  [FAIL] %s\n", what);
        ++g_failures;
    }
}

lb::ByteBuf bytes_of(const std::string& s) { return lb::ByteBuf(s.begin(), s.end()); }

std::string text_of(const lb::ByteBuf& b) { return std::string(b.begin(), b.end()); }

void put_le32(lb::ByteBuf& b, std::size_t off, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b[off + i] = static_cast<std::uint8_t>(v >> (i * 8));
}

lb::ByteBuf make_test_dib(int width, int height) {
    const std::size_t pixels = static_cast<std::size_t>(width) * height * 4u;
    lb::ByteBuf dib(40 + pixels, 0x30);
    put_le32(dib, 0, 40);                              // biSize
    put_le32(dib, 4, static_cast<std::uint32_t>(width));
    put_le32(dib, 8, static_cast<std::uint32_t>(-height)); // top-down
    dib[12] = 1;                                       // biPlanes
    dib[14] = 32;                                      // biBitCount
    put_le32(dib, 20, static_cast<std::uint32_t>(pixels));
    return dib;
}

// 测试用屏幕采集：固定返回一张小尺寸 DIB，并统计采集次数。
class FakeScreenCapture : public lb::IScreenCapture {
public:
    FakeScreenCapture() {
        frame_.width = 4;
        frame_.height = 3;
        frame_.mime = "image/dib";
        frame_.data = make_test_dib(4, 3);
    }
    lb::Expected<lb::ScreenFrame> capture() override {
        std::lock_guard<std::mutex> lk(mu_);
        ++captures_;
        return lb::Expected<lb::ScreenFrame>::success(frame_);
    }
    std::size_t captures() const {
        std::lock_guard<std::mutex> lk(mu_);
        return captures_;
    }

private:
    mutable std::mutex mu_;
    lb::ScreenFrame frame_;
    std::size_t captures_ = 0;
};

// 测试用剪贴板后端：模拟系统剪贴板的变化通知与写入计数。
class FakeClipboardBackend : public lb::IClipboardBackend {
public:
    bool wait_for_change(int timeout_ms) override {
        if (timeout_ms <= 0) return false;
        std::unique_lock<std::mutex> lk(mu_);
        const std::uint64_t gen = gen_;
        return cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                            [&] { return gen_ != gen; });
    }
    std::uint64_t sequence() override {
        std::lock_guard<std::mutex> lk(mu_);
        return seq_;
    }
    lb::Expected<lb::ClipboardContent> read() override {
        std::lock_guard<std::mutex> lk(mu_);
        return lb::Expected<lb::ClipboardContent>::success(content_);
    }
    lb::Result write(const lb::ClipboardContent& content) override {
        {
            std::lock_guard<std::mutex> lk(mu_);
            content_ = content;
            ++seq_;
            ++gen_;
            ++writes_;
        }
        cv_.notify_all();
        return lb::Result::ok();
    }

    // 模拟本地用户复制内容（不增加远端写入计数）
    void set_local(const lb::ClipboardContent& content) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            content_ = content;
            ++seq_;
            ++gen_;
        }
        cv_.notify_all();
    }

    lb::ClipboardContent current() const {
        std::lock_guard<std::mutex> lk(mu_);
        return content_;
    }
    std::size_t write_count() const {
        std::lock_guard<std::mutex> lk(mu_);
        return writes_;
    }

private:
    mutable std::mutex mu_;
    std::condition_variable cv_;
    lb::ClipboardContent content_;
    std::size_t writes_ = 0;
    std::uint64_t seq_ = 0;
    std::uint64_t gen_ = 0;
};

bool wait_until(const std::function<bool()>& pred, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return pred();
}

void write_text(const std::filesystem::path& p, const std::string& s) {
    std::error_code ec;
    if (!p.parent_path().empty()) std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(s.data(), static_cast<std::streamsize>(s.size()));
}

std::string read_text(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// 查找 "<stem> (conflict ...)" 的冲突副本
std::vector<std::filesystem::path> conflict_copies(const std::filesystem::path& dir,
                                                   const std::string& stem) {
    std::vector<std::filesystem::path> out;
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(dir, ec);
         it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        const std::string name = it->path().filename().string();
        if (name.rfind(stem + " (conflict ", 0) == 0) out.push_back(it->path());
    }
    return out;
}

lb::AppConfig make_config(const char* name) {
    lb::AppConfig cfg;
    cfg.device_name = name;
    cfg.network.bind_addr = "127.0.0.1";
    cfg.network.pair_port = 0;      // 由系统分配，避免端口冲突
    cfg.network.heartbeat_port = 0;
    cfg.network.transfer_port = 0;
    cfg.network.clipboard_port = 0;
    cfg.network.screen_port = 0;
    cfg.network.sync_port = 0;
    cfg.network.connect_timeout_ms = 3000;
    cfg.heartbeat.interval_sec = 10;
    cfg.heartbeat.max_failures = 3;
    cfg.heartbeat.offline_probe_sec = 5;
    cfg.transfer.chunk_size = 32768; // 小分块以覆盖多块与续传路径
    cfg.clipboard.enabled = true;
    cfg.clipboard.mode = "auto";
    cfg.screen.enabled = true;
    cfg.screen.require_confirm_each_session = true;
    cfg.screen.max_fps = 10; // 缩短 e2e 等待
    cfg.sync.enabled = true;
    cfg.sync.rescan_interval_sec = 3600; // e2e 仅用显式 sync 与短时自动触发
    return cfg;
}

} // namespace

int main() {
    std::printf("LinkBridge 端到端测试（真实 TCP）\n================================\n");

    const auto base = std::filesystem::temp_directory_path() /
                      ("lb_e2e_" + std::to_string(lb::now_secs()));
    const auto dir_a = base / "A";
    const auto dir_b = base / "B";
    std::filesystem::remove_all(base);
    std::filesystem::create_directories(dir_a);
    std::filesystem::create_directories(dir_b);

    auto clip_a = std::make_shared<FakeClipboardBackend>();
    auto clip_b = std::make_shared<FakeClipboardBackend>();
    auto screen_a = std::make_shared<FakeScreenCapture>();
    auto screen_b = std::make_shared<FakeScreenCapture>();

    const auto sync_a = base / "sync_a";
    const auto sync_b = base / "sync_b";
    std::filesystem::create_directories(sync_a);
    std::filesystem::create_directories(sync_b);

    lb::NodeOptions options_a;
    options_a.data_dir = dir_a.string();
    options_a.config = make_config("E2E-A");
    options_a.device_name = "E2E-A";
    options_a.clipboard_backend = clip_a;
    options_a.screen_capture = screen_a;
    options_a.config.sync.folder = sync_a.string();
    options_a.config.sync.max_file_size_mb = 1; // 覆盖超限跳过
    lb::NodeOptions options_b;
    options_b.data_dir = dir_b.string();
    options_b.config = make_config("E2E-B");
    options_b.device_name = "E2E-B";
    options_b.clipboard_backend = clip_b;
    options_b.screen_capture = screen_b;
    options_b.config.sync.folder = sync_b.string();
    options_b.config.clipboard.mode = "confirm"; // B 默认为确认模式
    options_b.config.clipboard.max_bytes = 64 * 1024; // 便于覆盖超限拒绝

    auto a_result = lb::Node::create(options_a);
    auto b_result = lb::Node::create(options_b);
    check(a_result.has_value(), "节点 A 创建成功");
    check(b_result.has_value(), "节点 B 创建成功");
    if (!a_result || !b_result) return 1;

    auto node_a = std::move(*a_result);
    auto node_b = std::move(*b_result);

    node_a->pairing().set_auto_accept(true);
    node_b->pairing().set_auto_accept(true);
    // B 为 confirm 模式；e2e 中先自动接受，后续再单独验证拒绝路径
    node_b->clipboard().set_auto_accept(true);
    node_b->screen().set_auto_accept(true);

    check(node_a->start().success, "节点 A 监听启动");
    check(node_b->start().success, "节点 B 监听启动");
    check(node_a->sync().auto_running() && node_b->sync().auto_running(),
          "两端自动同步线程已启动");
    // e2e 需要确定性时序：关闭自动同步，仅保留显式 sync 与最后的自动触发用例
    node_a->sync().stop_auto();
    node_b->sync().stop_auto();

    const std::string id_a = node_a->identity().device_id();
    const std::string id_b = node_b->identity().device_id();
    std::printf("  A: %s (%s) 配对端口 %d 心跳端口 %d\n", id_a.c_str(),
                node_a->identity().fingerprint().c_str(), node_a->pair_port(),
                node_a->heartbeat_port());
    std::printf("  B: %s (%s) 配对端口 %d 心跳端口 %d\n", id_b.c_str(),
                node_b->identity().fingerprint().c_str(), node_b->pair_port(),
                node_b->heartbeat_port());

    // ---- 1. 正常配对 ----
    const lb::PairCode code = node_a->pairing().generate_code(300, 3);
    check(code.value.size() == 6, "配对码为 6 位");

    const lb::Result paired =
        node_b->pairing().pair_with("127.0.0.1", node_a->pair_port(), code.value);
    check(paired.success, "B 发起配对成功");
    if (!paired.success) std::printf("  原因: %s\n", paired.error.c_str());

    check(wait_until([&] { return node_a->trust().size() == 1; }, 5000),
          "A 侧写入受信任设备");
    check(node_b->trust().size() == 1, "B 侧写入受信任设备");

    const auto rec_a = node_a->trust().get(id_b);
    const auto rec_b = node_b->trust().get(id_a);
    check(rec_a.has_value(), "A 的信任库包含 B");
    check(rec_b.has_value(), "B 的信任库包含 A");
    if (rec_b) {
        check(rec_b->heartbeat_port == node_a->heartbeat_port(), "B 记录到 A 的心跳端口");
        check(rec_b->transfer_port == node_a->transfer_port(), "B 记录到 A 的传输端口");
        check(rec_b->clipboard_port == node_a->clipboard_port(), "B 记录到 A 的剪贴板端口");
        check(rec_b->screen_port == node_a->screen_port(), "B 记录到 A 的屏幕共享端口");
        check(rec_b->sync_port == node_a->sync_port(), "B 记录到 A 的文件夹同步端口");
        check(rec_b->last_ip == "127.0.0.1", "B 记录到 A 的 IP");
    }
    check(!node_a->pairing().current_code().has_value(), "配对成功后码立即作废");

    // ---- 2. 心跳在线判定 ----
    node_b->heartbeat().probe_now(id_a);
    check(node_b->heartbeat().presence(id_a) == lb::Presence::Online, "B 判定 A 在线");
    node_a->heartbeat().probe_now(id_b);
    check(node_a->heartbeat().presence(id_b) == lb::Presence::Online, "A 判定 B 在线");

    // ---- 2b. 文件互传 ----
    const auto inbox_b = std::filesystem::path(dir_b) / "inbox";
    const auto src_path = base / "source.bin";
    const std::size_t src_size = 3u * 1024 * 1024 + 137; // 多块
    {
        std::ofstream out(src_path, std::ios::binary);
        for (std::size_t i = 0; i < src_size; ++i) {
            const char c = static_cast<char>((i * 31 + 7) & 0xFF);
            out.write(&c, 1);
        }
    }

    const lb::Result sent =
        node_a->transfer().send_file(id_b, src_path.string(), "", 5000);
    check(sent.success, "A 向 B 发送文件成功");
    if (!sent.success) std::printf("  原因: %s\n", sent.error.c_str());

    const auto received_path = inbox_b / "source.bin";
    check(std::filesystem::exists(received_path), "B 接收目录出现目标文件");
    check(!std::filesystem::exists(inbox_b / "source.bin.lbpart"), "传输成功后无残留 .lbpart");
    auto received_bytes = lb::pal::read_file(received_path.string());
    check(received_bytes.has_value() && received_bytes->size() == src_size,
          "接收文件大小一致");
    if (received_bytes && received_bytes->size() == src_size) {
        bool identical = true;
        std::ifstream in(src_path, std::ios::binary);
        for (std::size_t i = 0; i < src_size && identical; ++i) {
            char c = 0;
            in.read(&c, 1);
            identical = (static_cast<std::uint8_t>(c) == (*received_bytes)[i]);
        }
        check(identical, "接收文件内容逐字节一致");
    }

    // 断点续传：预置 50000 字节前缀的 .lbpart
    const auto resume_src = base / "resume.bin";
    const std::size_t resume_size = 200000;
    {
        std::ofstream out(resume_src, std::ios::binary);
        for (std::size_t i = 0; i < resume_size; ++i) {
            const char c = static_cast<char>((i * 17 + 3) & 0xFF);
            out.write(&c, 1);
        }
    }
    {
        std::ofstream part(inbox_b / "resume.bin.lbpart", std::ios::binary);
        for (std::size_t i = 0; i < 50000; ++i) {
            const char c = static_cast<char>((i * 17 + 3) & 0xFF);
            part.write(&c, 1);
        }
    }
    const lb::Result resumed =
        node_a->transfer().send_file(id_b, resume_src.string(), "", 5000);
    check(resumed.success, "断点续传发送成功");
    if (!resumed.success) std::printf("  原因: %s\n", resumed.error.c_str());
    check(!std::filesystem::exists(inbox_b / "resume.bin.lbpart"), "续传完成后 .lbpart 移除");
    auto resumed_bytes = lb::pal::read_file((inbox_b / "resume.bin").string());
    check(resumed_bytes.has_value() && resumed_bytes->size() == resume_size,
          "续传后文件大小一致");
    if (resumed_bytes && resumed_bytes->size() == resume_size) {
        bool identical = true;
        for (std::size_t i = 0; i < resume_size && identical; ++i)
            identical = ((*resumed_bytes)[i] == static_cast<std::uint8_t>((i * 17 + 3) & 0xFF));
        check(identical, "续传后文件内容逐字节一致");
    }

    // 路径穿越：发送端即拒绝
    const lb::Result evil =
        node_a->transfer().send_file(id_b, src_path.string(), "../evil.txt", 3000);
    check(!evil.success, "../evil.txt 被拒绝");
    check(!std::filesystem::exists(base / "evil.txt"), "未在接收目录之外生成文件");

    // 权限撤销：B 撤销对 A 的文件传输权限后拒绝
    if (const auto b_to_a = node_b->trust().get(id_a)) {
        lb::TrustRecord revoked = *b_to_a;
        revoked.permissions &= ~lb::kPermFileTransfer;
        check(node_b->trust().upsert(revoked).success, "B 撤销 A 的文件传输权限");
        const lb::Result denied =
            node_a->transfer().send_file(id_b, src_path.string(), "denied.bin", 5000);
        check(!denied.success, "权限撤销后传输被拒绝");
        check(!std::filesystem::exists(inbox_b / "denied.bin"), "被拒后未生成文件");

        lb::TrustRecord restored = *node_b->trust().get(id_a);
        restored.permissions |= lb::kPermFileTransfer;
        node_b->trust().upsert(restored);
    }

    // ---- 2c. 剪贴板同步 ----
    const std::string clip_text = "LinkBridge M4 剪贴板同步";
    const lb::ClipboardContent text_content{"text/plain", bytes_of(clip_text)};
    const lb::Result clip_sent = node_a->clipboard().push_to(id_b, text_content, 3000);
    check(clip_sent.success, "A 向 B 推送文本剪贴板成功");
    if (!clip_sent.success) std::printf("  原因: %s\n", clip_sent.error.c_str());
    check(clip_b->write_count() == 1, "B 本地剪贴板写入一次");
    check(clip_b->current().mime == "text/plain", "B 剪贴板类型为 text/plain");
    check(text_of(clip_b->current().data) == clip_text, "B 剪贴板内容与推送文本一致");

    // 幂等：同一内容重复推送不重复覆盖
    const lb::Result clip_again = node_a->clipboard().push_to(id_b, text_content, 3000);
    check(clip_again.success, "重复推送得到幂等成功应答");
    check(clip_b->write_count() == 1, "重复推送不重复写入本地剪贴板");

    // 回环抑制：B 写入后其 watcher 不应把同一内容回推给 A
    std::this_thread::sleep_for(std::chrono::milliseconds(1800));
    check(clip_b->write_count() == 1, "B watcher 未重复写入");
    check(clip_a->write_count() == 0, "回环被抑制，A 未被写入");

    // 图片（原始 DIB）
    lb::ByteBuf dib(96, 0);
    dib[0] = 40; // BITMAPINFOHEADER 大小，小端
    const lb::ClipboardContent image_content{"image/dib", dib};
    const lb::Result image_sent = node_a->clipboard().push_to(id_b, image_content, 3000);
    check(image_sent.success, "A 向 B 推送 image/dib 成功");
    check(clip_b->write_count() == 2, "B 写入图片一次");
    check(clip_b->current().mime == "image/dib", "B 剪贴板类型为 image/dib");
    check(clip_b->current().data.size() == dib.size(), "B 图片数据大小一致");

    // 超限：超过 B 的 max_bytes 被拒绝
    lb::ByteBuf oversized(66000, 0x5A); // > 64 KiB 但仍在帧上限内
    const lb::Result too_large =
        node_a->clipboard().push_to(id_b, lb::ClipboardContent{"text/plain", oversized}, 5000);
    check(!too_large.success, "超过 max_bytes 的推送被拒绝");
    check(clip_b->write_count() == 2, "超限内容未写入 B 剪贴板");

    // 权限撤销：B 撤销对 A 的剪贴板权限后拒绝
    if (const auto b_to_a = node_b->trust().get(id_a)) {
        lb::TrustRecord revoked = *b_to_a;
        revoked.permissions &= ~lb::kPermClipboard;
        check(node_b->trust().upsert(revoked).success, "B 撤销 A 的剪贴板权限");
        const lb::Result denied = node_a->clipboard().push_to(
            id_b, lb::ClipboardContent{"text/plain", bytes_of("denied")}, 3000);
        check(!denied.success, "权限撤销后剪贴板推送被拒绝");
        check(clip_b->write_count() == 2, "被拒后未写入 B 剪贴板");

        lb::TrustRecord restored = *node_b->trust().get(id_a);
        restored.permissions |= lb::kPermClipboard;
        node_b->trust().upsert(restored);
    }

    // 确认模式：无确认处理器时拒绝，开启自动接受后成功
    node_b->clipboard().set_auto_accept(false);
    const lb::Result confirm_denied = node_a->clipboard().push_to(
        id_b, lb::ClipboardContent{"text/plain", bytes_of("confirm-needed")}, 3000);
    check(!confirm_denied.success, "confirm 模式无处理器时拒绝");
    check(clip_b->write_count() == 2, "拒绝后未写入 B 剪贴板");

    node_b->clipboard().set_auto_accept(true);
    const lb::Result confirm_ok = node_a->clipboard().push_to(
        id_b, lb::ClipboardContent{"text/plain", bytes_of("confirm-ok")}, 3000);
    check(confirm_ok.success, "自动接受开启后推送成功");
    check(clip_b->write_count() == 3, "确认接受后写入一次");

    // 本地变化自动推送：A watcher 读取本地内容并推送到 B
    const std::string watched_text = "watcher-propagated";
    clip_a->set_local(lb::ClipboardContent{"text/plain", bytes_of(watched_text)});
    const auto local_read = node_a->clipboard().read_local();
    check(local_read.has_value() && local_read->mime == "text/plain", "A 读取本地剪贴板");
    check(wait_until([&] { return clip_b->write_count() >= 4; }, 8000),
          "A 本地变化后自动推送到 B");
    check(text_of(clip_b->current().data) == watched_text, "B 收到 watcher 推送的文本");

    std::this_thread::sleep_for(std::chrono::milliseconds(1800));
    check(clip_b->write_count() == 4, "watcher 推送后 B 未重复写入");
    check(clip_a->write_count() == 0, "watcher 推送未触发回环写入");

    // ---- 2d. 屏幕共享（只读）----
    std::atomic<int> screen_started{0};
    std::atomic<int> screen_ended{0};
    auto screen_started_sub =
        node_b->events().subscribe<lb::ScreenSessionStarted>(
            [&](const lb::ScreenSessionStarted&) { ++screen_started; });
    auto screen_ended_sub =
        node_b->events().subscribe<lb::ScreenSessionEnded>(
            [&](const lb::ScreenSessionEnded&) { ++screen_ended; });

    const auto screen_out = base / "screen_a";
    const auto view1 = node_a->screen().view(id_b, screen_out.string(), 800);
    check(view1.has_value(), "A 查看 B 屏幕成功");
    if (!view1) std::printf("  原因: %s\n", view1.error.c_str());
    if (view1) {
        check(view1->frames >= 3, "查看会话接收多帧");
        check(screen_b->captures() >= view1->frames, "B 采集次数覆盖已发送帧数");
        check(!view1->last_frame_path.empty(), "返回最新帧路径");
    }
    check(screen_started.load() >= 1, "B 发布屏幕会话开始事件");
    check(wait_until([&] { return screen_ended.load() >= 1; }, 3000),
          "B 发布屏幕会话结束事件");
    (void)screen_started_sub;
    (void)screen_ended_sub;
    const auto latest_bmp = screen_out / "latest.bmp";
    check(std::filesystem::exists(latest_bmp), "最新帧已保存为 BMP");
    const auto bmp_bytes = lb::pal::read_file(latest_bmp.string());
    check(bmp_bytes.has_value() && bmp_bytes->size() == 14 + 40 + 4 * 3 * 4,
          "BMP 文件大小与 DIB 一致");
    if (bmp_bytes && bmp_bytes->size() >= 2)
        check((*bmp_bytes)[0] == 'B' && (*bmp_bytes)[1] == 'M', "BMP 文件头正确");

    // 权限撤销：B 撤销对 A 的屏幕共享权限后拒绝
    if (const auto b_to_a = node_b->trust().get(id_a)) {
        lb::TrustRecord revoked = *b_to_a;
        revoked.permissions &= ~lb::kPermScreenShare;
        check(node_b->trust().upsert(revoked).success, "B 撤销 A 的屏幕共享权限");
        const auto denied = node_a->screen().view(id_b, screen_out.string(), 400);
        check(!denied.has_value(), "权限撤销后屏幕查看被拒绝");

        lb::TrustRecord restored = *node_b->trust().get(id_a);
        restored.permissions |= lb::kPermScreenShare;
        node_b->trust().upsert(restored);
    }

    // 每会话确认：无确认处理器时拒绝，开启自动接受后成功
    node_b->screen().set_auto_accept(false);
    const auto screen_confirm_denied = node_a->screen().view(id_b, screen_out.string(), 400);
    check(!screen_confirm_denied.has_value(), "未确认的屏幕查看被拒绝");

    node_b->screen().set_auto_accept(true);
    const auto screen_confirm_ok = node_a->screen().view(id_b, screen_out.string(), 400);
    check(screen_confirm_ok.has_value() && screen_confirm_ok->frames > 0,
          "自动接受后屏幕查看成功");

    // ---- 2e. 文件夹同步（M6）----
    write_text(sync_a / "hello.txt", "hello from A");
    write_text(sync_a / "docs" / "note.txt", "nested note");
    lb::SyncStats st1;
    check(node_a->sync().sync_with(id_b, 5000, &st1).success, "A 发起初始同步成功");
    check(st1.pushed == 2 && st1.pulled == 0, "初始同步推送两个文件");
    check(read_text(sync_b / "hello.txt") == "hello from A", "B 收到 hello.txt");
    check(read_text(sync_b / "docs" / "note.txt") == "nested note", "B 收到子目录文件");
    check(node_a->sync().index_size() >= 2 && node_b->sync().index_size() >= 2,
          "两端写入同步索引");

    // 增量：B 修改既有文件并新增文件 → A
    write_text(sync_b / "hello.txt", "hello from B");
    write_text(sync_b / "new_b.txt", "created on B");
    lb::SyncStats st2;
    check(node_b->sync().sync_with(id_a, 5000, &st2).success, "B 发起增量同步成功");
    check(st2.pushed == 2 && st2.pulled == 0, "增量同步推送修改与新增");
    check(read_text(sync_a / "hello.txt") == "hello from B", "A 收到修改后的 hello.txt");
    check(read_text(sync_a / "new_b.txt") == "created on B", "A 收到新增文件");

    // 冲突：双方同时修改同一文件 → keep_both
    write_text(sync_a / "hello.txt", "A version");
    write_text(sync_b / "hello.txt", "B version");
    lb::SyncStats st3;
    check(node_a->sync().sync_with(id_b, 5000, &st3).success, "存在冲突时同步仍成功");
    check(st3.conflicts == 1, "检测到 1 个冲突");
    check(read_text(sync_a / "hello.txt") == "A version", "A 保留本端版本");
    check(read_text(sync_b / "hello.txt") == "B version", "B 保留本端版本");
    const auto conflicts_a = conflict_copies(sync_a, "hello");
    const auto conflicts_b = conflict_copies(sync_b, "hello");
    check(conflicts_a.size() == 1, "A 生成 1 个冲突副本");
    check(conflicts_b.size() == 1, "B 生成 1 个冲突副本");
    if (!conflicts_a.empty())
        check(read_text(conflicts_a[0]) == "B version", "A 的冲突副本是对端版本");
    if (!conflicts_b.empty())
        check(read_text(conflicts_b[0]) == "A version", "B 的冲突副本是对端版本");

    // 冲突路径已标记，不再重复判定或互传副本
    lb::SyncStats st4;
    check(node_a->sync().sync_with(id_b, 5000, &st4).success, "冲突后再次同步成功");
    check(st4.conflicts == 0, "同一冲突不重复判定");
    check(conflict_copies(sync_a, "hello").size() == 1 &&
              conflict_copies(sync_b, "hello").size() == 1,
          "冲突副本未重复生成也未互传");

    // M7：冲突列出与解决（A 保留对端版本）
    {
        const auto pending = node_a->sync().conflicts();
        check(pending.size() == 1 && pending[0].path == "hello.txt" &&
                  pending[0].copies.size() == 1,
              "A 列出未解决的冲突");
        check(node_a->sync().resolve_conflict("hello.txt", "keep_remote").success,
              "A 以对端版本解决冲突");
        check(read_text(sync_a / "hello.txt") == "B version", "keep_remote 用副本覆盖原文件");
        check(node_a->sync().conflicts().empty(), "解决后冲突列表清空");

        const auto pending_b = node_b->sync().conflicts();
        check(pending_b.size() == 1 && pending_b[0].path == "hello.txt",
              "B 同样列出未解决的冲突");
        check(node_b->sync().resolve_conflict("hello.txt", "keep_local").success,
              "B 保留本端版本解决冲突");
        check(node_b->sync().conflicts().empty(), "B 解决后冲突列表清空");
    }

    // 忽略规则与单文件大小上限
    write_text(sync_a / "skip.tmp", "temp");
    {
        std::ofstream big(sync_a / "big.bin", std::ios::binary);
        const std::string chunk(1024 * 1024, 'x');
        big.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        big.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
    }
    lb::SyncStats st5;
    check(node_a->sync().sync_with(id_b, 5000, &st5).success, "忽略规则下同步成功");
    check(st5.pushed == 0 && st5.pulled == 0, "忽略/超限文件不参与同步");
    check(!std::filesystem::exists(sync_b / "skip.tmp"), "*.tmp 未被同步");
    check(!std::filesystem::exists(sync_b / "big.bin"), "超过单文件上限的文件未被同步");

    // M7：删除传播 + 回收站
    std::filesystem::remove(sync_b / "new_b.txt");
    lb::SyncStats st6;
    check(node_a->sync().sync_with(id_b, 5000, &st6).success, "删除后同步成功");
    check(st6.deleted == 1, "统计到 1 个删除传播");
    check(!std::filesystem::exists(sync_a / "new_b.txt"), "A 跟随 B 删除文件");
    check(node_a->sync().history("new_b.txt").size() == 1, "被删除文件已归档到回收站");
    check(node_a->sync().restore_version("new_b.txt").success, "可从回收站恢复文件");
    check(read_text(sync_a / "new_b.txt") == "created on B", "恢复内容正确");
    check(node_a->sync().sync_with(id_b, 5000).success, "恢复后再次同步成功");
    check(read_text(sync_b / "new_b.txt") == "created on B", "恢复后的文件复活对端");

    // 删除 vs 修改冲突：B 删除、A 修改 → 修改优先
    write_text(sync_a / "hello.txt", "A 修改后");
    std::filesystem::remove(sync_b / "hello.txt");
    lb::SyncStats st7;
    check(node_a->sync().sync_with(id_b, 5000, &st7).success, "删除与修改并发时同步成功");
    check(read_text(sync_a / "hello.txt") == "A 修改后", "A 保留修改后的文件");
    check(read_text(sync_b / "hello.txt") == "A 修改后", "B 的删除被修改优先覆盖（文件复活）");

    // 权限撤销：B 撤销 A 的同步权限后拒绝
    if (const auto b_to_a = node_b->trust().get(id_a)) {
        lb::TrustRecord revoked = *b_to_a;
        revoked.permissions &= ~lb::kPermSync;
        check(node_b->trust().upsert(revoked).success, "B 撤销 A 的同步权限");
        check(!node_a->sync().sync_with(id_b, 3000).success, "权限撤销后同步被拒绝");

        lb::TrustRecord restored = *node_b->trust().get(id_a);
        restored.permissions |= lb::kPermSync;
        node_b->trust().upsert(restored);
    }

    // 自动同步：文件监听 + 1 秒防抖触发
    node_a->sync().start_auto();
    std::this_thread::sleep_for(std::chrono::milliseconds(500)); // 等待监听就绪
    write_text(sync_a / "auto.txt", "auto triggered");
    check(wait_until([&] { return std::filesystem::exists(sync_b / "auto.txt"); }, 10000),
          "文件变化触发自动同步");
    node_a->sync().stop_auto();

    // ---- 3. 错误配对码被拒 ----
    const lb::PairCode code2 = node_a->pairing().generate_code(300, 3);
    const std::string wrong = code2.value == "000000" ? "123456" : "000000";
    const lb::Result bad =
        node_b->pairing().pair_with("127.0.0.1", node_a->pair_port(), wrong);
    check(!bad.success, "错误配对码被拒绝");
    check(wait_until([&] { return !node_a->pairing().current_code().has_value(); }, 3000),
          "错误尝试后配对码作废");
    check(node_a->trust().size() == 1, "拒绝后 A 信任库未变化");

    // ---- 4. 重启后身份与信任保持 ----
    const std::string id_a_before = node_a->identity().device_id();
    const std::string fp_a_before = node_a->identity().fingerprint();
    node_a->stop();
    {
        auto reopened = lb::Node::create(options_a);
        check(reopened.has_value(), "节点 A 可重新打开数据目录");
        if (reopened) {
            check((*reopened)->identity().device_id() == id_a_before, "重启后设备 ID 不变");
            check((*reopened)->identity().fingerprint() == fp_a_before, "重启后指纹不变");
            check((*reopened)->trust().size() == 1, "重启后信任库保留");
        }
    }
    node_b->stop();

    std::filesystem::remove_all(base);

    std::printf("--------------------------------\n");
    if (g_failures == 0) {
        std::printf("端到端测试全部通过\n");
        return 0;
    }
    std::printf("端到端测试失败 %d 项\n", g_failures);
    return 1;
}
