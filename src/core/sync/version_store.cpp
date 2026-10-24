#include "core/sync/version_store.h"

#include "core/common/bytes.h"
#include "core/crypto/crypto.h"
#include "core/platform/platform.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <map>

namespace lb {
namespace {

std::filesystem::path to_path(const std::string& s) {
    return std::filesystem::path(std::u8string(s.begin(), s.end()));
}

std::string to_utf8(const std::filesystem::path& p) {
    const auto u8 = p.u8string();
    return std::string(u8.begin(), u8.end());
}

std::string basename_of(const std::string& rel) {
    const auto pos = rel.find_last_of('/');
    return pos == std::string::npos ? rel : rel.substr(pos + 1);
}

std::string dirname_of(const std::string& rel) {
    const auto pos = rel.find_last_of('/');
    return pos == std::string::npos ? "" : rel.substr(0, pos);
}

std::string format_id_time(std::int64_t ts) {
    const std::time_t t = static_cast<std::time_t>(ts);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d%02d%02d-%02d%02d%02d", tm.tm_year + 1900, tm.tm_mon + 1,
                  tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

} // namespace

VersionStore::VersionStore(std::string sync_root, int keep_count, int keep_days, int max_mb)
    : sync_root_(std::move(sync_root)), keep_count_(keep_count), keep_days_(keep_days),
      max_mb_(max_mb) {}

std::int64_t VersionStore::parse_id_time(const std::string& id) {
    if (id.size() < 15 || id[8] != '-') return 0;
    std::tm tm{};
    const std::string date = id.substr(0, 8) + id.substr(9, 6);
    if (std::sscanf(date.c_str(), "%4d%2d%2d%2d%2d%2d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
                    &tm.tm_hour, &tm.tm_min, &tm.tm_sec) != 6)
        return 0;
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
#if defined(_WIN32)
    return static_cast<std::int64_t>(_mkgmtime(&tm));
#else
    return static_cast<std::int64_t>(timegm(&tm));
#endif
}

Result VersionStore::archive(const std::string& rel, std::int64_t now) {
    const std::string src = pal::join_path(sync_root_, rel);
    if (!pal::file_exists(src)) return Result::ok();

    const std::string id = format_id_time(now) + "-" + to_hex(crypto::random_bytes(4));
    std::string dest_dir = pal::join_path(versions_root(), id);
    const std::string parent_rel = dirname_of(rel);
    if (!parent_rel.empty()) dest_dir = pal::join_path(dest_dir, parent_rel);
    if (auto r = pal::ensure_dir(dest_dir); !r) return Result::fail("创建版本目录失败: " + r.error);
    const std::string dest = pal::join_path(dest_dir, basename_of(rel));

    std::error_code ec;
    std::filesystem::rename(to_path(src), to_path(dest), ec);
    if (ec) {
        ec.clear();
        std::filesystem::copy_file(to_path(src), to_path(dest),
                                   std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) return Result::fail("归档文件失败: " + rel);
        if (auto r = pal::remove_file(src); !r) return Result::fail("移除原文件失败: " + r.error);
    }
    return Result::ok();
}

Result VersionStore::remove(const std::string& rel, std::int64_t now) {
    return archive(rel, now);
}

std::vector<VersionInfo> VersionStore::list(const std::string& rel) const {
    std::vector<VersionInfo> out;
    std::error_code ec;
    const auto root = to_path(versions_root());
    if (!std::filesystem::is_directory(root, ec) || ec) return out;

    for (auto it = std::filesystem::directory_iterator(root, ec);
         it != std::filesystem::directory_iterator() && !ec; it.increment(ec)) {
        if (!it->is_directory(ec)) continue;
        const std::string id = to_utf8(it->path().filename());
        const std::int64_t ts = parse_id_time(id);
        std::error_code inner_ec;
        for (auto fit = std::filesystem::recursive_directory_iterator(it->path(), inner_ec);
             fit != std::filesystem::recursive_directory_iterator() && !inner_ec;
             fit.increment(inner_ec)) {
            if (!fit->is_regular_file(inner_ec)) continue;
            std::string path = to_utf8(fit->path().lexically_relative(it->path()));
            std::replace(path.begin(), path.end(), '\\', '/');
            if (!rel.empty() && path != rel) continue;
            VersionInfo v;
            v.id = id;
            v.path = path;
            v.size = static_cast<std::uint64_t>(fit->file_size(inner_ec));
            v.archived_at = ts;
            if (!inner_ec) out.push_back(std::move(v));
        }
    }
    std::sort(out.begin(), out.end(), [](const VersionInfo& a, const VersionInfo& b) {
        if (a.archived_at != b.archived_at) return a.archived_at > b.archived_at;
        return a.id > b.id;
    });
    return out;
}

Result VersionStore::restore(const std::string& rel, const std::string& version_id,
                             std::int64_t now) {
    std::vector<VersionInfo> versions = list(rel);
    if (!version_id.empty())
        versions.erase(std::remove_if(versions.begin(), versions.end(),
                                      [&](const VersionInfo& v) { return v.id != version_id; }),
                       versions.end());
    if (versions.empty()) return Result::fail("没有可恢复的历史版本: " + rel);

    const VersionInfo v = versions.front();
    const std::string src = pal::join_path(pal::join_path(versions_root(), v.id), rel);
    if (!pal::file_exists(src)) return Result::fail("历史版本文件不存在");

    if (auto r = archive(rel, now); !r) return Result::fail(r.error);

    const std::string dest = pal::join_path(sync_root_, rel);
    const std::string parent = dirname_of(dest);
    if (!parent.empty()) {
        if (auto r = pal::ensure_dir(parent); !r) return Result::fail(r.error);
    }
    std::error_code ec;
    std::filesystem::copy_file(to_path(src), to_path(dest),
                               std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) return Result::fail("恢复历史版本失败: " + rel);
    pal::remove_file(src);
    std::filesystem::remove_all(to_path(pal::join_path(versions_root(), v.id)), ec);
    return Result::ok();
}

void VersionStore::prune(std::int64_t now) {
    std::vector<VersionInfo> all = list();
    // 按（路径, 时间）倒序，按文件维度执行数量与天数限制
    std::sort(all.begin(), all.end(), [](const VersionInfo& a, const VersionInfo& b) {
        if (a.path != b.path) return a.path < b.path;
        if (a.archived_at != b.archived_at) return a.archived_at > b.archived_at;
        return a.id > b.id;
    });

    std::vector<VersionInfo> keep;
    std::vector<VersionInfo> drop;
    std::map<std::string, int> per_path;
    for (const auto& v : all) {
        bool discard = false;
        const int seen = per_path[v.path]++;
        if (keep_count_ > 0 && seen >= keep_count_) discard = true;
        if (keep_days_ > 0 && now - v.archived_at > static_cast<std::int64_t>(keep_days_) * 86400)
            discard = true;
        if (discard) drop.push_back(v);
        else keep.push_back(v);
    }

    // 容量上限：剩余版本按时间从旧到新删除，直到总量不超限
    std::uint64_t total = 0;
    for (const auto& v : keep) total += v.size;
    const std::uint64_t cap =
        static_cast<std::uint64_t>(std::max(0, max_mb_)) * 1024ull * 1024ull;
    if (cap > 0 && total > cap) {
        std::sort(keep.begin(), keep.end(), [](const VersionInfo& a, const VersionInfo& b) {
            if (a.archived_at != b.archived_at) return a.archived_at < b.archived_at;
            return a.id < b.id;
        });
        for (const auto& v : keep) {
            if (total <= cap) break;
            total -= v.size;
            drop.push_back(v);
        }
    }

    for (const auto& v : drop) {
        std::error_code ec;
        std::filesystem::remove_all(to_path(pal::join_path(versions_root(), v.id)), ec);
    }
}

} // namespace lb
