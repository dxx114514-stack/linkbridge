#pragma once
// 受信任设备库：整体经 DPAPI 加密后落盘，MAC/IP 等字段不明文存于磁盘。
#include "core/common/types.h"

#include <optional>
#include <string>
#include <vector>

namespace lb {

class ByteReader;

// 权限位（逐设备独立授权；每次变更都写入审计日志）
enum PermissionFlags : std::uint32_t {
    kPermFileTransfer = 1u << 0,
    kPermClipboard = 1u << 1,
    kPermRemoteFs = 1u << 2,
    kPermScreenControl = 1u << 3,
    kPermScreenShare = 1u << 4,
    kPermSync = 1u << 5,
    kPermNotifyMirror = 1u << 6, // 允许对端把通知镜像到本机
    kPermNotifyAction = 1u << 7, // 允许对端在本机执行通知预设动作（敏感）
    kPermDefault = kPermFileTransfer | kPermClipboard | kPermScreenShare | kPermSync |
                   kPermNotifyMirror,
};

// 权限目录：UI/CLI 按同一份定义渲染开关与说明
struct PermissionInfo {
    std::uint32_t flag = 0;
    const char* key = "";
    const char* name = "";
    const char* description = "";
    bool default_granted = false;
};

const std::vector<PermissionInfo>& permission_catalog();
// 按位查名称；未命中返回 "unknown"
const char* permission_name(std::uint32_t flag);
// "0x0000007f" 形式的掩码文本
std::string permission_mask_text(std::uint32_t mask);

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
    int notify_port = 45717;    // 对端通告的通知镜像通道端口
    std::int64_t paired_at = 0;
    std::uint32_t permissions = kPermDefault;

    ByteBuf to_bytes() const;
    // legacy=true 时按旧版（无 notify_port）字段顺序解析
    static Expected<TrustRecord> from_bytes(ByteReader& reader, bool legacy = false);
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

    // 逐设备、逐权限位更新；变更即时落盘并追加审计日志
    Result set_permissions(const DeviceId& id, std::uint32_t permissions);
    // 审计日志（每行：ISO 时间 | 设备 ID | 设备名 | 旧掩码 -> 新掩码 | 变更位）
    std::vector<std::string> permission_audit_log(std::size_t max_lines = 200) const;

private:
    std::string dir_;
    std::string path_;
    std::string audit_path_;
    std::vector<TrustRecord> records_;
};

} // namespace lb
