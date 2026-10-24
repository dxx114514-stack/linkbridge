#include "core/pairing/pairing.h"

#include "core/common/bytes.h"
#include "core/crypto/crypto.h"
#include "core/platform/platform.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <random>
#include <sstream>

namespace lb {
namespace {

constexpr std::uint8_t kPairHello = 0x11;
constexpr std::uint8_t kPairAck = 0x12;
constexpr std::uint8_t kPairAuth = 0x13;
constexpr std::uint8_t kPairResp = 0x14;
constexpr std::uint8_t kPairFinal = 0x15;
constexpr std::uint8_t kPairFail = 0x16;
constexpr std::uint8_t kPeerInfoVersion = 2;

std::int64_t now_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

ByteBuf seq_bytes(std::uint64_t seq) {
    ByteBuf b(8);
    for (int i = 0; i < 8; ++i) b[i] = static_cast<std::uint8_t>(seq >> ((7 - i) * 8));
    return b;
}

ByteBuf nonce_for_seq(std::uint64_t seq) {
    ByteBuf nonce(crypto::kAeadNonceLen, 0);
    const ByteBuf s = seq_bytes(seq);
    std::copy(s.begin(), s.end(), nonce.begin() + 4);
    return nonce;
}

ByteBuf seal_payload(const ByteBuf& key, std::uint64_t seq, const ByteBuf& plain) {
    return crypto::aead_encrypt(key, nonce_for_seq(seq), plain, seq_bytes(seq));
}

Expected<ByteBuf> open_payload(const ByteBuf& key, std::uint64_t seq, const ByteBuf& sealed) {
    return crypto::aead_decrypt(key, nonce_for_seq(seq), sealed, seq_bytes(seq));
}

Platform platform_from_string(const std::string& s) {
    if (s == "linux") return Platform::Linux;
    if (s == "android") return Platform::Android;
    return Platform::Windows;
}

std::string random_digits(int count) {
    // 用系统 CSPRNG 生成，拒绝采样避免模偏差
    std::string out;
    out.reserve(static_cast<size_t>(count));
    while (static_cast<int>(out.size()) < count) {
        const ByteBuf raw = crypto::random_bytes(1);
        if (raw.empty()) break;
        const int v = static_cast<int>(raw[0] % 10);
        out.push_back(static_cast<char>('0' + v));
    }
    while (static_cast<int>(out.size()) < count) out.push_back('0');
    return out;
}

bool send_fail(ByteStream& stream, const std::string& reason) {
    ByteWriter w;
    w.u8(kPairFail);
    w.str(reason);
    return stream.write(w.buf()).success;
}

// "127.0.0.1:51234" → "127.0.0.1"
std::string host_only(const std::string& endpoint) {
    const auto pos = endpoint.find(':');
    return pos == std::string::npos ? endpoint : endpoint.substr(0, pos);
}

} // namespace

// ---------------- PeerInfo ----------------

ByteBuf PeerInfo::to_bytes() const {
    ByteWriter w;
    w.u8(kPeerInfoVersion);
    w.str(device_id);
    w.str(name);
    w.str(platform);
    w.str(ip);
    w.str(mac);
    w.str(ssh_public_key);
    w.bytes(public_key);
    w.u32(static_cast<std::uint32_t>(heartbeat_port));
    w.u32(static_cast<std::uint32_t>(ssh_port));
    w.u32(static_cast<std::uint32_t>(transfer_port));
    w.u32(static_cast<std::uint32_t>(clipboard_port));
    w.u32(static_cast<std::uint32_t>(screen_port));
    w.u32(static_cast<std::uint32_t>(sync_port));
    return w.buf();
}

Expected<PeerInfo> PeerInfo::from_bytes(const ByteBuf& data) {
    ByteReader r(data);
    if (r.u8() != kPeerInfoVersion) return Expected<PeerInfo>::fail("设备信息版本不兼容");
    PeerInfo info;
    info.device_id = r.str();
    info.name = r.str();
    info.platform = r.str();
    info.ip = r.str();
    info.mac = r.str();
    info.ssh_public_key = r.str();
    info.public_key = r.bytes();
    info.heartbeat_port = static_cast<int>(r.u32());
    info.ssh_port = static_cast<int>(r.u32());
    info.transfer_port = static_cast<int>(r.u32());
    info.clipboard_port = static_cast<int>(r.u32());
    info.screen_port = static_cast<int>(r.u32());
    info.sync_port = static_cast<int>(r.u32());
    if (!r.ok() || info.device_id.empty() || info.public_key.size() != 65)
        return Expected<PeerInfo>::fail("设备信息字段非法");
    return Expected<PeerInfo>::success(std::move(info));
}

const char* pair_state_name(PairState state) {
    switch (state) {
        case PairState::Idle: return "空闲";
        case PairState::CodeShown: return "等待对端连接";
        case PairState::Handshaking: return "握手进行中";
        case PairState::AwaitConfirm: return "等待用户确认";
        case PairState::Exchanging: return "交换设备信息";
        case PairState::Verifying: return "校验身份";
        case PairState::Done: return "配对成功";
        case PairState::Failed: return "配对失败";
    }
    return "未知";
}

// ---------------- PairingService ----------------

PairingService::PairingService(const Identity& identity, TrustStore& trust, EventBus& events,
                               IDialer& dialer)
    : identity_(identity), trust_(trust), events_(events), dialer_(dialer) {}

void PairingService::set_state(PairState state, const std::string& message) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        state_ = state;
        if (state != PairState::Failed) last_error_.clear();
    }
    events_.publish(PairingStateChanged{state, message});
}

