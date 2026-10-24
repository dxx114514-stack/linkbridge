#include "core/identity/identity.h"

#include "core/common/bytes.h"
#include "core/crypto/crypto.h"
#include "core/platform/platform.h"

#include <cctype>

namespace lb {
namespace {

std::string to_lower_hex(const std::string& s) {
    std::string out = s;
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

} // namespace

DeviceId Identity::compute_device_id(const ByteBuf& pub_raw) {
    const ByteBuf digest = crypto::sha256(pub_raw);
    if (digest.size() != crypto::kSha256Len) return {};
    return to_hex(digest.data(), 8); // 16 个十六进制字符
}

std::string Identity::format_fingerprint(const ByteBuf& pub_raw) {
    const ByteBuf digest = crypto::sha256(pub_raw);
    if (digest.size() != crypto::kSha256Len) return {};
    std::string hex = to_hex(digest.data(), 6); // 12 字符 → 3 组
    for (auto& c : hex) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return hex.substr(0, 4) + "-" + hex.substr(4, 4) + "-" + hex.substr(8, 4);
}

Expected<Identity> Identity::load_or_create(const std::string& dir, const std::string& device_name) {
    Identity id;
    id.dir_ = dir;
    if (auto r = pal::ensure_dir(dir); !r) return Expected<Identity>::fail(r.error);

    const std::string key_path = pal::join_path(dir, "identity.key");
    const std::string name_path = pal::join_path(dir, "identity.name");

    if (pal::file_exists(key_path)) {
        auto sealed = pal::read_file(key_path);
        if (!sealed) return Expected<Identity>::fail("读取身份密钥失败: " + sealed.error);
        auto plain = pal::unprotect_secret(*sealed);
        if (!plain) return Expected<Identity>::fail("解密身份密钥失败（可能换了用户或机器）");
        id.private_blob_ = *plain;
    } else {
        auto priv = crypto::ecdsa_generate_private();
        if (!priv) return Expected<Identity>::fail(priv.error);
        id.private_blob_ = *priv;
        const ByteBuf sealed = pal::protect_secret(id.private_blob_);
        if (sealed.empty()) return Expected<Identity>::fail("保护身份密钥失败");
        if (auto r = pal::write_file_atomic(key_path, sealed); !r)
            return Expected<Identity>::fail("保存身份密钥失败: " + r.error);
    }

    auto pub = crypto::ecdsa_public_raw(id.private_blob_);
    if (!pub) return Expected<Identity>::fail("导出身份公钥失败");
    id.public_key_ = *pub;
    id.device_id_ = compute_device_id(id.public_key_);
    id.fingerprint_ = format_fingerprint(id.public_key_);
    if (id.device_id_.empty()) return Expected<Identity>::fail("计算 device_id 失败");

    // v0：SSH 公钥字段以 OpenSSH 文本形式登记同一把密钥，供 M3 绑定到 SSH 主机密钥
    id.ssh_public_key_ = "ecdsa-sha2-nistp256 " + to_hex(id.public_key_);

    if (pal::file_exists(name_path)) {
        auto stored = pal::read_file(name_path);
        if (stored && !stored->empty()) {
            std::string n(stored->begin(), stored->end());
            while (!n.empty() && (n.back() == '\n' || n.back() == '\r')) n.pop_back();
            if (!n.empty()) id.name_ = n;
        }
    }
    if (id.name_.empty()) id.name_ = device_name.empty() ? pal::hostname() : device_name;

    id.set_name(id.name_); // 持久化名称
    return Expected<Identity>::success(std::move(id));
}

void Identity::set_name(const std::string& name) {
    name_ = name;
    const std::string data = name_ + "\n";
    ByteBuf buf(data.begin(), data.end());
    pal::write_file_atomic(pal::join_path(dir_, "identity.name"), buf);
}

ByteBuf Identity::sign(const ByteBuf& message) const {
    return crypto::ecdsa_sign(private_blob_, message);
}

bool Identity::verify_peer(const ByteBuf& peer_pub, const ByteBuf& message,
                           const ByteBuf& sig) const {
    return crypto::ecdsa_verify(peer_pub, message, sig);
}

} // namespace lb
