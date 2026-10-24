#pragma once
// 会话层：STS 双向认证握手 + AES-256-GCM 加密帧。
// 设计文档中的 TLS 1.3 在 v0 由本模块等价替代（无第三方依赖）：
//   - 临时 ECDH 提供前向保密
//   - 长期身份密钥签名提供双向身份认证（类 Station-to-Station 协议）
// M9 可替换为 BoringSSL/OpenSSL 的标准 TLS 1.3 实现，接口保持不变。
#include "core/common/stream.h"
#include "core/crypto/crypto.h"
#include "core/identity/identity.h"
#include "core/trust/trust_store.h"

namespace lb {

struct SessionKeys {
    ByteBuf enc; // 32 字节
    ByteBuf mac; // 32 字节
};

// 加密帧：nonce = 4 字节零串 || 8 字节大端计数器；序号同时作为 AAD
Result seal_send(ByteStream& stream, const SessionKeys& keys, std::uint64_t& seq,
                 const ByteBuf& plaintext);
Expected<ByteBuf> seal_recv(ByteStream& stream, const SessionKeys& keys, std::uint64_t& seq,
                           size_t max_len = 65536);

struct StsResult {
    SessionKeys keys;
    DeviceId peer_id;
    ByteBuf peer_public_key;
};

// 服务端（被控端/响应方）：校验对端必须在信任库中，否则立即中断
Expected<StsResult> sts_server(ByteStream& stream, const Identity& self, const TrustStore& trust);

// 客户端（发起方）：必须指定预期对端
Expected<StsResult> sts_client(ByteStream& stream, const Identity& self, const TrustStore& trust,
                               const DeviceId& expected_peer);

// 供配对模块复用：由共享密钥与转录哈希派生会话密钥
SessionKeys derive_session_keys(const ByteBuf& shared_secret, const ByteBuf& transcript,
                                const std::string& info);

ByteBuf build_transcript(const std::string& label, const std::vector<ByteBuf>& parts);

} // namespace lb