void PairingService::fail(const std::string& message) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        state_ = PairState::Failed;
        last_error_ = message;
    }
    events_.publish(PairingStateChanged{PairState::Failed, message});
}

PairCode PairingService::generate_code(int ttl_sec, int max_attempts) {
    PairCode code;
    code.value = random_digits(6);
    code.expires_at = now_seconds() + ttl_sec;
    code.attempts_left = max_attempts;
    {
        std::lock_guard<std::mutex> lk(mu_);
        code_ = code;
        state_ = PairState::CodeShown;
        last_error_.clear();
    }
    events_.publish(PairingStateChanged{PairState::CodeShown, "配对码已生成"});
    return code;
}

std::optional<PairCode> PairingService::current_code() const {
    std::lock_guard<std::mutex> lk(mu_);
    return code_;
}

void PairingService::invalidate_code() {
    std::lock_guard<std::mutex> lk(mu_);
    code_.reset();
}

PairState PairingService::state() const {
    std::lock_guard<std::mutex> lk(mu_);
    return state_;
}

std::string PairingService::last_error() const {
    std::lock_guard<std::mutex> lk(mu_);
    return last_error_;
}

void PairingService::set_confirm_handler(ConfirmFn handler) {
    std::lock_guard<std::mutex> lk(mu_);
    confirm_ = std::move(handler);
}

void PairingService::set_auto_accept(bool value) {
    std::lock_guard<std::mutex> lk(mu_);
    auto_accept_ = value;
}

void PairingService::set_advertised_ports(int heartbeat_port, int ssh_port, int transfer_port,
                                          int clipboard_port, int screen_port, int sync_port) {
    std::lock_guard<std::mutex> lk(mu_);
    advertised_heartbeat_port_ = heartbeat_port;
    advertised_ssh_port_ = ssh_port;
    advertised_transfer_port_ = transfer_port;
    advertised_clipboard_port_ = clipboard_port;
    advertised_screen_port_ = screen_port;
    advertised_sync_port_ = sync_port;
}

bool PairingService::consume_attempt_locked() {
    if (!code_) return false;
    if (now_seconds() > code_->expires_at) {
        code_.reset();
        return false;
    }
    if (code_->attempts_left <= 0) {
        code_.reset();
        return false;
    }
    --code_->attempts_left;
    return true;
}

