// Windows DPAPI 密钥保护：解密与当前用户/机器绑定，文件被拷走也无法还原。
#include "core/platform/platform.h"

#include <windows.h>
#include <dpapi.h>

#include <cstring>

namespace lb::pal {
namespace {

const char kEntropy[] = "LinkBridge-v1-secret-protection";

} // namespace

ByteBuf protect_secret(const ByteBuf& plain) {
    if (plain.empty()) return {};

    DATA_BLOB in;
    in.pbData = const_cast<BYTE*>(plain.data());
    in.cbData = static_cast<DWORD>(plain.size());

    DATA_BLOB entropy;
    entropy.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(kEntropy));
    entropy.cbData = static_cast<DWORD>(std::strlen(kEntropy));

    DATA_BLOB out{};
    if (!CryptProtectData(&in, L"LinkBridge", &entropy, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &out))
        return {};
    ByteBuf result(out.pbData, out.pbData + out.cbData);
    LocalFree(out.pbData);
    return result;
}

Expected<ByteBuf> unprotect_secret(const ByteBuf& sealed) {
    if (sealed.empty()) return Expected<ByteBuf>::fail("dpapi: empty input");

    DATA_BLOB in;
    in.pbData = const_cast<BYTE*>(sealed.data());
    in.cbData = static_cast<DWORD>(sealed.size());

    DATA_BLOB entropy;
    entropy.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(kEntropy));
    entropy.cbData = static_cast<DWORD>(std::strlen(kEntropy));

    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN,
                            &out))
        return Expected<ByteBuf>::fail("dpapi: unprotect failed");
    ByteBuf result(out.pbData, out.pbData + out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return Expected<ByteBuf>::success(std::move(result));
}

} // namespace lb::pal
