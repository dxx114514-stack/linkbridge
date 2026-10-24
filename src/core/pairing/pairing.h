#pragma once
// 配对服务：6 位数字码 + ECDH 会话密钥 + 口令绑定 KDF + 双向 MAC 确认。
//
// 关于 PAKE：SPAKE2+ 需要任意椭圆曲线点运算，Windows CNG 未暴露该能力。
// v0 采用 ECDH + 口令绑定 HKDF（与 SPAKE2+ 的差距：主动 MITM 可离线试码），
// 因此把「双端指纹人工比对」+「单次有效码 + 严格限次」作为主防线；
// M9 引入 OpenSSL/libsodium 后可替换为 SPAKE2+，本模块接口不变。
#include "core/common/event_bus.h"
#include "core/common/stream.h"
#include "core/common/types.h"
#include "core/identity/identity.h"
#include "core/trust/trust_store.h"

#include <memory>
#include <mutex>
#include <optional>

namespace lb {

struct PeerInfo {
    std::string device_id;
    std::string name;
    std::string platform; // "windows" | "linux" | "android"
    std::string ip;
    std::string mac;
    std::string ssh_public_key;
    ByteBuf public_key;
    int heartbeat_port = 0; // 对端心跳监听端口（配对时通告，支持自定义端口）
    int ssh_port = 0;       // 对端 SSH 端口（M3 使用）
    int transfer_port = 0;  // 对端文件传输端口
    int clipboard_port = 0; // 对端剪贴板通道端口
    int screen_port = 0;    // 对端屏幕共享通道端口
    int sync_port = 0;      // 对端文件夹同步通道端口

    ByteBuf to_bytes() const;
    static Expected<PeerInfo> from_bytes(const ByteBuf& data);
};

enum class PairState : std::uint8_t {
    Idle,
    CodeShown,
    Handshaking,
    AwaitConfirm,
    Exchanging,
    Verifying,
    Done,
    Failed,
};

const char* pair_state_name(PairState state);

struct PairCode {
    std::string value;
    std::int64_t expires_at = 0;
    int attempts_left = 0;
};

struct PairingStateChanged {
    PairState state = PairState::Idle;
    std::string message;
};

struct PairingCompleted {
    PeerInfo peer;
    std::string fingerprint;
    bool initiator = true;
};

using ConfirmFn = std::function<bool(const PeerInfo& peer, const std::string& fingerprint)>;

class PairingService {
public:
    PairingService(const Identity& identity, TrustStore& trust, EventBus& events, IDialer& dialer);

    // 响应方：生成 6 位码（默认 5 分钟有效，最多 3 次尝试，成功后立即作废）
    PairCode generate_code(int ttl_sec = 300, int max_attempts = 3);
    std::optional<PairCode> current_code() const;
    void invalidate_code();

    PairState state() const;
    std::string last_error() const;

    void set_confirm_handler(ConfirmFn handler); // 本端作为响应方时确认入站配对
    void set_auto_accept(bool value);
    void set_advertised_ports(int heartbeat_port, int ssh_port, int transfer_port,
                              int clipboard_port, int screen_port,
                              int sync_port); // 监听实际启动后调用

    // 发起方：阻塞完成一次配对
    Result pair_with(const std::string& ip, int port, const std::string& code);

    // 响应方：处理一条已建立的入站连接（由监听线程调用）
    void serve(std::unique_ptr<ByteStream> stream);

    // 指纹：双方公钥按 device_id 排序后哈希，展示为 XXX-XXXX-XXXX
    static std::string compute_fingerprint(const std::string& id_a, const ByteBuf& pub_a,
                                           const std::string& id_b, const ByteBuf& pub_b);
    std::string self_fingerprint() const;

private:
    struct HandshakeKeys {
        ByteBuf enc;
        ByteBuf mac;
        ByteBuf transcript;
    };

    void set_state(PairState state, const std::string& message = {});
    void fail(const std::string& message);
    bool consume_attempt_locked();
    std::optional<HandshakeKeys> derive_keys(const ByteBuf& shared, const ByteBuf& eph_a,
                                             const ByteBuf& eph_b, const ByteBuf& nonce_a,
                                             const ByteBuf& nonce_b, const std::string& code) const;
    PeerInfo build_self_info(const std::string& peer_ip) const;
    Result persist_peer(const PeerInfo& peer);

    const Identity& identity_;
    TrustStore& trust_;
    EventBus& events_;
    IDialer& dialer_;

    mutable std::mutex mu_;
    std::optional<PairCode> code_;
    PairState state_ = PairState::Idle;
    std::string last_error_;
    ConfirmFn confirm_;
    bool auto_accept_ = false;
    int advertised_heartbeat_port_ = 0;
    int advertised_ssh_port_ = 0;
    int advertised_transfer_port_ = 0;
    int advertised_clipboard_port_ = 0;
    int advertised_screen_port_ = 0;
    int advertised_sync_port_ = 0;
};

} // namespace lb