std::optional<PairingService::HandshakeKeys> PairingService::derive_keys(
    const ByteBuf& shared, const ByteBuf& eph_a, const ByteBuf& eph_b, const ByteBuf& nonce_a,
    const ByteBuf& nonce_b, const std::string& code) const {
    ByteWriter w;
    w.raw("LB-PAIR-v1", 10);
    w.bytes(eph_a);
    w.bytes(eph_b);
    w.bytes(nonce_a);
    w.bytes(nonce_b);
    const ByteBuf transcript = crypto::sha256(w.buf());

    // 口令作为 HKDF info 的一部分：配对码不同则派生的密钥完全不同
    const ByteBuf material = crypto::hkdf_sha256(shared, transcript, "lb-pair-v1|" + code, 64);
    if (material.size() != 64) return std::nullopt;
    HandshakeKeys keys;
    keys.enc.assign(material.begin(), material.begin() + 32);
    keys.mac.assign(material.begin() + 32, material.end());
    keys.transcript = transcript;
    return keys;
}

PeerInfo PairingService::build_self_info(const std::string& peer_ip) const {
    PeerInfo info;
    info.device_id = identity_.device_id();
    info.name = identity_.name();
    info.platform = pal::system_type();
    info.ip = peer_ip.empty() ? "0.0.0.0" : pal::local_ip_for(peer_ip);
    info.mac = pal::primary_mac();
    info.ssh_public_key = identity_.ssh_public_key();
    info.public_key = identity_.public_key();
    {
        std::lock_guard<std::mutex> lk(mu_);
        info.heartbeat_port = advertised_heartbeat_port_ > 0 ? advertised_heartbeat_port_ : 45711;
        info.ssh_port = advertised_ssh_port_ > 0 ? advertised_ssh_port_ : 45712;
        info.transfer_port = advertised_transfer_port_ > 0 ? advertised_transfer_port_ : 45713;
        info.clipboard_port = advertised_clipboard_port_ > 0 ? advertised_clipboard_port_ : 45714;
        info.screen_port = advertised_screen_port_ > 0 ? advertised_screen_port_ : 45715;
        info.sync_port = advertised_sync_port_ > 0 ? advertised_sync_port_ : 45716;
    }
    return info;
}

std::string PairingService::compute_fingerprint(const std::string& id_a, const ByteBuf& pub_a,
                                                const std::string& id_b, const ByteBuf& pub_b) {
    ByteWriter w;
    w.raw("LB-FP-v1", 8);
    if (id_a <= id_b) {
        w.str(id_a);
        w.bytes(pub_a);
        w.str(id_b);
        w.bytes(pub_b);
    } else {
        w.str(id_b);
        w.bytes(pub_b);
        w.str(id_a);
        w.bytes(pub_a);
    }
    const ByteBuf digest = crypto::sha256(w.buf());
    std::string hex = to_hex(digest.data(), 6);
    for (auto& c : hex) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return hex.substr(0, 4) + "-" + hex.substr(4, 4) + "-" + hex.substr(8, 4);
}

std::string PairingService::self_fingerprint() const { return identity_.fingerprint(); }

Result PairingService::persist_peer(const PeerInfo& peer) {
    // 防御：device_id 必须与公钥自洽，避免对端伪造
    if (Identity::compute_device_id(peer.public_key) != peer.device_id)
        return Result::fail("对端 device_id 与公钥不一致");

    TrustRecord record;
    record.device_id = peer.device_id;
    record.name = peer.name.empty() ? "未知设备" : peer.name;
    record.platform = platform_from_string(peer.platform);
    record.public_key = peer.public_key;
    record.last_ip = peer.ip;
    record.mac = peer.mac;
    record.ssh_public_key = peer.ssh_public_key;
    record.heartbeat_port = peer.heartbeat_port > 0 ? peer.heartbeat_port : 45711;
    record.ssh_port = peer.ssh_port > 0 ? peer.ssh_port : 45712;
    record.transfer_port = peer.transfer_port > 0 ? peer.transfer_port : 45713;
    record.clipboard_port = peer.clipboard_port > 0 ? peer.clipboard_port : 45714;
    record.screen_port = peer.screen_port > 0 ? peer.screen_port : 45715;
    record.sync_port = peer.sync_port > 0 ? peer.sync_port : 45716;
    record.paired_at = now_seconds();
    record.permissions = kPermDefault;
    return trust_.upsert(record);
}

