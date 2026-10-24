#include "core/config/config.h"
#include "core/sync/sync.h"
#include "core/sync/version_store.h"

#include "test.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace lb;

namespace {

std::string make_temp_dir(const char* tag) {
    const auto base = std::filesystem::temp_directory_path() /
                      (std::string("lb_sync_") + tag + "_" + std::to_string(now_secs()));
    std::filesystem::remove_all(base);
    std::filesystem::create_directories(base);
    return base.string();
}

ByteBuf hash_of(std::uint8_t seed) { return ByteBuf(32, seed); }

SyncEntry entry(const std::string& path, std::uint8_t seed) {
    SyncEntry e;
    e.path = path;
    e.size = 10 + seed;
    e.mtime = 1000 + seed;
    e.hash = hash_of(seed);
    return e;
}

SyncEntry tombstone(const std::string& path, std::uint8_t seed, std::int64_t at) {
    SyncEntry e = entry(path, seed);
    e.deleted = true;
    e.deleted_at = at;
    return e;
}

void write_file(const std::string& path, const std::string& text) {
    std::ofstream out(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

std::string read_file(const std::string& path) {
    std::ifstream in(std::filesystem::path(path), std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

const SyncPlanItem* find_item(const std::vector<SyncPlanItem>& list, const std::string& path) {
    for (const auto& i : list)
        if (i.path == path) return &i;
    return nullptr;
}

} // namespace

// ---------------- 忽略规则 ----------------

TEST(sync_split_ignore) {
    const auto pats = SyncService::split_ignore(" *.tmp , ~$* ,.git/, node_modules/ ,, .linkbridge/");
    CHECK(pats.size() == 5);
    if (pats.size() == 5) {
        CHECK_STR_EQ(pats[0], "*.tmp");
        CHECK_STR_EQ(pats[1], "~$*");
        CHECK_STR_EQ(pats[2], ".git/");
        CHECK_STR_EQ(pats[3], "node_modules/");
        CHECK_STR_EQ(pats[4], ".linkbridge/");
    }
    CHECK(SyncService::split_ignore("").empty());
}

TEST(sync_ignore_glob_semantics) {
    const std::vector<std::string> pats = {"*.tmp", "build/*.o", "**/cache/**", ".git/", "!keep.tmp"};

    CHECK(SyncService::ignore_match("a.tmp", pats));
    CHECK(SyncService::ignore_match("dir/sub/a.tmp", pats)); // 无 '/' 的模式匹配 basename
    CHECK(SyncService::ignore_match("build/x.o", pats));
    CHECK(!SyncService::ignore_match("build/sub/x.o", pats)); // * 不跨 '/'
    CHECK(SyncService::ignore_match("x/cache/y.bin", pats));  // ** 跨 '/'
    CHECK(SyncService::ignore_match("cache/y.bin", pats));
    CHECK(SyncService::ignore_match(".git", pats));
    CHECK(SyncService::ignore_match(".git/objects/x", pats));
    CHECK(SyncService::ignore_match("src/.git/config", pats)); // 任意层级同名目录
    CHECK(!SyncService::ignore_match("keep.tmp", pats));       // 取反规则生效
    CHECK(!SyncService::ignore_match("normal.txt", pats));
    CHECK(!SyncService::ignore_match("sub/a.TMP", pats)); // 大小写敏感（与 .stignore 一致）
}

// ---------------- 路径与冲突命名 ----------------

TEST(sync_sanitize_rel_path) {
    const char* ok_paths[] = {"a.txt", "docs/readme.md", "中文/文件.txt", "a/b/c.bin"};
    for (const char* p : ok_paths) {
        std::string rel = p;
        CHECK(SyncService::sanitize_rel_path(rel).success);
        CHECK_STR_EQ(rel, p);
    }

    std::string backslash = "docs\\readme.md";
    CHECK(SyncService::sanitize_rel_path(backslash).success);
    CHECK_STR_EQ(backslash, "docs/readme.md");

    std::string dotted = "./docs/readme.md";
    CHECK(SyncService::sanitize_rel_path(dotted).success);
    CHECK_STR_EQ(dotted, "docs/readme.md");

    const char* bad_paths[] = {"",          "../evil.txt", "/abs.txt", "C:/x.txt",
                               "a/../b",    "a//b",       "a/./b",    ".linkbridge/x",
                               ".linkbridge"};
    for (const char* p : bad_paths) {
        std::string rel = p;
        CHECK(!SyncService::sanitize_rel_path(rel).success);
    }
}

TEST(sync_conflict_name) {
    CHECK_STR_EQ(SyncService::conflict_name("readme.txt", "dev-A", 0),
                 "readme (conflict dev-A 19700101-000000).txt");
    CHECK_STR_EQ(SyncService::conflict_name("docs/a.tar.gz", "dev A", 0),
                 "docs/a.tar (conflict dev_A 19700101-000000).gz");
    CHECK_STR_EQ(SyncService::conflict_name("noext", "dev", 0),
                 "noext (conflict dev 19700101-000000)");
    CHECK_STR_EQ(SyncService::conflict_name(".hidden", "dev", 0),
                 ".hidden (conflict dev 19700101-000000)");
}

TEST(sync_mode_parse) {
    CHECK(SyncService::parse_mode("send_receive") == SyncMode::SendReceive);
    CHECK(SyncService::parse_mode("send_only") == SyncMode::SendOnly);
    CHECK(SyncService::parse_mode("receive_only") == SyncMode::ReceiveOnly);
    CHECK(SyncService::parse_mode("mirror") == SyncMode::Mirror);
    CHECK(SyncService::parse_mode("bogus") == SyncMode::SendReceive);
    CHECK_STR_EQ(sync_mode_name(SyncMode::Mirror), "mirror");
}

// ---------------- 索引持久化 ----------------

TEST(sync_index_roundtrip) {
    const std::string dir = make_temp_dir("index");
    const std::string path = dir + "\\sync_index.bin";

    SyncIndex idx;
    CHECK(idx.load(path).has_value()); // 不存在时为空索引
    idx.set("a.txt", SyncIndexEntry{12, 34, hash_of(7), false});
    idx.set("dir/b.txt", SyncIndexEntry{56, 78, hash_of(9), true});
    idx.set("gone.txt", SyncIndexEntry{1, 2, hash_of(3), false, true, 999});
    CHECK(idx.save(path).success);

    auto loaded = SyncIndex::load(path);
    CHECK(loaded.has_value());
    if (loaded) {
        CHECK(loaded->entries.size() == 3);
        const SyncIndexEntry* a = loaded->find("a.txt");
        CHECK(a != nullptr);
        if (a) {
            CHECK(a->size == 12);
            CHECK(a->mtime == 34);
            CHECK(a->hash == hash_of(7));
            CHECK(!a->conflicted);
            CHECK(!a->deleted);
        }
        const SyncIndexEntry* b = loaded->find("dir/b.txt");
        CHECK(b != nullptr);
        if (b) CHECK(b->conflicted);
        const SyncIndexEntry* g = loaded->find("gone.txt");
        CHECK(g != nullptr);
        if (g) {
            CHECK(g->deleted);
            CHECK(g->deleted_at == 999);
        }

        // 再次保存后仍可读，且条目不变
        CHECK(loaded->save(path).success);
        auto again = SyncIndex::load(path);
        CHECK(again.has_value());
        if (again) CHECK(again->entries.size() == 3);
    }

    std::filesystem::remove_all(dir);
}

// ---------------- 差量计划 ----------------

TEST(sync_plan_initial_sync) {
    const std::vector<SyncEntry> local = {entry("a.txt", 1), entry("same.txt", 2)};
    const std::vector<SyncEntry> remote = {entry("same.txt", 2), entry("c.txt", 3)};
    const SyncIndex base;

    const SyncPlan plan = SyncService::compute_plan(local, remote, base, SyncMode::SendReceive,
                                                    "SELF", "PEER", 0);
    CHECK(plan.want.size() == 1);
    CHECK(plan.send.size() == 1);
    CHECK(plan.conflicts.empty());
    const auto* want = find_item(plan.want, "c.txt");
    CHECK(want != nullptr);
    if (want) CHECK_STR_EQ(want->target_path, "c.txt");
    const auto* send = find_item(plan.send, "a.txt");
    CHECK(send != nullptr);
    if (send) CHECK_STR_EQ(send->target_path, "a.txt");
}

TEST(sync_plan_one_side_changed) {
    SyncIndex base;
    base.set("a.txt", SyncIndexEntry{0, 0, hash_of(1), false});

    // 本端未改、对端改 → 拉取对端版本覆盖本地
    {
        const std::vector<SyncEntry> local = {entry("a.txt", 1)};
        const std::vector<SyncEntry> remote = {entry("a.txt", 5)};
        const SyncPlan plan = SyncService::compute_plan(local, remote, base,
                                                        SyncMode::SendReceive, "SELF", "PEER", 0);
        CHECK(plan.conflicts.empty());
        CHECK(plan.want.size() == 1);
        CHECK(plan.send.empty());
        if (plan.want.size() == 1) CHECK_STR_EQ(plan.want[0].target_path, "a.txt");
    }
    // 本端改、对端未改 → 推送本端版本
    {
        SyncIndex b2;
        b2.set("a.txt", SyncIndexEntry{0, 0, hash_of(5), false});
        const std::vector<SyncEntry> local = {entry("a.txt", 1)};
        const std::vector<SyncEntry> remote = {entry("a.txt", 5)};
        const SyncPlan plan = SyncService::compute_plan(local, remote, b2,
                                                        SyncMode::SendReceive, "SELF", "PEER", 0);
        CHECK(plan.conflicts.empty());
        CHECK(plan.send.size() == 1);
        CHECK(plan.want.empty());
    }
}

TEST(sync_plan_conflict_keep_both) {
    SyncIndex base;
    base.set("docs/report.txt", SyncIndexEntry{0, 0, hash_of(1), false});

    std::vector<std::string> conflicted;
    const std::vector<SyncEntry> local = {entry("docs/report.txt", 2)};
    const std::vector<SyncEntry> remote = {entry("docs/report.txt", 3)};
    const SyncPlan plan = SyncService::compute_plan(local, remote, base, SyncMode::SendReceive,
                                                    "SELF", "PEER", 0, &conflicted);

    CHECK(plan.conflicts.size() == 1);
    CHECK(conflicted.size() == 1);
    if (!plan.conflicts.empty()) CHECK_STR_EQ(plan.conflicts[0], "docs/report.txt");
    CHECK(plan.want.size() == 1);
    CHECK(plan.send.size() == 1);
    if (plan.want.size() == 1) {
        CHECK_STR_EQ(plan.want[0].path, "docs/report.txt");
        CHECK_STR_EQ(plan.want[0].target_path,
                     "docs/report (conflict PEER 19700101-000000).txt");
        CHECK(plan.want[0].hash == hash_of(3));
    }
    if (plan.send.size() == 1) {
        CHECK_STR_EQ(plan.send[0].target_path,
                     "docs/report (conflict SELF 19700101-000000).txt");
        CHECK(plan.send[0].hash == hash_of(2));
    }

    // 无基线且双方文件不同，同样判为冲突
    const SyncPlan no_base = SyncService::compute_plan(local, remote, SyncIndex(),
                                                       SyncMode::SendReceive, "SELF", "PEER", 0);
    CHECK(no_base.conflicts.size() == 1);
    CHECK(no_base.want.size() == 1);
    CHECK(no_base.send.size() == 1);
}

TEST(sync_plan_skips_conflicted_path) {
    SyncIndex base;
    base.set("a.txt", SyncIndexEntry{0, 0, hash_of(1), true});
    const std::vector<SyncEntry> local = {entry("a.txt", 2)};
    const std::vector<SyncEntry> remote = {entry("a.txt", 3)};
    const SyncPlan plan = SyncService::compute_plan(local, remote, base, SyncMode::SendReceive,
                                                    "SELF", "PEER", 0);
    CHECK(plan.want.empty());
    CHECK(plan.send.empty());
    CHECK(plan.conflicts.empty());
}

TEST(sync_plan_propagates_delete) {
    SyncIndex base;
    base.set("gone.txt", SyncIndexEntry{0, 0, hash_of(1), false});

    // 本端墓碑 + 对端未修改 → 传播删除
    {
        const std::vector<SyncEntry> local = {tombstone("gone.txt", 1, 500)};
        const std::vector<SyncEntry> remote = {entry("gone.txt", 1)};
        const SyncPlan plan = SyncService::compute_plan(local, remote, base,
                                                        SyncMode::SendReceive, "SELF", "PEER", 0);
        CHECK(plan.want.empty());
        CHECK(plan.send.empty());
        CHECK(plan.want_delete.size() == 1);
        if (plan.want_delete.size() == 1) CHECK_STR_EQ(plan.want_delete[0], "gone.txt");
    }
    // 本端墓碑 + 对端已修改 → 修改优先，复活对端
    {
        const std::vector<SyncEntry> local = {tombstone("gone.txt", 1, 500)};
        const std::vector<SyncEntry> remote = {entry("gone.txt", 4)};
        const SyncPlan plan = SyncService::compute_plan(local, remote, base,
                                                        SyncMode::SendReceive, "SELF", "PEER", 0);
        CHECK(plan.want_delete.empty());
        CHECK(plan.want.size() == 1);
        CHECK(plan.send.empty());
        if (plan.want.size() == 1) CHECK_STR_EQ(plan.want[0].path, "gone.txt");
    }
    // 本端修改 + 对端墓碑 → 修改优先，不删除本地
    {
        const std::vector<SyncEntry> local = {entry("gone.txt", 2)};
        const std::vector<SyncEntry> remote = {tombstone("gone.txt", 1, 500)};
        const SyncPlan plan = SyncService::compute_plan(local, remote, base,
                                                        SyncMode::SendReceive, "SELF", "PEER", 0);
        CHECK(plan.send_delete.empty());
        CHECK(plan.send.size() == 1);
        CHECK(plan.want.empty());
    }
    // 本端未修改 + 对端墓碑 → 跟随删除
    {
        const std::vector<SyncEntry> local = {entry("gone.txt", 1)};
        const std::vector<SyncEntry> remote = {tombstone("gone.txt", 1, 500)};
        const SyncPlan plan = SyncService::compute_plan(local, remote, base,
                                                        SyncMode::SendReceive, "SELF", "PEER", 0);
        CHECK(plan.send.empty());
        CHECK(plan.want.empty());
        CHECK(plan.send_delete.size() == 1);
        if (plan.send_delete.size() == 1) CHECK_STR_EQ(plan.send_delete[0], "gone.txt");
    }
    // 双方墓碑 → 无动作
    {
        const std::vector<SyncEntry> local = {tombstone("gone.txt", 1, 400)};
        const std::vector<SyncEntry> remote = {tombstone("gone.txt", 1, 500)};
        const SyncPlan plan = SyncService::compute_plan(local, remote, base,
                                                        SyncMode::SendReceive, "SELF", "PEER", 0);
        CHECK(plan.want.empty());
        CHECK(plan.send.empty());
        CHECK(plan.want_delete.empty());
        CHECK(plan.send_delete.empty());
    }
}

TEST(sync_plan_revives_restored_file) {
    // 基线为墓碑（曾同步删除过），本端恢复出旧内容 → 推送复活对端
    SyncIndex base;
    base.set("photo.jpg", SyncIndexEntry{0, 0, hash_of(1), false, true, 500});
    const std::vector<SyncEntry> local = {entry("photo.jpg", 1)};
    const std::vector<SyncEntry> remote = {tombstone("photo.jpg", 1, 500)};
    const SyncPlan plan = SyncService::compute_plan(local, remote, base, SyncMode::SendReceive,
                                                    "SELF", "PEER", 0);
    CHECK(plan.send.size() == 1);
    CHECK(plan.send_delete.empty());
    CHECK(plan.want_delete.empty());
}

TEST(sync_plan_conflict_policies) {
    SyncIndex base;
    base.set("f.bin", SyncIndexEntry{0, 0, hash_of(1), false});
    const std::vector<SyncEntry> local = {entry("f.bin", 5)};  // mtime/size 都更小
    const std::vector<SyncEntry> remote = {entry("f.bin", 8)}; // 更新/更大

    {
        const SyncPlan plan = SyncService::compute_plan(local, remote, base,
                                                        SyncMode::SendReceive, "SELF", "PEER", 0,
                                                        nullptr, "newer");
        CHECK(plan.conflicts.empty());
        CHECK(plan.want.size() == 1);
        CHECK(plan.send.empty());
        if (plan.want.size() == 1) CHECK_STR_EQ(plan.want[0].target_path, "f.bin");
    }
    {
        const SyncPlan plan = SyncService::compute_plan(local, remote, base,
                                                        SyncMode::SendReceive, "SELF", "PEER", 0,
                                                        nullptr, "larger");
        CHECK(plan.want.size() == 1);
        CHECK(plan.send.empty());
    }
    {
        // 本端更新/更大 → 推送本端
        const std::vector<SyncEntry> newer_local = {entry("f.bin", 9)};
        const SyncPlan plan = SyncService::compute_plan(newer_local, remote, base,
                                                        SyncMode::SendReceive, "SELF", "PEER", 0,
                                                        nullptr, "newer");
        CHECK(plan.send.size() == 1);
        CHECK(plan.want.empty());
    }
}

TEST(sync_plan_send_only_and_receive_only) {
    const std::vector<SyncEntry> local = {entry("a.txt", 1), entry("same.txt", 2)};
    const std::vector<SyncEntry> remote = {entry("same.txt", 2), entry("c.txt", 3)};

    const SyncPlan send_only = SyncService::compute_plan(local, remote, SyncIndex(),
                                                         SyncMode::SendOnly, "SELF", "PEER", 0);
    CHECK(send_only.want.empty());
    CHECK(send_only.send.size() == 1);
    if (send_only.send.size() == 1) CHECK_STR_EQ(send_only.send[0].path, "a.txt");

    const SyncPlan receive_only =
        SyncService::compute_plan(local, remote, SyncIndex(), SyncMode::ReceiveOnly, "SELF", "PEER", 0);
    CHECK(receive_only.send.empty());
    CHECK(receive_only.want.size() == 1);
    if (receive_only.want.size() == 1) CHECK_STR_EQ(receive_only.want[0].path, "c.txt");

    // mirror 与 send_only 相同语义
    const SyncPlan mirror = SyncService::compute_plan(local, remote, SyncIndex(), SyncMode::Mirror,
                                                      "SELF", "PEER", 0);
    CHECK(mirror.want.empty());
    CHECK(mirror.send.size() == 1);
}

TEST(sync_plan_mode_delete_propagation) {
    SyncIndex base;
    base.set("gone.txt", SyncIndexEntry{0, 0, hash_of(1), false});
    base.set("keep.txt", SyncIndexEntry{0, 0, hash_of(2), false});

    // send_only / mirror：本端为权威源，推送本端删除
    for (SyncMode mode : {SyncMode::SendOnly, SyncMode::Mirror}) {
        const std::vector<SyncEntry> local = {tombstone("gone.txt", 1, 500),
                                              entry("keep.txt", 2)};
        const std::vector<SyncEntry> remote = {entry("gone.txt", 1), entry("keep.txt", 2)};
        const SyncPlan plan = SyncService::compute_plan(local, remote, base, mode, "SELF", "PEER", 0);
        CHECK(plan.want_delete.size() == 1);
        if (plan.want_delete.size() == 1) CHECK_STR_EQ(plan.want_delete[0], "gone.txt");
    }

    // receive_only：跟随对端删除（本地未改）
    {
        const std::vector<SyncEntry> local = {entry("gone.txt", 1), entry("keep.txt", 2)};
        const std::vector<SyncEntry> remote = {tombstone("gone.txt", 1, 500), entry("keep.txt", 2)};
        const SyncPlan plan = SyncService::compute_plan(local, remote, base,
                                                        SyncMode::ReceiveOnly, "SELF", "PEER", 0);
        CHECK(plan.send_delete.size() == 1);
        if (plan.send_delete.size() == 1) CHECK_STR_EQ(plan.send_delete[0], "gone.txt");
    }
}

// ---------------- 版本库/回收站 ----------------

TEST(sync_version_store_archive_and_restore) {
    const std::string dir = make_temp_dir("versions");
    const std::string file = dir + "\\a.txt";
    write_file(file, "v1");

    VersionStore store(dir, 5, 30, 1024);
    const std::int64_t now = 1790000000;
    CHECK(store.archive("a.txt", now).success);
    CHECK(!std::filesystem::exists(file));

    const auto list = store.list("a.txt");
    CHECK(list.size() == 1);
    if (list.size() == 1) {
        CHECK_STR_EQ(list[0].path, "a.txt");
        CHECK(list[0].size == 2);
        CHECK(list[0].archived_at == VersionStore::parse_id_time(list[0].id));
    }

    CHECK(store.restore("a.txt", "", now + 10).success);
    CHECK(std::filesystem::exists(file));
    CHECK_STR_EQ(read_file(file), "v1");
    // 恢复后版本文件被消费，回收站为空
    CHECK(store.list("a.txt").empty());

    std::filesystem::remove_all(dir);
}

TEST(sync_version_store_prune_count_and_days) {
    const std::string dir = make_temp_dir("prune");
    VersionStore store(dir, 2, 30, 1024);
    const std::int64_t base = 1790000000;

    for (int i = 0; i < 3; ++i) {
        write_file(dir + "\\a.txt", "v" + std::to_string(i));
        CHECK(store.archive("a.txt", base + i).success);
    }
    store.prune(base + 10);
    CHECK(store.list("a.txt").size() == 2); // 数量上限 2

    // 过期清理：保留 30 天，用 60 天后的时间清理
    store.prune(base + 60 * 86400);
    CHECK(store.list("a.txt").empty());

    std::filesystem::remove_all(dir);
}

// ---------------- 配置校验 ----------------

TEST(sync_config_validation) {
    AppConfig cfg;
    cfg.device_name = "sync-test";
    CHECK(validate_config(cfg).success);

    cfg.sync.enabled = true; // folder 为空 → 报错
    CHECK(!validate_config(cfg).success);

    cfg.sync.folder = "C:\\sync";
    CHECK(validate_config(cfg).success);

    cfg.sync.mode = "two_way";
    CHECK(!validate_config(cfg).success);
    cfg.sync.mode = "send_receive";

    cfg.sync.default_conflict_policy = "newest_wins";
    CHECK(!validate_config(cfg).success);
    cfg.sync.default_conflict_policy = "keep_both";

    cfg.sync.max_file_size_mb = 0;
    CHECK(!validate_config(cfg).success);
    cfg.sync.max_file_size_mb = 4096;

    cfg.sync.rescan_interval_sec = 4;
    CHECK(!validate_config(cfg).success);
    cfg.sync.rescan_interval_sec = 3600;

    cfg.sync.max_concurrent_files = 0;
    CHECK(!validate_config(cfg).success);
    cfg.sync.max_concurrent_files = 4;

    cfg.sync.version_keep_count = -1;
    CHECK(!validate_config(cfg).success);
    cfg.sync.version_keep_count = 5;

    cfg.sync.version_keep_days = 4000;
    CHECK(!validate_config(cfg).success);
    cfg.sync.version_keep_days = 30;

    cfg.sync.trash_max_mb = -1;
    CHECK(!validate_config(cfg).success);
    cfg.sync.trash_max_mb = 2048;

    cfg.sync.rate_limit_kbps = -1;
    CHECK(!validate_config(cfg).success);
    cfg.sync.rate_limit_kbps = 0;

    cfg.network.sync_port = cfg.network.screen_port; // 端口重复
    CHECK(!validate_config(cfg).success);
    cfg.network.sync_port = 45716;
    CHECK(validate_config(cfg).success);

    // 默认配置文本必须包含同步端口与 [sync] 段
    const std::string text = default_config_text();
    CHECK(text.find("sync_port = 45716") != std::string::npos);
    CHECK(text.find("[sync]") != std::string::npos);
    CHECK(text.find("keep_both") != std::string::npos);
    CHECK(text.find("trash_enabled") != std::string::npos);
    CHECK(text.find("version_keep_count") != std::string::npos);
    CHECK(text.find("rate_limit_kbps") != std::string::npos);
}
