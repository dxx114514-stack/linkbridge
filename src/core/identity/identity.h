#pragma once
// 设备身份：长期 ECDSA P-256 密钥对 + device_id + 指纹。
// 私钥以 CNG blob 形式经系统密钥保护（DPAPI）后落盘，绝不写明文。
#include "core/common/types.h"

#include <string>

namespace lb {

class Identity {
public:
    // 从目录加载；不存在则生成并保存
    static Expected<Identity> load_or_create(const std::string& dir, const std::string& device_name);

    const DeviceId& device_id() const { return device_id_; }
    const std::string& name() const { return name_; }
    const ByteBuf& public_key() const { return public_key_; }
    const std::string& ssh_public_key() const { return ssh_public_key_; }
    const std::string& fingerprint() const { return fingerprint_; }
    Platform platform() const {
#if defined(__ANDROID__)
        return Platform::Android;
#elif defined(_WIN32)
        return Platform::Windows;
#else
        return Platform::Linux;
#endif
    }

    ByteBuf sign(const ByteBuf& message) const;
    bool verify_peer(const ByteBuf& peer_pub, const ByteBuf& message, const ByteBuf& sig) const;

    void set_name(const std::string& name);

    // device_id = hex(SHA-256(pub))[0:16]
    static DeviceId compute_device_id(const ByteBuf& pub_raw);
    // 展示用指纹：3 组 4 位十六进制
    static std::string format_fingerprint(const ByteBuf& pub_raw);

private:
    ByteBuf private_blob_;
    ByteBuf public_key_;
    DeviceId device_id_;
    std::string name_;
    std::string ssh_public_key_;
    std::string fingerprint_;
    std::string dir_;
};

} // namespace lb
