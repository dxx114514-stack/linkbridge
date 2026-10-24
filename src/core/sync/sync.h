#pragma once
// M6/M7 文件夹同步（单文件夹）：扫描 → 索引比对 → 交换清单 → 双向增量传输；
// M7 增加删除传播（墓碑）、版本库/回收站、newer/larger 冲突策略与冲突处理。
// 文件数据经 STS 加密通道传输，协议对标设计文档 §9。
#include "core/common/event_bus.h"
#include "core/common/stream.h"
#include "core/common/types.h"
#include "core/config/config.h"
#include "core/identity/identity.h"
#include "core/sync/version_store.h"
#include "core/trust/trust_store.h"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace lb {

class IFileWatcher;
struct SessionKeys;

// 本端快照条目（相对路径统一使用 '/'）
struct SyncEntry {
    std::string path;
    std::uint64_t size = 0;
    std::int64_t mtime = 0;
    ByteBuf hash; // SHA-256（32B）
    bool deleted = false;      // 墓碑：本端已删除、待传播
    std::int64_t deleted_at = 0;
};

// 索引条目：上一次成功同步后的基线
struct SyncIndexEntry {
    std::uint64_t size = 0;
    std::int64_t mtime = 0;
    ByteBuf hash;
    bool conflicted = false; // keep_both 冲突未解决：后续会话跳过，避免重复冲突副本
    bool deleted = false;    // 墓碑：已删除，防止对端旧版本复活
    std::int64_t deleted_at = 0;
};

// 索引文件：<data_dir>/sync_index.bin，格式 LBS2（LBS1 增加墓碑字段）
struct SyncIndex {
    std::map<std::string, SyncIndexEntry> entries;

    static Expected<SyncIndex> load(const std::string& path);
    Result save(const std::string& path) const;

    const SyncIndexEntry* find(const std::string& path) const;
    void set(const std::string& path, SyncIndexEntry entry);
};

// 计划项：path 为原相对路径，target_path 为实际落盘相对路径（冲突时已重命名）
struct SyncPlanItem {
    std::string path;
    std::string target_path;
    std::uint64_t size = 0;
    std::int64_t mtime = 0;
    ByteBuf hash;
};

struct SyncPlan {
    std::vector<SyncPlanItem> want;    // 本端需要向对端拉取的文件
    std::vector<SyncPlanItem> send;    // 本端需要推送给对端的文件
    std::vector<std::string> conflicts; // 本次判定为 keep_both 冲突的原路径
    std::vector<std::string> want_delete; // 本端希望发起方删除的路径
    std::vector<std::string> send_delete; // 本端将自行删除的路径
};

struct SyncStats {
    std::uint64_t pushed = 0;    // 本端发出的文件数
    std::uint64_t pulled = 0;    // 本端接收的文件数
    std::uint64_t conflicts = 0;
    std::uint64_t deleted = 0;   // 因删除传播移除的文件数
    std::uint64_t bytes = 0;     // 双向累计字节
    std::uint64_t skipped = 0;   // 因超限/忽略/冲突未处理
};

// 冲突列表项：path 为原路径，copies 为已生成的冲突副本
struct SyncConflictInfo {
    std::string path;
    std::vector<std::string> copies;
};

struct SyncStarted {
    std::string peer;
    std::string peer_name;
};

struct SyncCompleted {
    std::string peer;
    std::string peer_name;
    SyncStats stats;
};

struct SyncFailed {
    std::string peer;
    std::string peer_name;
    std::string reason;
};

enum class SyncMode : std::uint8_t { SendReceive, SendOnly, ReceiveOnly, Mirror };

const char* sync_mode_name(SyncMode mode);

class SyncService {
public:
    SyncService(const Identity& identity, TrustStore& trust, EventBus& events, IDialer& dialer,
                const SyncConfig& config, std::string data_dir);
    ~SyncService();

    // 发起方：与一台受信任设备执行一次完整同步
    Result sync_with(const DeviceId& peer, int timeout_ms, SyncStats* out = nullptr);

