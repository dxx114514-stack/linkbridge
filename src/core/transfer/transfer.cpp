#include "core/transfer/transfer.h"

#include "core/common/bytes.h"
#include "core/crypto/crypto.h"
#include "core/platform/platform.h"
#include "net/secure_channel.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace lb {
namespace {

constexpr std::uint8_t kOffer = 0x31;
constexpr std::uint8_t kAccept = 0x32;
constexpr std::uint8_t kChunk = 0x33;
constexpr std::uint8_t kAck = 0x34;
constexpr std::uint8_t kDone = 0x35;
constexpr std::uint8_t kFail = 0x36;
constexpr std::uint8_t kOk = 0x37;

constexpr std::uint64_t kMaxFileSize = 64ull * 1024 * 1024 * 1024; // 64 GiB
constexpr std::uint32_t kMinChunkSize = 4096;
constexpr std::uint32_t kMaxChunkSize = 8u * 1024 * 1024;
constexpr size_t kMaxNameBytes = 200;
constexpr size_t kChunkFrameSlack = 1024;

std::filesystem::path to_path(const std::string& s) {
    return std::filesystem::path(std::u8string(s.begin(), s.end()));
}

std::string file_basename(const std::string& path) {
    const auto pos = path.find_last_of("/\\");
    return pos == std::string::npos ? path : path.substr(pos + 1);
}

std::string make_task_id() {
    const ByteBuf raw = crypto::random_bytes(8);
    return to_hex(raw);
}

Expected<ByteBuf> hash_file(const std::string& path) {
    std::ifstream in(to_path(path), std::ios::binary);
    if (!in) return Expected<ByteBuf>::fail("打开本地文件失败");
    crypto::Sha256Hasher hasher;
    std::array<char, 65536> buf{};
    while (in) {
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        const auto got = in.gcount();
        if (got > 0 && !hasher.update(buf.data(), static_cast<size_t>(got)))
            return Expected<ByteBuf>::fail("计算文件哈希失败");
    }
    if (!in.eof()) return Expected<ByteBuf>::fail("读取本地文件失败");
    ByteBuf digest = hasher.finish();
    if (digest.size() != crypto::kSha256Len)
        return Expected<ByteBuf>::fail("计算文件哈希失败");
    return Expected<ByteBuf>::success(std::move(digest));
}

ByteBuf build_fail(const std::string& reason) {
    ByteWriter w;
    w.u8(kFail);
    w.str(reason);
    return w.buf();
}

} // namespace

// ---------------- 纯函数 ----------------

