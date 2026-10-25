// Linux 密钥保护：用「机器 ID + 用户 ID」派生密钥做 AES-256-GCM 封装，等价于
// Windows DPAPI 的「文件拷走也无法解密」语义。若系统提供 libsecret 且 Secret Service
// 可用，可由调用方替换为钥匙串实现；当前实现零外部依赖、可在无桌面会话的环境工作。
#include "core/platform/platform.h"

#include "core/crypto/crypto.h"

#include <sys/types.h>
#include <unistd.h>

#include <cstring>
#include <string>

namespace lb::pal {
namespace {

constexpr char kMagic[8] = {'L', 'B', 'S', 'S', '1', 0, 0, 0};
constexpr std::size_t kMagicLen = sizeof(kMagic);

std::string machine_id() {
    auto read_trimmed = [](const char* path) -> std::string {
        auto data = read_file(path);
        if (!data) return {};
        std::string text(reinterpret_cast<const char*>(data->data()), data->size());
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' '))
            text.pop_back();
        return text;
    };
    std::string id = read_trimmed("/etc/machine-id");
    if (id.empty()) id = read_trimmed("/var/lib/dbus/machine-id");
    if (id.empty()) id = hostname();
    return id;
}

ByteBuf derive_key() {
    const std::string material = "LinkBridge-v1-secret-protection|" + machine_id() + "|" +
                                 std::to_string(static_cast<long long>(::getuid()));
    return crypto::sha256(material.data(), material.size());
}

} // namespace

ByteBuf protect_secret(const ByteBuf& plain) {
    if (plain.empty()) return {};
    const ByteBuf key = derive_key();
    if (key.size() != crypto::kAeadKeyLen) return {};
    const ByteBuf nonce = crypto::random_bytes(crypto::kAeadNonceLen);
    if (nonce.size() != crypto::kAeadNonceLen) return {};
    const ByteBuf sealed = crypto::aead_encrypt(key, nonce, plain, {});
    if (sealed.size() < crypto::kAeadTagLen) return {};

    ByteBuf out(kMagicLen + nonce.size() + sealed.size());
    std::memcpy(out.data(), kMagic, kMagicLen);
    std::memcpy(out.data() + kMagicLen, nonce.data(), nonce.size());
    std::memcpy(out.data() + kMagicLen + nonce.size(), sealed.data(), sealed.size());
    return out;
}

Expected<ByteBuf> unprotect_secret(const ByteBuf& sealed) {
    const std::size_t header = kMagicLen + crypto::kAeadNonceLen;
    if (sealed.size() < header + crypto::kAeadTagLen ||
        std::memcmp(sealed.data(), kMagic, kMagicLen) != 0)
        return Expected<ByteBuf>::fail("secret: 密文格式非法");

    const ByteBuf key = derive_key();
    if (key.size() != crypto::kAeadKeyLen) return Expected<ByteBuf>::fail("secret: 派生密钥失败");
    const ByteBuf nonce(sealed.begin() + kMagicLen, sealed.begin() + header);
    const ByteBuf body(sealed.begin() + header, sealed.end());
    auto plain = crypto::aead_decrypt(key, nonce, body, {});
    if (!plain)
        return Expected<ByteBuf>::fail("secret: 解密失败（可能换了机器或用户）");
    return plain;
}

} // namespace lb::pal