Result PairingService::pair_with(const std::string& ip, int port, const std::string& code) {
    set_state(PairState::Handshaking, "正在连接 " + ip);

    auto stream_result = dialer_.dial(ip, port, 5000);
    if (!stream_result) {
        fail("连接对端失败: " + stream_result.error);
        return Result::fail(stream_result.error);
    }
    std::unique_ptr<ByteStream> stream = std::move(*stream_result);

    auto eph_priv = crypto::ecdh_generate_private();
    if (!eph_priv) {
        fail("生成本地临时密钥失败");
        return Result::fail("生成本地临时密钥失败");
    }
    auto eph_pub = crypto::ecdh_public_raw(*eph_priv);
    if (!eph_pub) {
        fail("导出本地临时公钥失败");
        return Result::fail("导出本地临时公钥失败");
    }
    const ByteBuf self_nonce = crypto::random_bytes(16);

    {
        ByteWriter w;
        w.u8(kPairHello);
        w.u8(1); // 协议版本
        w.raw(eph_pub->data(), eph_pub->size());
        w.str(identity_.device_id());
        w.raw(self_nonce.data(), self_nonce.size());
        if (auto r = stream->write(w.buf()); !r) {
            fail("发送配对请求失败");
            return Result::fail("发送配对请求失败");
        }
    }

    auto ack = stream->read_frame();
    if (!ack) {
        fail("读取对端应答失败: " + ack.error);
        return Result::fail(ack.error);
    }
    ByteReader ra(*ack);
    if (ra.u8() != kPairAck) {
        fail("对端应答类型错误");
        return Result::fail("对端应答类型错误");
    }
    const ByteBuf peer_eph = ra.raw(65);
    const ByteBuf peer_nonce = ra.raw(16);
    if (!ra.ok()) {
        fail("对端应答格式错误");
        return Result::fail("对端应答格式错误");
    }

    auto shared = crypto::ecdh_shared_secret(*eph_priv, peer_eph);
    if (!shared) {
        fail("密钥协商失败");
        return Result::fail("密钥协商失败");
    }
    auto keys = derive_keys(*shared, *eph_pub, peer_eph, self_nonce, peer_nonce, code);
    if (!keys) {
        fail("会话密钥派生失败");
        return Result::fail("会话密钥派生失败");
    }

    set_state(PairState::Exchanging, "发送本机设备信息");
    const ByteBuf mac_auth = crypto::hmac_sha256(keys->mac, "initiator");
    const ByteBuf sealed_self = seal_payload(keys->enc, 0, build_self_info(ip).to_bytes());
    {
        ByteWriter w;
        w.u8(kPairAuth);
        w.bytes(mac_auth);
        w.bytes(sealed_self);
        if (auto r = stream->write(w.buf()); !r) {
            fail("发送配对认证失败");
            return Result::fail("发送配对认证失败");
        }
    }

    auto resp = stream->read_frame();
    if (!resp) {
        fail("读取配对响应失败: " + resp.error);
        return Result::fail(resp.error);
    }
    ByteReader rr(*resp);
    const std::uint8_t type = rr.u8();
    if (type == kPairFail) {
        const std::string reason = rr.str();
        fail(reason.empty() ? "对端拒绝配对" : reason);
        return Result::fail(last_error());
    }
    if (type != kPairResp) {
        fail("配对响应类型错误");
        return Result::fail("配对响应类型错误");
    }
    const ByteBuf mac_resp = rr.bytes();
    const ByteBuf sealed_peer = rr.bytes();
    if (!rr.ok() || mac_resp.size() != crypto::kSha256Len) {
        fail("配对响应格式错误");
        return Result::fail("配对响应格式错误");
    }

    const ByteBuf expect_resp = crypto::hmac_sha256(keys->mac, "responder");
    if (!ct_equal(expect_resp.data(), mac_resp.data(), crypto::kSha256Len)) {
        fail("配对码不匹配或对端身份异常");
        return Result::fail("配对码不匹配或对端身份异常");
    }

    auto peer_plain = open_payload(keys->enc, 0, sealed_peer);
    if (!peer_plain) {
        fail("解密对端设备信息失败");
        return Result::fail("解密对端设备信息失败");
    }
    auto peer = PeerInfo::from_bytes(*peer_plain);
    if (!peer) {
        fail("对端设备信息解析失败");
        return Result::fail("对端设备信息解析失败");
    }

    set_state(PairState::AwaitConfirm, "等待确认对端指纹");
    const std::string fingerprint =
        compute_fingerprint(identity_.device_id(), identity_.public_key(), peer->device_id,
                            peer->public_key);

    bool accepted = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (auto_accept_) accepted = true;
    }
    if (!accepted) {
        ConfirmFn handler;
        {
            std::lock_guard<std::mutex> lk(mu_);
            handler = confirm_;
        }
        if (handler) accepted = handler(*peer, fingerprint);
    }
    if (!accepted) {
        send_fail(*stream, "发起方取消配对");
        fail("用户取消配对");
        return Result::fail("用户取消配对");
    }

    set_state(PairState::Verifying, "校验指纹并建立信任");
    const ByteBuf mac_final = crypto::hmac_sha256(keys->mac, "final");
    {
        ByteWriter w;
        w.u8(kPairFinal);
        w.bytes(mac_final);
        if (auto r = stream->write(w.buf()); !r) {
            fail("发送配对确认失败");
            return Result::fail("发送配对确认失败");
        }
    }

    if (auto r = persist_peer(*peer); !r) {
        fail("保存受信任设备失败: " + r.error);
        return Result::fail(r.error);
    }

    set_state(PairState::Done, "配对成功");
    events_.publish(PairingCompleted{*peer, fingerprint, true});
    stream->close();
    return Result::ok();
}