std::uint32_t TransferService::crc32c(const void* data, size_t n) {
    static const std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1u) ? (0x82F63B78u ^ (c >> 1)) : (c >> 1);
            t[i] = c;
        }
        return t;
    }();

    const auto* p = static_cast<const std::uint8_t*>(data);
    std::uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) crc = table[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

Expected<std::string> TransferService::sanitize_file_name(const std::string& name) {
    if (name.empty()) return Expected<std::string>::fail("文件名为空");
    if (name.size() > kMaxNameBytes) return Expected<std::string>::fail("文件名超过 200 字节");
    if (name == "." || name == "..") return Expected<std::string>::fail("非法文件名");

    for (unsigned char c : name) {
        if (c < 0x20 || c == 0x7F)
            return Expected<std::string>::fail("文件名含控制字符");
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' ||
            c == '>' || c == '|')
            return Expected<std::string>::fail("文件名含非法字符: " + name);
    }
    if (name.back() == ' ' || name.back() == '.')
        return Expected<std::string>::fail("文件名不能以空格或点结尾");

    std::string stem = name.substr(0, name.find('.'));
    std::transform(stem.begin(), stem.end(), stem.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    static const std::set<std::string> kReserved = {
        "con",  "prn",  "aux",  "nul",  "com1", "com2", "com3", "com4", "com5",
        "com6", "com7", "com8", "com9", "lpt1", "lpt2", "lpt3", "lpt4", "lpt5",
        "lpt6", "lpt7", "lpt8", "lpt9"};
    if (kReserved.count(stem)) return Expected<std::string>::fail("Windows 保留设备名不可用");

    return Expected<std::string>::success(name);
}

// ---------------- TransferService ----------------

TransferService::TransferService(const Identity& identity, TrustStore& trust, EventBus& events,
                                 IDialer& dialer, const TransferConfig& config,
                                 std::string inbox_dir)
    : identity_(identity),
      trust_(trust),
      events_(events),
      dialer_(dialer),
      config_(config),
      inbox_dir_(std::move(inbox_dir)) {}

void TransferService::publish_progress(const std::string& task_id, const std::string& name,
                                       std::uint64_t transferred, std::uint64_t total,
                                       bool outgoing) {
    events_.publish(TransferProgress{task_id, name, transferred, total, outgoing});
}

void TransferService::publish_failed(const std::string& task_id, const std::string& name,
                                     const std::string& reason, bool outgoing) {
    events_.publish(TransferFailed{task_id, name, reason, outgoing});
}

Result TransferService::send_file(const DeviceId& peer, const std::string& local_path,
                                  const std::string& remote_name, int timeout_ms) {
    const std::string task_id = make_task_id();
    std::string name = remote_name.empty() ? file_basename(local_path) : remote_name;

    auto sane = sanitize_file_name(name);
    if (!sane) {
        publish_failed(task_id, name, sane.error, true);
        return Result::fail(sane.error);
    }
    name = *sane;

    const auto record = trust_.get(peer);
    if (!record) {
        publish_failed(task_id, name, "目标设备不在信任库中", true);
        return Result::fail("目标设备不在信任库中");
    }
    if (record->last_ip.empty()) {
        publish_failed(task_id, name, "尚未学习到对端 IP，请先运行 daemon 完成心跳", true);
        return Result::fail("尚未学习到对端 IP，请先运行 daemon 完成心跳");
    }
    const int port = record->transfer_port > 0 ? record->transfer_port : 45713;

    std::error_code ec;
    const auto path = to_path(local_path);
    if (!std::filesystem::is_regular_file(path, ec) || ec) {
        publish_failed(task_id, name, "本地文件不存在或不是普通文件", true);
        return Result::fail("本地文件不存在或不是普通文件: " + local_path);
    }
    const std::uint64_t size = static_cast<std::uint64_t>(std::filesystem::file_size(path, ec));
    if (ec) {
        publish_failed(task_id, name, "读取本地文件大小失败", true);
        return Result::fail("读取本地文件大小失败: " + local_path);
    }
    if (size > kMaxFileSize) {
        publish_failed(task_id, name, "文件超过 64GiB 上限", true);
        return Result::fail("文件超过 64GiB 上限");
    }

    auto digest = hash_file(local_path);
    if (!digest) {
        publish_failed(task_id, name, digest.error, true);
        return Result::fail(digest.error);
    }

    auto dialed = dialer_.dial(record->last_ip, port, timeout_ms);
    if (!dialed) {
        publish_failed(task_id, name, "连接对端失败: " + dialed.error, true);
        return Result::fail("连接对端失败: " + dialed.error);
    }
    std::unique_ptr<ByteStream> stream = std::move(*dialed);

    auto sts = sts_client(*stream, identity_, trust_, peer);
    if (!sts) {
        stream->close();
        publish_failed(task_id, name, sts.error, true);
        return Result::fail(sts.error);
    }

    SessionKeys keys = std::move(sts->keys);
    std::uint64_t seq = 0;
    auto send_msg = [&](const ByteBuf& msg) { return seal_send(*stream, keys, seq, msg); };
    auto recv_msg = [&](size_t max_len) { return seal_recv(*stream, keys, seq, max_len); };
    auto abort = [&](const std::string& why) {
        stream->close();
        publish_failed(task_id, name, why, true);
        return Result::fail(why);
    };

    const std::uint32_t chunk_size =
        config_.chunk_size >= static_cast<int>(kMinChunkSize) &&
                config_.chunk_size <= static_cast<int>(kMaxChunkSize)
            ? static_cast<std::uint32_t>(config_.chunk_size)
            : 1048576u;

    {
        ByteWriter w;
        w.u8(kOffer);
        w.str(task_id);
        w.str(name);
        w.u64(size);
        w.i64(pal::file_mtime(local_path));
        w.bytes(*digest);
        w.u32(chunk_size);
        if (!send_msg(w.buf())) return abort("发送传输请求失败");
    }

    auto resp = recv_msg(65536);
    if (!resp) return abort("等待对端应答失败");
    std::uint64_t resume = 0;
    {
        ByteReader r(*resp);
        const std::uint8_t type = r.u8();
        if (type == kFail) {
            const std::string reason = r.str();
            return abort(reason.empty() ? "对端拒绝接收" : reason);
        }
        if (type != kAccept) return abort("对端应答类型错误");
        resume = r.u64();
        if (!r.ok() || resume > size) return abort("对端续传位置非法");
    }

    std::ifstream in(path, std::ios::binary);
    if (!in) return abort("打开本地文件失败");
    if (resume > 0) {
        in.seekg(static_cast<std::streamoff>(resume));
        if (!in) return abort("定位续传偏移失败");
    }

    std::uint64_t offset = resume;
    const auto started_at = std::chrono::steady_clock::now();
    ByteBuf block(chunk_size, 0);
    while (offset < size) {
        const size_t want = static_cast<size_t>(std::min<std::uint64_t>(chunk_size, size - offset));
        in.read(reinterpret_cast<char*>(block.data()), static_cast<std::streamsize>(want));
        if (static_cast<size_t>(in.gcount()) != want) return abort("读取本地文件失败");

        const ByteBuf payload(block.begin(), block.begin() + static_cast<long>(want));
        ByteWriter cw;
        cw.u8(kChunk);
        cw.u64(offset);
        cw.u32(crc32c(payload));
        cw.bytes(payload);
        if (!send_msg(cw.buf())) return abort("发送数据块失败");

        auto ack = recv_msg(65536);
        if (!ack) return abort("等待对端确认失败");
        ByteReader ar(*ack);
        if (ar.u8() != kAck) return abort("对端确认类型错误");
        const std::uint64_t next = ar.u64();
        if (!ar.ok() || next != offset + want) return abort("对端确认偏移不匹配");
        offset = next;

        publish_progress(task_id, name, offset, size, true);

        if (config_.rate_limit_kbps > 0) {
            const double bytes_per_sec = static_cast<double>(config_.rate_limit_kbps) * 1024.0 / 8.0;
            const double target_sec = static_cast<double>(offset - resume) / bytes_per_sec;
            const double elapsed =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - started_at).count();
            if (target_sec > elapsed)
                std::this_thread::sleep_for(std::chrono::duration<double>(target_sec - elapsed));
        }
    }

    {
        ByteWriter w;
        w.u8(kDone);
        w.u64(size);
        w.bytes(*digest);
        if (!send_msg(w.buf())) return abort("发送完成包失败");
    }

    auto final_resp = recv_msg(65536);
    if (!final_resp) return abort("等待对端最终确认失败");
    {
        ByteReader r(*final_resp);
        const std::uint8_t type = r.u8();
        if (type == kFail) {
            const std::string reason = r.str();
            return abort(reason.empty() ? "对端校验失败" : reason);
        }
        if (type != kOk) return abort("对端最终确认类型错误");
        const std::string remote_path = r.str();
        if (!r.ok()) return abort("对端最终确认格式错误");
        events_.publish(TransferCompleted{task_id, name, remote_path, size, true});
    }

    stream->close();
    return Result::ok();
}

