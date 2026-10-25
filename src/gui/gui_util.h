#pragma once
// GUI 通用小工具：UTF-8 转换、体积与时间格式化。
#include <wx/string.h>

#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>

namespace lb::gui {

inline wxString W(const std::string& s) { return wxString::FromUTF8(s.data(), s.size()); }
inline std::string U8(const wxString& s) {
    const auto buf = s.utf8_str();
    return std::string(buf.data(), buf.length());
}

inline std::string format_size(std::uint64_t bytes) {
    char buf[64];
    if (bytes >= 1024ull * 1024 * 1024)
        std::snprintf(buf, sizeof(buf), "%.2f GiB", static_cast<double>(bytes) / (1024.0 * 1024 * 1024));
    else if (bytes >= 1024ull * 1024)
        std::snprintf(buf, sizeof(buf), "%.2f MiB", static_cast<double>(bytes) / (1024.0 * 1024));
    else if (bytes >= 1024)
        std::snprintf(buf, sizeof(buf), "%.1f KiB", static_cast<double>(bytes) / 1024.0);
    else
        std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(bytes));
    return buf;
}

inline std::string format_ts(std::int64_t ts) {
    if (ts <= 0) return "-";
    const std::time_t t = static_cast<std::time_t>(ts);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d", tm.tm_year + 1900, tm.tm_mon + 1,
                  tm.tm_mday, tm.tm_hour, tm.tm_min);
    return buf;
}

inline std::string host_time_now() {
    char buf[32];
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

} // namespace lb::gui
