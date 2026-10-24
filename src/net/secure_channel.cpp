#include "net/secure_channel.h"

#include "core/common/bytes.h"

#include <cstring>

namespace lb {
namespace {

constexpr std::uint8_t kStsHello = 0x01;
constexpr std::uint8_t kStsAccept = 0x02;
constexpr std::uint8_t kStsFinish = 0x03;

ByteBuf make_nonce(ByteBuf& seq_bytes) {
    ByteBuf nonce(crypto::kAeadNonceLen, 0);
    std::memcpy(nonce.data() + 4, seq_bytes.data(), 8);
    return nonce;
}

ByteBuf seq_to_bytes(std::uint64_t seq) {
    ByteBuf b(8);
    for (int i = 0; i < 8; ++i) b[i] = static_cast<std::uint8_t>(seq >> ((7 - i) * 8));
    return b;
}

} // namespace

ByteBuf build_transcript(const std::string& label, const std::vector<ByteBuf>& parts) {
    ByteWriter w;
    w.raw(label.data(), label.size());
    for (const auto& p : parts) w.bytes(p);
    return crypto::sha256(w.buf());
}

SessionKeys derive_session_keys(const ByteBuf& shared_secret, const ByteBuf& transcript,
                                const std::string& info) {
    SessionKeys keys;
    const ByteBuf material = crypto::hkdf_sha256(shared_secret, transcript, info, 64);
    if (material.size() != 64) return keys;
    keys.enc.assign(material.begin(), material.begin() + 32);
    keys.mac.assign(material.begin() + 32, material.begin() + 64);
    return keys;
}

Result seal_send(ByteStream& stream, const SessionKeys& keys, std::uint64_t& seq,
                 const ByteBuf& plaintext) {
    if (keys.enc.size() != crypto::kAeadKeyLen) return Result::fail("会话密钥未就绪");
    const ByteBuf seq_bytes = seq_to_bytes(seq);
    const ByteBuf nonce = make_nonce(const_cast<ByteBuf&>(seq_bytes));
    const ByteBuf sealed = crypto::aead_encrypt(keys.enc, nonce, plaintext, seq_bytes);
    if (sealed.empty()) return Result::fail("加密失败");
    if (auto r = stream.write(sealed); !r) return r;
    ++seq;
    return Result::ok();
}

Expected<ByteBuf> seal_recv(ByteStream& stream, const SessionKeys& keys, std::uint64_t& seq,
                           size_t max_len) {
    auto frame = stream.read_frame(max_len);
    if (!frame) return Expected<ByteBuf>::fail(frame.error);
    const ByteBuf seq_bytes = seq_to_bytes(seq);
    const ByteBuf nonce = make_nonce(const_cast<ByteBuf&>(seq_bytes));
    auto plain = crypto::aead_decrypt(keys.enc, nonce, *frame, seq_bytes);
    if (!plain) return Expected<ByteBuf>::fail("解密或认证失败（可能被篡改或序号错乱）");
    ++seq;
    return plain;
}

Expected<StsResult> sts_server(ByteStream& stream, const Identity& self, const TrustStore& trust) {
    auto hello = stream.read_frame();
    if (!hello) return Expected<StsResult>::fail("读取握手首包失败");

    ByteReader r(*hello);
    if (r.u8() != kStsHello) return Expected<StsResult>::fail("协议不匹配");
    const ByteBuf peer_eph = r.raw(65);
    const DeviceId peer_id = r.str();
    const ByteBuf peer_nonce = r.raw(16);
    if (!r.ok()) return Expected<StsResult>::fail("握手首包格式错误");

    // 未配对设备一律不响应（防止被用于探测/扫描）
    const auto record = trust.get(peer_id);
    if (!record) return Expected<StsResult>::fail("对端未配对，拒绝握手");

    auto eph_priv = crypto::ecdh_generate_private();
    if (!eph_priv) return Expected<StsResult>::fail(eph_priv.error);
    auto eph_pub = crypto::ecdh_public_raw(*eph_priv);
    if (!eph_pub) return Expected<StsResult>::fail(eph_pub.error);

    auto shared = crypto::ecdh_shared_secret(*eph_priv, peer_eph);
    if (!shared) return Expected<StsResult>::fail(shared.error);

    const ByteBuf self_nonce = crypto::random_bytes(16);
    const ByteBuf transcript = build_transcript(
        "LB-STS-v1", {peer_eph, *eph_pub, peer_nonce, self_nonce,
                      ByteBuf(peer_id.begin(), peer_id.end()),
                      ByteBuf(self.device_id().begin(), self.device_id().end())});

    const ByteBuf self_sig = self.sign(transcript);
    if (self_sig.empty()) return Expected<StsResult>::fail("本地签名失败");

    ByteWriter w;
    w.u8(kStsAccept);
    w.raw(eph_pub->data(), eph_pub->size());
    w.str(self.device_id());
    w.raw(self_nonce.data(), self_nonce.size());
    w.raw(self_sig.data(), self_sig.size());
    if (auto res = stream.write(w.buf()); !res) return Expected<StsResult>::fail(res.error);

    auto finish = stream.read_frame();
    if (!finish) return Expected<StsResult>::fail("读取握手完成包失败");
    ByteReader rf(*finish);
    if (rf.u8() != kStsFinish) return Expected<StsResult>::fail("握手完成包类型错误");
    const ByteBuf peer_sig = rf.raw(64);
    if (!rf.ok()) return Expected<StsResult>::fail("握手完成包格式错误");

    if (!self.verify_peer(record->public_key, transcript, peer_sig))
        return Expected<StsResult>::fail("对端签名校验失败：身份不匹配");

    StsResult out;
    out.keys = derive_session_keys(*shared, transcript, "lb-sts-v1");
    out.peer_id = peer_id;
    out.peer_public_key = record->public_key;
    if (out.keys.enc.size() != crypto::kAeadKeyLen)
        return Expected<StsResult>::fail("密钥派生失败");
    return Expected<StsResult>::success(std::move(out));
}

Expected<StsResult> sts_client(ByteStream& stream, const Identity& self, const TrustStore& trust,
                               const DeviceId& expected_peer) {
    const auto record = trust.get(expected_peer);
    if (!record) return Expected<StsResult>::fail("目标设备不在信任库中");

    auto eph_priv = crypto::ecdh_generate_private();
    if (!eph_priv) return Expected<StsResult>::fail(eph_priv.error);
    auto eph_pub = crypto::ecdh_public_raw(*eph_priv);
    if (!eph_pub) return Expected<StsResult>::fail(eph_pub.error);

    const ByteBuf self_nonce = crypto::random_bytes(16);
    ByteWriter w;
    w.u8(kStsHello);
    w.raw(eph_pub->data(), eph_pub->size());
    w.str(self.device_id());
    w.raw(self_nonce.data(), self_nonce.size());
    if (auto res = stream.write(w.buf()); !res) return Expected<StsResult>::fail(res.error);

    auto accept = stream.read_frame();
    if (!accept) return Expected<StsResult>::fail("读取服务端握手包失败");
    ByteReader ra(*accept);
    if (ra.u8() != kStsAccept) return Expected<StsResult>::fail("服务端握手包类型错误");
    const ByteBuf peer_eph = ra.raw(65);
    const DeviceId peer_id = ra.str();
    const ByteBuf peer_nonce = ra.raw(16);
    const ByteBuf peer_sig = ra.raw(64);
    if (!ra.ok()) return Expected<StsResult>::fail("服务端握手包格式错误");
    if (peer_id != expected_peer) return Expected<StsResult>::fail("对端设备 ID 与预期不符");

    const ByteBuf transcript = build_transcript(
        "LB-STS-v1", {*eph_pub, peer_eph, self_nonce, peer_nonce,
                      ByteBuf(self.device_id().begin(), self.device_id().end()),
                      ByteBuf(peer_id.begin(), peer_id.end())});

    if (!self.verify_peer(record->public_key, transcript, peer_sig))
        return Expected<StsResult>::fail("服务端签名校验失败：可能存在中间人或密钥变更");

    auto shared = crypto::ecdh_shared_secret(*eph_priv, peer_eph);
    if (!shared) return Expected<StsResult>::fail(shared.error);

    const ByteBuf self_sig = self.sign(transcript);
    if (self_sig.empty()) return Expected<StsResult>::fail("本地签名失败");
    ByteWriter wf;
    wf.u8(kStsFinish);
    wf.raw(self_sig.data(), self_sig.size());
    if (auto res = stream.write(wf.buf()); !res) return Expected<StsResult>::fail(res.error);

    StsResult out;
    out.keys = derive_session_keys(*shared, transcript, "lb-sts-v1");
    out.peer_id = peer_id;
    out.peer_public_key = record->public_key;
    if (out.keys.enc.size() != crypto::kAeadKeyLen)
        return Expected<StsResult>::fail("密钥派生失败");
    return Expected<StsResult>::success(std::move(out));
}

} // namespace lb