std::string TransferService::choose_target(const std::string& name, std::uint64_t size,
                                           std::uint64_t& resume) {
    const auto dot = name.find_last_of('.');
    const std::string stem = (dot == std::string::npos || dot == 0) ? name : name.substr(0, dot);
    const std::string ext = (dot == std::string::npos || dot == 0) ? "" : name.substr(dot);

    for (int i = 0; i < 10000; ++i) {
        const std::string candidate_name =
            i == 0 ? name : stem + " (" + std::to_string(i) + ")" + ext;
        const std::string candidate = pal::join_path(inbox_dir_, candidate_name);
        const std::string temp = candidate + ".lbpart";
        if (active_temps_.count(temp)) continue;
        if (pal::file_exists(candidate)) continue;

        if (pal::file_exists(temp)) {
            std::error_code ec;
            const auto temp_size =
                static_cast<std::uint64_t>(std::filesystem::file_size(to_path(temp), ec));
            if (ec || temp_size > size) {
                pal::remove_file(temp);
                resume = 0;
            } else {
                resume = temp_size;
            }
        } else {
            resume = 0;
        }
        active_temps_.insert(temp);
        return candidate;
    }
    // 极端情况：兜底使用带时间戳的名字
    const std::string fallback = pal::join_path(
        inbox_dir_, stem + "." + std::to_string(now_secs()) + ext);
    active_temps_.insert(fallback + ".lbpart");
    resume = 0;
    return fallback;
}

