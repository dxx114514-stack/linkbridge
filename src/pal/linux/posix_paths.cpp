// Linux/POSIX 文件系统与标准目录实现（Windows 侧的对应实现在 pal/windows）。
#include "core/platform/platform.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace lb::pal {
namespace {

std::string home_dir() {
    if (const char* h = std::getenv("HOME"); h && *h) return h;
    if (const passwd* pw = ::getpwuid(::getuid()); pw && pw->pw_dir) return pw->pw_dir;
    return ".";
}

std::string join3(const std::string& a, const std::string& b, const std::string& c) {
    return join_path(join_path(a, b), c);
}

} // namespace

Result ensure_dir(const std::string& path) {
    if (path.empty()) return Result::fail("empty path");
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path), ec);
    if (ec && !std::filesystem::is_directory(std::filesystem::path(path), ec))
        return Result::fail("create_directories failed: " + ec.message());
    return Result::ok();
}

bool file_exists(const std::string& path) {
    std::error_code ec;
    return std::filesystem::exists(std::filesystem::path(path), ec) && !ec;
}

Expected<ByteBuf> read_file(const std::string& path) {
    std::ifstream in(std::filesystem::path(path), std::ios::binary);
    if (!in) return Expected<ByteBuf>::fail("open failed: " + path);
    ByteBuf data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return Expected<ByteBuf>::success(std::move(data));
}

Result write_file_atomic(const std::string& path, const ByteBuf& data) {
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(std::filesystem::path(tmp), std::ios::binary | std::ios::trunc);
        if (!out) return Result::fail("open temp failed: " + tmp);
        if (!data.empty())
            out.write(reinterpret_cast<const char*>(data.data()),
                      static_cast<std::streamsize>(data.size()));
        out.flush();
        if (!out) return Result::fail("write failed: " + tmp);
    }
    std::error_code ec;
    std::filesystem::rename(std::filesystem::path(tmp), std::filesystem::path(path), ec);
    if (ec) {
        std::filesystem::remove(std::filesystem::path(path), ec);
        ec.clear();
        std::filesystem::rename(std::filesystem::path(tmp), std::filesystem::path(path), ec);
        if (ec) return Result::fail("rename failed: " + ec.message());
    }
    return Result::ok();
}

Result remove_file(const std::string& path) {
    std::error_code ec;
    std::filesystem::remove(std::filesystem::path(path), ec);
    if (ec) return Result::fail("remove failed: " + ec.message());
    return Result::ok();
}

Result rename_file(const std::string& from, const std::string& to) {
    std::error_code ec;
    std::filesystem::rename(std::filesystem::path(from), std::filesystem::path(to), ec);
    if (ec) return Result::fail("rename failed: " + ec.message());
    return Result::ok();
}

std::string join_path(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    if (a.back() == '/') return a + b;
    return a + "/" + b;
}

std::int64_t file_mtime(const std::string& path) {
    std::error_code ec;
    const auto t = std::filesystem::last_write_time(std::filesystem::path(path), ec);
    if (ec) return 0;
    return static_cast<std::int64_t>(t.time_since_epoch().count());
}

std::string data_dir() {
    std::string base;
    if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg) {
        base = xdg;
    } else {
        base = join3(home_dir(), ".local", "share");
    }
    const std::string dir = join_path(base, "LinkBridge");
    ensure_dir(dir);
    return dir;
}

std::string default_config_path() { return join_path(data_dir(), "linkbridge.toml"); }

std::string hostname() {
    char buf[256] = {};
    if (::gethostname(buf, sizeof(buf) - 1) != 0) return "linux";
    return buf;
}

std::string primary_mac() {
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator("/sys/class/net", ec)) {
        const std::string name = entry.path().filename().string();
        if (name == "lo") continue;
        auto addr = read_file(entry.path().string() + "/address");
        if (!addr) continue;
        std::string mac(reinterpret_cast<const char*>(addr->data()), addr->size());
        while (!mac.empty() && (mac.back() == '\n' || mac.back() == '\r')) mac.pop_back();
        if (!mac.empty() && mac != "00:00:00:00:00:00") return mac;
    }
    return {};
}

std::string local_ip_for(const std::string& peer) {
    if (peer.rfind("127.", 0) == 0 || peer == "::1") return "127.0.0.1";

    // 首选 UDP connect 路由探测：不真正发包，但能得到内核选择的本机地址
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd >= 0) {
        sockaddr_in target{};
        target.sin_family = AF_INET;
        target.sin_port = htons(45710);
        if (::inet_pton(AF_INET, peer.c_str(), &target.sin_addr) == 1 &&
            ::connect(fd, reinterpret_cast<sockaddr*>(&target), sizeof(target)) == 0) {
            sockaddr_in local{};
            socklen_t len = sizeof(local);
            if (::getsockname(fd, reinterpret_cast<sockaddr*>(&local), &len) == 0) {
                char buf[INET_ADDRSTRLEN] = {};
                ::inet_ntop(AF_INET, &local.sin_addr, buf, sizeof(buf));
                ::close(fd);
                if (buf[0] && std::string(buf) != "0.0.0.0") return buf;
            }
        }
        ::close(fd);
    }

    ifaddrs* list = nullptr;
    if (::getifaddrs(&list) != 0) return {};
    std::string result;
    for (ifaddrs* it = list; it; it = it->ifa_next) {
        if (!it->ifa_addr || it->ifa_addr->sa_family != AF_INET) continue;
        if (!(it->ifa_flags & IFF_UP) || (it->ifa_flags & IFF_LOOPBACK)) continue;
        const auto* sin = reinterpret_cast<const sockaddr_in*>(it->ifa_addr);
        char buf[INET_ADDRSTRLEN] = {};
        ::inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf));
        result = buf;
        break;
    }
    ::freeifaddrs(list);
    return result;
}

std::string system_type() { return "linux"; }

} // namespace lb::pal
