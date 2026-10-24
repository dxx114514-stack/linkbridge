#pragma once
// 文件互传：在 STS 加密通道（等价 TLS）之上实现 OFFER/ACCEPT/CHUNK/ACK/DONE 协议。
// 设计文档 §3.4 以 SFTP 承载；v0 无 libssh，改用同一套消息语义自研实现，
// 断点续传、块级 CRC32C、整文件 SHA-256 与临时文件 <name>.lbpart 均保留。
#include "core/common/event_bus.h"
#include "core/common/stream.h"
#include "core/common/types.h"
#include "core/config/config.h"
#include "core/identity/identity.h"
#include "core/trust/trust_store.h"

#include <mutex>
#include <set>
#include <string>

namespace lb {

struct TransferProgress {
    std::string task_id;
    std::string file_name;
    std::uint64_t transferred = 0;
    std::uint64_t total = 0;
    bool outgoing = false;
};

struct TransferCompleted {
    std::string task_id;
    std::string file_name;
    std::string path;
    std::uint64_t size = 0;
    bool outgoing = false;
};

struct TransferFailed {
    std::string task_id;
    std::string file_name;
    std::string reason;
    bool outgoing = false;
};

class TransferService {
public:
    TransferService(const Identity& identity, TrustStore& trust, EventBus& events, IDialer& dialer,
                    const TransferConfig& config, std::string inbox_dir);

    // 发起方：阻塞完成一次发送。remote_name 为空时取本地文件名。
    Result send_file(const DeviceId& peer, const std::string& local_path,
                     const std::string& remote_name, int timeout_ms);

    // 响应方：处理一条已建立的入站连接（由监听线程调用）。
    void serve(std::unique_ptr<ByteStream> stream);

    const std::string& inbox_dir() const { return inbox_dir_; }

    // 纯函数：拒绝路径分隔符、控制字符、Windows 保留名、超长名
    static Expected<std::string> sanitize_file_name(const std::string& name);

    // Castagnoli CRC32C（校验向量 "123456789" → 0xE3069283）
    static std::uint32_t crc32c(const void* data, size_t n);
    static std::uint32_t crc32c(const ByteBuf& b) { return crc32c(b.data(), b.size()); }

private:
    void publish_progress(const std::string& task_id, const std::string& name,
                          std::uint64_t transferred, std::uint64_t total, bool outgoing);
    void publish_failed(const std::string& task_id, const std::string& name,
                        const std::string& reason, bool outgoing);

    // 选择不重名的落盘路径，并检测/清理残留 .lbpart 以决定续传偏移
    std::string choose_target(const std::string& name, std::uint64_t size, std::uint64_t& resume);
    void release_target(const std::string& final_path);

    const Identity& identity_;
    TrustStore& trust_;
    EventBus& events_;
    IDialer& dialer_;
    TransferConfig config_;
    std::string inbox_dir_;

    std::mutex inbox_mu_;
    std::set<std::string> active_temps_;
};

} // namespace lb
