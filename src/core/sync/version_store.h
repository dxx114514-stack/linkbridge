#pragma once
// M7 版本库/回收站：覆盖或删除前把旧文件归档到
// <sync_root>/.linkbridge/versions/<版本ID>/<原相对路径>。
// 版本 ID 形如 yyyyMMdd-HHmmss-<随机后缀>，按 UTC 解释。
#include "core/common/types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace lb {

struct VersionInfo {
    std::string id;               // 版本目录名
    std::string path;             // 原相对路径（'/' 分隔）
    std::uint64_t size = 0;
    std::int64_t archived_at = 0; // UTC 秒
};

class VersionStore {
public:
    VersionStore(std::string sync_root, int keep_count, int keep_days, int max_mb);

    // 归档并移除原文件；原文件不存在时返回成功
    Result archive(const std::string& rel, std::int64_t now);
    // 语义化别名：删除前归档
    Result remove(const std::string& rel, std::int64_t now);

    // 按时间倒序列出（rel 为空表示全部）
    std::vector<VersionInfo> list(const std::string& rel = "") const;

    // 恢复指定版本（id 为空时取最新）到原路径；当前文件先归档
    Result restore(const std::string& rel, const std::string& version_id, std::int64_t now);

    // 按数量/天数/容量三重上限清理，先到先清（最旧优先）
    void prune(std::int64_t now);

    const std::string& root() const { return sync_root_; }
    static std::int64_t parse_id_time(const std::string& id);

private:
    std::string versions_root() const { return sync_root_ + "/.linkbridge/versions"; }

    std::string sync_root_;
    int keep_count_ = 5;
    int keep_days_ = 30;
    int max_mb_ = 2048;
};

} // namespace lb