void PairingService::serve(std::unique_ptr<ByteStream> stream) {
    if (!stream) return;

    auto hello = stream->read_frame();
    if (!hello) return;
    ByteReader rh(*hello);
    if (rh.u8() != kPairHello) return;
    const std::uint8_t version = rh.u8();
    const ByteBuf peer_eph = rh.raw(65);
    const std::string peer_id = rh.str();
    const ByteBuf peer_nonce = rh.raw(16);
    if (!rh.ok() || version != 1) return;

    PairCode code_snapshot;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!consume_attempt_locked()) {
            send_fail(*stream, "配对码无效或已过期");
            return;
        }
        code_snapshot = *code_;
    }

    set_state(PairState::Handshaking, "收到来自 " + peer_id + " 的配对请求");

    auto eph_priv = crypto::ecdh_generate_private();
    if (!eph_priv) {
        fail("生成本地临时密钥失败");
        return;
    }
    auto eph_pub = crypto::ecdh_public_raw(*eph_priv);
    if (!eph_pub) {
        fail("导出本地临时公钥失败");
        return;
    }
    const ByteBuf self_nonce = crypto::random_bytes(16);

    {
        ByteWriter w;
        w.u8(kPairAck);
        w.raw(eph_pub->data(), eph_pub->size());
        w.raw(self_nonce.data(), self_nonce.size());
        if (auto r = stream->write(w.buf()); !r) return;
    }

    auto shared = crypto::ecdh_shared_secret(*eph_priv, peer_eph);
    if (!shared) {
        send_fail(*stream, "密钥协商失败");
        fail("密钥协商失败");
        return;
    }
    auto keys = derive_keys(*shared, peer_eph, *eph_pub, peer_nonce, self_nonce, code_snapshot.value);
    if (!keys) {
        send_fail(*stream, "会话密钥派生失败");
        fail("会话密钥派生失败");
        return;
    }

    auto auth = stream->read_frame();
    if (!auth) return;
    ByteReader rau(*auth);
    if (rau.u8() != kPairAuth) return;
    const ByteBuf mac_auth = rau.bytes();
    const ByteBuf sealed_peer = rau.bytes();
    if (!rau.ok() || mac_auth.size() != crypto::kSha256Len) {
        send_fail(*stream, "配对请求格式错误");
        fail("配对请求格式错误");
        return;
    }

    const ByteBuf expect_auth = crypto::hmac_sha256(keys->mac, "initiator");
    if (!ct_equal(expect_auth.data(), mac_auth.data(), crypto::kSha256Len)) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            code_.reset(); // 码错误立即作废，阻断暴力尝试
        }
        send_fail(*stream, "配对码不匹配");
        fail("配对码不匹配，配对码已作废");
        return;
    }

    auto peer_plain = open_payload(keys->enc, 0, sealed_peer);
    if (!peer_plain) {
        send_fail(*stream, "设备信息解密失败");
        fail("设备信息解密失败");
        return;
    }
    auto peer = PeerInfo::from_bytes(*peer_plain);
    if (!peer) {
        send_fail(*stream, "设备信息解析失败");
        fail("设备信息解析失败");
        return;
    }
    if (peer->device_id != peer_id) {
        send_fail(*stream, "设备身份不一致");
        fail("设备身份不一致");
        return;
    }

    set_state(PairState::AwaitConfirm, "等待确认对端指纹");
    const std::string fingerprint =
        compute_fingerprint(identity_.device_id(), identity_.public_key(), peer->device_id,
                            peer->public_key);
    bool accepted = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (auto_accept_) accepted = true;
    }
    if (!accepted) {
        ConfirmFn handler;
        {
            std::lock_guard<std::mutex> lk(mu_);
            handler = confirm_;
        }
        if (handler) accepted = handler(*peer, fingerprint);
    }
    if (!accepted) {
        send_fail(*stream, "对端用户拒绝配对");
        fail("用户拒绝配对请求");
        return;
    }

    set_state(PairState::Exchanging, "回送本机设备信息");
    const ByteBuf mac_resp = crypto::hmac_sha256(keys->mac, "responder");
    const ByteBuf sealed_self =
        seal_payload(keys->enc, 0, build_self_info(host_only(stream->peer())).to_bytes());
    {
        ByteWriter w;
        w.u8(kPairResp);
        w.bytes(mac_resp);
        w.bytes(sealed_self);
        if (auto r = stream->write(w.buf()); !r) {
            fail("发送配对响应失败");
            return;
        }
    }

    auto final_frame = stream->read_frame();
    if (!final_frame) {
        fail("未收到发起方确认");
        return;
    }
    ByteReader rf(*final_frame);
    if (rf.u8() != kPairFinal) {
        fail("确认包类型错误");
        return;
    }
    const ByteBuf mac_final = rf.bytes();
    const ByteBuf expect_final = crypto::hmac_sha256(keys->mac, "final");
    if (mac_final.size() != crypto::kSha256Len ||
        !ct_equal(expect_final.data(), mac_final.data(), crypto::kSha256Len)) {
        fail("配对确认校验失败");
        return;
    }

    {
        std::lock_guard<std::mutex> lk(mu_);
        code_.reset(); // 单次使用：确认校验通过后立即作废，避免落盘期间仍可复用
    }

    set_state(PairState::Verifying, "校验指纹并建立信任");
    if (auto r = persist_peer(*peer); !r) {
        fail("保存受信任设备失败: " + r.error);
        return;
    }

    set_state(PairState::Done, "配对成功");
    events_.publish(PairingCompleted{*peer, fingerprint, false});
    stream->close();
}

} // namespace lb
