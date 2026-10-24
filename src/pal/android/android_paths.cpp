// Android 文件系统与标准目录实现。应用私有目录由 JNI 注入（不使用环境变量）。
#include "core/platform/platform.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

namespace lb::pal {
namespace {

std::mutex g_dir_mu;
std::string g_files_dir;
std::string g_cache_dir;

const std::string& files_dir_locked() { return g_files_dir; }

} // namespace

void android_set_paths(const std::string& files_dir, const std::string& cache_dir) {
    std::lock_guard<std::mutex> lk(g_dir_mu);
    g_files_dir = files_dir;
    g_cache_dir = cache_dir;
    if (!g_files_dir.empty()) ensure_dir(g_files_dir);
    if (!g_cache_dir.empty()) ensure_dir(g_cache_dir);
}

std::string cache_dir() {
    std::lock_guard<std::mutex> lk(g_dir_mu);
    return g_cache_dir;
}

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
    std::lock_guard<std::mutex> lk(g_dir_mu);
    return files_dir_locked().empty() ? "." : files_dir_locked();
}

std::string default_config_path() { return join_path(data_dir(), "linkbridge.toml"); }

std::string hostname() {
    char buf[256] = {};
    if (::gethostname(buf, sizeof(buf) - 1) != 0) return "android";
    return buf;
}

std::string primary_mac() {
    // Android 10+ 对 /sys/class/net 可能返回 EACCES，失败时返回空串（仅用于设备信息展示）
    std::error_code ec;
    try {
        for (const auto& entry : std::filesystem::directory_iterator("/sys/class/net", ec)) {
            const std::string name = entry.path().filename().string();
            if (name == "lo") continue;
            auto addr = read_file(entry.path().string() + "/address");
            if (!addr) continue;
            std::string mac(reinterpret_cast<const char*>(addr->data()), addr->size());
            while (!mac.empty() && (mac.back() == '\n' || mac.back() == '\r')) mac.pop_back();
            if (!mac.empty() && mac != "00:00:00:00:00:00") return mac;
        }
    } catch (const std::filesystem::filesystem_error&) {
    }
    return {};
}

std::string local_ip_for(const std::string& peer) {
    if (peer.rfind("127.", 0) == 0 || peer == "::1") return "127.0.0.1";
    ifaddrs* list = nullptr;
    if (::getifaddrs(&list) != 0) return {};
    std::string result;
    for (ifaddrs* it = list; it; it = it->ifa_next) {
        if (!it->ifa_addr || it->ifa_addr->sa_family != AF_INET) continue;
        const auto* sin = reinterpret_cast<const sockaddr_in*>(it->ifa_addr);
        const std::uint32_t ip = ntohl(sin->sin_addr.s_addr);
        if (ip == 0x7f000001u) continue;
        char buf[INET_ADDRSTRLEN] = {};
        ::inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf));
        result = buf;
        break;
    }
    ::freeifaddrs(list);
    return result;
}

std::string system_type() { return "android"; }

} // namespace lb::pal