    // 响应方：处理一条已建立的同步连接（由监听线程调用）
    void serve(std::unique_ptr<ByteStream> stream);

    // 自动同步：文件监听 + 定时全量扫描（需 config.enabled 且 folder 非空）
    void start_auto();
    void stop_auto();
    void notify_fs_change();

    const SyncConfig& config() const { return config_; }
    const std::string& root() const { return config_.folder; }
    bool auto_running() const { return auto_thread_.joinable(); }
    std::size_t index_size() const;

    // ---- M7：冲突与版本库 ----
    // 列出未解决的冲突（原路径 + 已生成的副本）
    std::vector<SyncConflictInfo> conflicts() const;
    // action: keep_local（删副本保留原文件）| keep_remote（用副本覆盖原文件）|
    //         keep_both（两个文件都保留并解除标记）
    Result resolve_conflict(const std::string& path, const std::string& action);
    // 版本库列表（path 为空表示全部）
    std::vector<VersionInfo> history(const std::string& path = "") const;
    // 恢复历史版本（version_id 为空时取最新）；当前文件先归档
    Result restore_version(const std::string& path, const std::string& version_id = "");

    // ---- 纯函数（可单测）----
    static bool ignore_match(const std::string& rel, const std::vector<std::string>& patterns);
    static std::vector<std::string> split_ignore(const std::string& csv);
    // 归一化并校验相对路径；失败时返回错误（拒绝 ..、绝对路径、非法字符）
    static Result sanitize_rel_path(std::string& rel);
    // name (conflict <device> <yyyyMMdd-HHmmss>).ext
    static std::string conflict_name(const std::string& rel, const std::string& device_name,
                                     std::int64_t timestamp);
    // 依据本端快照/对端清单/索引基线计算差量（响应方视角）
    static SyncPlan compute_plan(const std::vector<SyncEntry>& local,
                                 const std::vector<SyncEntry>& remote, const SyncIndex& base,
                                 SyncMode mode, const std::string& self_name,
                                 const std::string& peer_name, std::int64_t timestamp,
                                 std::vector<std::string>* conflicted_paths = nullptr,
                                 const std::string& policy = "keep_both");
    static SyncMode parse_mode(const std::string& mode);

private:
    std::vector<SyncEntry> scan(); // 快照（索引命中时复用哈希）
    // 生成本端状态：索引副本 + 墓碑；manifest 为待发送清单（活跃文件 + 墓碑）；
    // baseline（可选）返回本次标记墓碑之前的基线，供 compute_plan 判断变化
    SyncIndex prepare_local_state(const std::vector<SyncEntry>& live, std::int64_t now,
                                  std::vector<SyncEntry>* manifest,
                                  SyncIndex* baseline = nullptr);
    // 删除本端文件：trash_enabled 时先归档
    Result delete_local(const std::string& rel, std::int64_t now);
    std::string index_path() const;

    // 传输辅助（本端实现，供 sync_with/serve 复用）
    Result send_file_to(ByteStream& stream, const SessionKeys& keys, std::uint64_t& seq,
                        const SyncPlanItem& item, SyncStats& stats);
    Result recv_file_from(ByteStream& stream, const SessionKeys& keys, std::uint64_t& seq,
                          const SyncPlanItem& item, ByteBuf& received_hash, SyncStats& stats);

    void auto_loop();

    const Identity& identity_;
    TrustStore& trust_;
    EventBus& events_;
    IDialer& dialer_;
    SyncConfig config_;
    std::string data_dir_;
    std::vector<std::string> ignore_;

    mutable std::mutex index_mu_;
    SyncIndex index_;
    bool index_loaded_ = false;
    VersionStore versions_;

    std::mutex session_mu_; // 同一时刻只跑一个同步会话
    std::atomic<bool> auto_stop_{false};
    std::atomic<bool> dirty_{false};
    std::thread auto_thread_;
    std::shared_ptr<IFileWatcher> watcher_;

    void publish_started(const DeviceId& peer, const std::string& name);
    void publish_failed(const DeviceId& peer, const std::string& name, const std::string& reason);
};

} // namespace lb