void TransferService::release_target(const std::string& final_path) {
    std::lock_guard<std::mutex> lk(inbox_mu_);
    active_temps_.erase(final_path + ".lbpart");
}

void TransferService::serve(std::unique_ptr<ByteStream> stream) {
    if (!stream) return;

    auto sts = sts_server(*stream, identity_, trust_);
    if (!sts) {
        stream->close();
        return;
    }

    SessionKeys keys = std::move(sts->keys);
    std::uint64_t seq = 0;
    auto send_msg = [&](const ByteBuf& msg) { return seal_send(*stream, keys, seq, msg); };
    auto recv_msg = [&](size_t max_len) { return seal_recv(*stream, keys, seq, max_len); };

    const auto record = trust_.get(sts->peer_id);
    if (!record || (record->permissions & kPermFileTransfer) == 0) {
        send_msg(build_fail("对端未授予文件传输权限"));
        stream->close();
        return;
    }

    auto offer = recv_msg(65536);
    if (!offer) {
        stream->close();
        return;
    }

    std::string task_id;
    std::string name;
    std::uint64_t size = 0;
    std::uint32_t chunk_size = 0;
    ByteBuf digest;
    {
        ByteReader r(*offer);
        if (r.u8() != kOffer) {
            send_msg(build_fail("协议错误"));
            stream->close();
            return;
        }
        task_id = r.str();
        name = r.str();
        size = r.u64();
        (void)r.i64(); // mtime：v0 仅保留字段，落盘后不还原时间戳
        digest = r.bytes();
        chunk_size = r.u32();
        if (!r.ok() || digest.size() != crypto::kSha256Len || size > kMaxFileSize ||
            chunk_size < kMinChunkSize || chunk_size > kMaxChunkSize) {
            send_msg(build_fail("传输请求字段非法"));
            stream->close();
            return;
        }
    }

    auto sane = sanitize_file_name(name);
    if (!sane) {
        const std::string reason = sane.error;
        send_msg(build_fail(reason));
        publish_failed(task_id, name, reason, false);
        stream->close();
        return;
    }
    name = *sane;

    if (auto r = pal::ensure_dir(inbox_dir_); !r) {
        send_msg(build_fail("接收目录不可用"));
        publish_failed(task_id, name, r.error, false);
        stream->close();
        return;
    }

    std::uint64_t resume = 0;
    std::string final_path;
    {
        std::lock_guard<std::mutex> lk(inbox_mu_);
        final_path = choose_target(name, size, resume);
    }
    const std::string temp_path = final_path + ".lbpart";

    auto finish_failed = [&](const std::string& reason, bool send) {
        if (send) send_msg(build_fail(reason));
        publish_failed(task_id, name, reason, false);
        release_target(final_path);
        stream->close();
    };

    {
        ByteWriter w;
        w.u8(kAccept);
        w.u64(resume);
        if (!send_msg(w.buf())) {
            finish_failed("发送接收应答失败", false);
            return;
        }
    }

    std::ofstream out(to_path(temp_path), std::ios::binary | std::ios::app);
    if (!out) {
        finish_failed("创建临时文件失败", true);
        return;
    }
    if (resume > 0) publish_progress(task_id, name, resume, size, false);

    std::uint64_t offset = resume;
    while (true) {
        auto msg = recv_msg(static_cast<size_t>(chunk_size) + kChunkFrameSlack);
        if (!msg) {
            // 连接中断：保留 .lbpart 供下次续传
            publish_failed(task_id, name, "连接中断", false);
            release_target(final_path);
            stream->close();
            return;
        }

        ByteReader r(*msg);
        const std::uint8_t type = r.u8();
        if (type == kChunk) {
            const std::uint64_t off = r.u64();
            const std::uint32_t crc = r.u32();
            const ByteBuf data = r.bytes();
            if (!r.ok()) {
                out.close();
                pal::remove_file(temp_path);
                finish_failed("数据块格式错误", true);
                return;
            }
            if (off != offset) {
                out.close();
                pal::remove_file(temp_path);
                finish_failed("数据块偏移不连续", true);
                return;
            }
            if (offset + data.size() > size) {
                out.close();
                pal::remove_file(temp_path);
                finish_failed("数据块超出声明大小", true);
                return;
            }
            if (crc32c(data) != crc) {
                out.close();
                pal::remove_file(temp_path);
                finish_failed("数据块校验失败", true);
                return;
            }
            if (!data.empty()) {
                out.write(reinterpret_cast<const char*>(data.data()),
                          static_cast<std::streamsize>(data.size()));
                if (!out) {
                    out.close();
                    pal::remove_file(temp_path);
                    finish_failed("写入临时文件失败", true);
                    return;
                }
            }
            offset += data.size();
            publish_progress(task_id, name, offset, size, false);

            ByteWriter ack;
            ack.u8(kAck);
            ack.u64(offset);
            if (!send_msg(ack.buf())) {
                out.close();
                release_target(final_path);
                publish_failed(task_id, name, "发送确认失败", false);
                stream->close();
                return;
            }
        } else if (type == kDone) {
            const std::uint64_t declared = r.u64();
            const ByteBuf done_digest = r.bytes();
            if (!r.ok() || declared != size || done_digest.size() != crypto::kSha256Len) {
                out.close();
                pal::remove_file(temp_path);
                finish_failed("完成包格式错误", true);
                return;
            }
            out.flush();
            out.close();
            if (offset != size) {
                finish_failed("数据不完整", true);
                return;
            }
            auto actual = hash_file(temp_path);
            if (!actual || *actual != digest) {
                pal::remove_file(temp_path);
                finish_failed("整文件 SHA-256 校验失败", true);
                return;
            }
            if (auto rn = pal::rename_file(temp_path, final_path); !rn) {
                pal::remove_file(temp_path);
                finish_failed("落盘失败: " + rn.error, true);
                return;
            }
            ByteWriter ok;
            ok.u8(kOk);
            ok.str(final_path);
            ok.u8(resume > 0 ? 1 : 0);
            send_msg(ok.buf());
            release_target(final_path);
            events_.publish(TransferCompleted{task_id, name, final_path, size, false});
            stream->close();
            return;
        } else if (type == kFail) {
            const std::string reason = r.str();
            out.close();
            pal::remove_file(temp_path);
            publish_failed(task_id, name, reason.empty() ? "对端取消传输" : reason, false);
            release_target(final_path);
            stream->close();
            return;
        } else {
            out.close();
            pal::remove_file(temp_path);
            finish_failed("协议错误", true);
            return;
        }
    }
}

} // namespace lb
