#pragma once
// 受信任设备库：整体经 DPAPI 加密后落盘，MAC/IP 等字段不明文存于磁盘。
#include "core/common/types.h"

#include <optional>
#include <string>
#include <vector>

namespace lb {

class ByteReader;

// 权限位（v0 仅记录，M3+ 由 ACL 模块消费）
enum PermissionFlags : std::uint32_t {
    kPermFileTransfer = 1u << 0,
    kPermClipboard = 1u << 1,
    kPermRemoteFs = 1u << 2,
    kPermScreenControl = 1u << 3,
    kPermScreenShare = 1u << 4,
    kPermSync = 1u << 5,
    kPermDefault = kPermFileTransfer | kPermClipboard | kPermScreenShare | kPermSync,
};

struct TrustRecord {
    DeviceId device_id;
    std::string name;
    Platform platform = Platform::Windows;
    ByteBuf public_key;      // 65 字节身份公钥（心跳/配对校验用）
    std::string last_ip;     // 坐标：仅内存使用；落盘时整体加密
    std::string mac;
    std::string ssh_public_key;
    int heartbeat_port = 45711; // 对端通告的心跳端口
    int ssh_port = 45712;       // 对端通告的 SSH 端口（M3 使用）
    int transfer_port = 45713;  // 对端通告的文件传输端口
    int clipboard_port = 45714; // 对端通告的剪贴板通道端口
    int screen_port = 45715;    // 对端通告的屏幕共享通道端口
    int sync_port = 45716;      // 对端通告的文件夹同步通道端口
    std::int64_t paired_at = 0;
    std::uint32_t permissions = kPermDefault;

    ByteBuf to_bytes() const;
    static Expected<TrustRecord> from_bytes(ByteReader& reader);
};

class TrustStore {
public:
    explicit TrustStore(const std::string& dir);

    Result load();
    Result save() const;

    std::vector<TrustRecord> all() const;
    std::optional<TrustRecord> get(const DeviceId& id) const;
    bool has(const DeviceId& id) const;
    std::size_t size() const;

    Result upsert(const TrustRecord& record);
    Result remove(const DeviceId& id);
    Result update_last_ip(const DeviceId& id, const std::string& ip);

private:
    std::string path_;
    std::vector<TrustRecord> records_;
};

} // namespace lb
