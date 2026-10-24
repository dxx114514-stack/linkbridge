// Windows 设备信息：主机名、MAC、本地出口地址。
#include "core/platform/platform.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <vector>

namespace lb::pal {
namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(len, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), len);
    return w;
}

std::string narrow(const wchar_t* w) {
    const int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) return {};
    std::string s(len - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), len - 1, nullptr, nullptr);
    return s;
}

bool is_virtual_adapter(const char* desc) {
    if (!desc) return false;
    const char* virtuals[] = {"virtual", "vmware", "hyper-v", "loopback", "vpn", "tap",
                              "bluetooth", "wsl", "docker"};
    std::string lower(desc);
    for (auto& c : lower) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    for (const char* v : virtuals)
        if (lower.find(v) != std::string::npos) return true;
    return false;
}

} // namespace

std::string hostname() {
    wchar_t buf[256] = {};
    DWORD len = 256;
    if (!GetComputerNameExW(ComputerNameDnsHostname, buf, &len)) return "unknown";
    return narrow(buf);
}

std::string primary_mac() {
    ULONG size = 0;
    GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                     GAA_FLAG_SKIP_DNS_SERVER,
                         nullptr, nullptr, &size);
    if (size == 0) return {};
    std::vector<unsigned char> buffer(size);
    auto* addrs = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    if (GetAdaptersAddresses(AF_UNSPEC,
                             GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                 GAA_FLAG_SKIP_DNS_SERVER,
                             nullptr, addrs, &size) != NO_ERROR)
        return {};

    for (auto* a = addrs; a; a = a->Next) {
        if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        if (a->OperStatus != IfOperStatusUp) continue;
        if (a->PhysicalAddressLength < 6) continue;
        if (is_virtual_adapter(narrow(a->Description).c_str())) continue;
        char mac[32] = {};
        std::snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X", a->PhysicalAddress[0],
                      a->PhysicalAddress[1], a->PhysicalAddress[2], a->PhysicalAddress[3],
                      a->PhysicalAddress[4], a->PhysicalAddress[5]);
        return mac;
    }
    return {};
}

std::string local_ip_for(const std::string& peer) {
    SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return "127.0.0.1";

    sockaddr_in target{};
    target.sin_family = AF_INET;
    target.sin_port = htons(9);
    if (::inet_pton(AF_INET, peer.c_str(), &target.sin_addr) != 1) {
        // 解析失败时退化为回环地址
        ::closesocket(s);
        return "127.0.0.1";
    }
    std::string result = "127.0.0.1";
    if (::connect(s, reinterpret_cast<sockaddr*>(&target), sizeof(target)) == 0) {
        sockaddr_in local{};
        int len = sizeof(local);
        if (::getsockname(s, reinterpret_cast<sockaddr*>(&local), &len) == 0) {
            char buf[INET_ADDRSTRLEN] = {};
            ::inet_ntop(AF_INET, &local.sin_addr, buf, sizeof(buf));
            result = buf;
        }
    }
    ::closesocket(s);
    return result;
}

std::string system_type() { return "windows"; }

} // namespace lb::pal
