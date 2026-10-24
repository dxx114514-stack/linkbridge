// Windows 文件系统与标准目录实现。
#include "core/platform/platform.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace lb::pal {
namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(len, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), len);
    return w;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr,
                                        0, nullptr, nullptr);
    std::string s(len, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), len, nullptr,
                        nullptr);
    return s;
}

} // namespace

Result ensure_dir(const std::string& path) {
    if (path.empty()) return Result::fail("empty path");
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(widen(path)), ec);
    if (ec) return Result::fail("create_directories failed: " + ec.message());
    return Result::ok();
}

bool file_exists(const std::string& path) {
    std::error_code ec;
    return std::filesystem::exists(std::filesystem::path(widen(path)), ec) && !ec;
}

Expected<ByteBuf> read_file(const std::string& path) {
    std::ifstream in(std::filesystem::path(widen(path)), std::ios::binary);
    if (!in) return Expected<ByteBuf>::fail("open failed: " + path);
    ByteBuf data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return Expected<ByteBuf>::success(std::move(data));
}

Result write_file_atomic(const std::string& path, const ByteBuf& data) {
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(std::filesystem::path(widen(tmp)),
                          std::ios::binary | std::ios::trunc);
        if (!out) return Result::fail("open temp failed: " + tmp);
        if (!data.empty())
            out.write(reinterpret_cast<const char*>(data.data()),
                      static_cast<std::streamsize>(data.size()));
        out.flush();
        if (!out) return Result::fail("write failed: " + tmp);
    }
    std::error_code ec;
    std::filesystem::rename(std::filesystem::path(widen(tmp)), std::filesystem::path(widen(path)),
                            ec);
    if (ec) {
        // 目标已存在时 Windows 的 rename 行为因实现而异，退化为覆盖
        std::filesystem::remove(std::filesystem::path(widen(path)), ec);
        ec.clear();
        std::filesystem::rename(std::filesystem::path(widen(tmp)),
                                std::filesystem::path(widen(path)), ec);
        if (ec) return Result::fail("rename failed: " + ec.message());
    }
    return Result::ok();
}

Result remove_file(const std::string& path) {
    std::error_code ec;
    std::filesystem::remove(std::filesystem::path(widen(path)), ec);
    if (ec) return Result::fail("remove failed: " + ec.message());
    return Result::ok();
}

Result rename_file(const std::string& from, const std::string& to) {
    std::error_code ec;
    std::filesystem::rename(std::filesystem::path(widen(from)), std::filesystem::path(widen(to)),
                            ec);
    if (ec) return Result::fail("rename failed: " + ec.message());
    return Result::ok();
}

std::string join_path(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    const char last = a.back();
    if (last == '\\' || last == '/') return a + b;
    return a + "\\" + b;
}

std::int64_t file_mtime(const std::string& path) {
    std::error_code ec;
    const auto t = std::filesystem::last_write_time(std::filesystem::path(widen(path)), ec);
    if (ec) return 0;
    return static_cast<std::int64_t>(t.time_since_epoch().count());
}

std::string data_dir() {
    std::string base;
    if (const char* local = std::getenv("LOCALAPPDATA")) {
        base = local;
    } else if (const char* profile = std::getenv("USERPROFILE")) {
        base = join_path(profile, "AppData\\Local");
    } else {
        base = ".";
    }
    const std::string dir = join_path(base, "LinkBridge");
    ensure_dir(dir);
    return dir;
}

std::string default_config_path() { return join_path(data_dir(), "linkbridge.toml"); }

} // namespace lb::pal
