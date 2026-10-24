#include "core/sync/sync.h"

#include "core/common/bytes.h"
#include "core/crypto/crypto.h"
#include "core/platform/platform.h"
#include "core/sync/watcher.h"
#include "core/transfer/transfer.h"
#include "net/secure_channel.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

namespace lb {
namespace {

constexpr std::uint8_t kHello = 0x71;
constexpr std::uint8_t kManifest = 0x72;
constexpr std::uint8_t kPlan = 0x73;
constexpr std::uint8_t kFileStart = 0x74;
constexpr std::uint8_t kFileChunk = 0x75;
constexpr std::uint8_t kFileEnd = 0x76;
constexpr std::uint8_t kApplyOk = 0x77;
constexpr std::uint8_t kApplyFail = 0x78;
constexpr std::uint8_t kDone = 0x79;
constexpr std::uint8_t kFail = 0x7A;

constexpr std::uint32_t kChunkSize = 1024u * 1024u;
constexpr size_t kChunkSlack = 64u * 1024u;
constexpr size_t kMaxMetaFrame = 32u * 1024u * 1024u; // 清单/计划帧上限
constexpr size_t kMaxEntries = 200000;
constexpr std::uint64_t kMaxHashFileSize = 64ull * 1024 * 1024 * 1024;
constexpr char kInternalDir[] = ".linkbridge";

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

std::string trim(const std::string& s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

bool glob_rec(const std::string& p, size_t pi, const std::string& s, size_t si) {
    if (pi == p.size()) return si == s.size();
    if (p[pi] == '*') {
        const bool dbl = pi + 1 < p.size() && p[pi + 1] == '*';
        if (dbl) {
            // "**/" 也匹配零个目录层级（gitignore 语义），如 "**/cache/**" 命中 "cache/x"
            if (pi + 2 < p.size() && p[pi + 2] == '/' && glob_rec(p, pi + 3, s, si)) return true;
            for (size_t k = si; k <= s.size(); ++k)
                if (glob_rec(p, pi + 2, s, k)) return true;
            return false;
        }
        for (size_t k = si; k <= s.size(); ++k) {
            if (k < s.size() && s[k] == '/') break;
            if (glob_rec(p, pi + 1, s, k)) return true;
        }
        return false;
    }
    if (si == s.size()) return false;
    if (p[pi] == '?') {
        if (s[si] == '/') return false;
        return glob_rec(p, pi + 1, s, si + 1);
    }
    if (p[pi] != s[si]) return false;
    return glob_rec(p, pi + 1, s, si + 1);
}

bool glob_match(const std::string& pattern, const std::string& text) {
    return glob_rec(pattern, 0, text, 0);
}

// 目录型忽略（pattern 以 '/' 结尾）：匹配任意层级的同名目录前缀
bool dir_prefix_match(const std::string& rel, const std::string& dir) {
    if (rel == dir) return true;
    if (rel.rfind(dir + "/", 0) == 0) return true;
    if (rel.find("/" + dir + "/") != std::string::npos) return true;
    if (rel.size() > dir.size() + 1 &&
        rel.compare(rel.size() - dir.size() - 1, dir.size() + 1, "/" + dir) == 0)
        return true;
    return false;
}

Expected<ByteBuf> hash_file(const std::string& path) {
    std::ifstream in(to_path(path), std::ios::binary);
    if (!in) return Expected<ByteBuf>::fail("打开文件失败: " + path);
    crypto::Sha256Hasher hasher;
    std::array<char, 65536> buf{};
    while (in) {
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        const auto got = in.gcount();
        if (got > 0 && !hasher.update(buf.data(), static_cast<size_t>(got)))
            return Expected<ByteBuf>::fail("计算文件哈希失败: " + path);
    }
    if (!in.eof()) return Expected<ByteBuf>::fail("读取文件失败: " + path);
    ByteBuf digest = hasher.finish();
    if (digest.size() != crypto::kSha256Len)
        return Expected<ByteBuf>::fail("计算文件哈希失败: " + path);
    return Expected<ByteBuf>::success(std::move(digest));
}

ByteBuf encode_hello(SyncMode mode) {
    ByteWriter w;
    w.u8(kHello);
    w.str(sync_mode_name(mode));
    return w.buf();
}

ByteBuf encode_manifest(const std::vector<SyncEntry>& entries) {
    ByteWriter w;
    w.u8(kManifest);
    w.u32(static_cast<std::uint32_t>(entries.size()));
    for (const auto& e : entries) {
        w.str(e.path);
        w.u64(e.size);
        w.i64(e.mtime);
        w.bytes(e.hash);
        w.u8(e.deleted ? 1 : 0);
        w.i64(e.deleted_at);
    }
    return w.buf();
}

Expected<std::vector<SyncEntry>> decode_manifest(ByteReader& r) {
    const std::uint32_t count = r.u32();
    if (!r.ok() || count > kMaxEntries) return Expected<std::vector<SyncEntry>>::fail("清单条目过多");
    std::vector<SyncEntry> out;
    out.reserve(count);
    std::set<std::string> seen;
    for (std::uint32_t i = 0; i < count; ++i) {
        SyncEntry e;
        e.path = r.str();
        e.size = r.u64();
        e.mtime = r.i64();
        e.hash = r.bytes();
        e.deleted = r.u8() != 0;
        e.deleted_at = r.i64();
        if (!r.ok() || e.hash.size() != crypto::kSha256Len)
            return Expected<std::vector<SyncEntry>>::fail("清单条目格式非法");
        std::string rel = e.path;
        if (auto ok = SyncService::sanitize_rel_path(rel); !ok)
            return Expected<std::vector<SyncEntry>>::fail("清单路径非法: " + ok.error);
        e.path = rel;
        if (!seen.insert(e.path).second)
            return Expected<std::vector<SyncEntry>>::fail("清单路径重复");
        out.push_back(std::move(e));
    }
    return Expected<std::vector<SyncEntry>>::success(std::move(out));
}

void encode_item(ByteWriter& w, const SyncPlanItem& item) {
    w.str(item.path);
    w.str(item.target_path);
    w.u64(item.size);
    w.i64(item.mtime);
    w.bytes(item.hash);
}

Expected<SyncPlanItem> decode_item(ByteReader& r) {
    SyncPlanItem item;
    item.path = r.str();
    item.target_path = r.str();
    item.size = r.u64();
    item.mtime = r.i64();
    item.hash = r.bytes();
    if (!r.ok() || item.hash.size() != crypto::kSha256Len)
        return Expected<SyncPlanItem>::fail("计划条目格式非法");
    std::string rel = item.path;
    if (auto ok = SyncService::sanitize_rel_path(rel); !ok)
        return Expected<SyncPlanItem>::fail("计划源路径非法");
    item.path = rel;
    if (auto ok = SyncService::sanitize_rel_path(item.target_path); !ok)
        return Expected<SyncPlanItem>::fail("计划目标路径非法");
    return Expected<SyncPlanItem>::success(std::move(item));
}

ByteBuf encode_plan(const SyncPlan& plan) {
    ByteWriter w;
    w.u8(kPlan);
    w.u32(static_cast<std::uint32_t>(plan.conflicts.size()));
    for (const auto& c : plan.conflicts) w.str(c);
    w.u32(static_cast<std::uint32_t>(plan.want.size()));
    for (const auto& i : plan.want) encode_item(w, i);
    w.u32(static_cast<std::uint32_t>(plan.send.size()));
    for (const auto& i : plan.send) encode_item(w, i);
    w.u32(static_cast<std::uint32_t>(plan.want_delete.size()));
    for (const auto& p : plan.want_delete) w.str(p);
    w.u32(static_cast<std::uint32_t>(plan.send_delete.size()));
    for (const auto& p : plan.send_delete) w.str(p);
    return w.buf();
}

Expected<SyncPlan> decode_plan(ByteReader& r) {
    SyncPlan plan;
    const std::uint32_t conflicts = r.u32();
    if (!r.ok() || conflicts > kMaxEntries) return Expected<SyncPlan>::fail("冲突列表过大");
    plan.conflicts.reserve(conflicts);
    for (std::uint32_t i = 0; i < conflicts; ++i) {
        std::string c = r.str();
        if (!r.ok()) return Expected<SyncPlan>::fail("计划格式非法");
        std::string rel = c;
        if (auto ok = SyncService::sanitize_rel_path(rel); !ok)
            return Expected<SyncPlan>::fail("冲突路径非法");
        plan.conflicts.push_back(rel);
    }
    for (auto* list : {&plan.want, &plan.send}) {
        const std::uint32_t count = r.u32();
        if (!r.ok() || count > kMaxEntries) return Expected<SyncPlan>::fail("计划条目过多");
        list->reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            auto item = decode_item(r);
            if (!item) return Expected<SyncPlan>::fail(item.error);
            list->push_back(std::move(*item));
        }
    }
    for (auto* list : {&plan.want_delete, &plan.send_delete}) {
        const std::uint32_t count = r.u32();
        if (!r.ok() || count > kMaxEntries) return Expected<SyncPlan>::fail("删除列表过大");
        list->reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            std::string p = r.str();
            if (!r.ok()) return Expected<SyncPlan>::fail("删除路径格式非法");
            std::string rel = p;
            if (auto ok = SyncService::sanitize_rel_path(rel); !ok)
                return Expected<SyncPlan>::fail("删除路径非法");
            list->push_back(rel);
        }
    }
    return Expected<SyncPlan>::success(std::move(plan));
}

ByteBuf encode_fail(const std::string& reason) {
    ByteWriter w;
    w.u8(kFail);
    w.str(reason);
    return w.buf();
}

std::string random_suffix() {
    return to_hex(crypto::random_bytes(8));
}

std::string format_timestamp(std::int64_t ts) {
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

std::string sanitize_device_label(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    for (unsigned char c : name) {
        if (std::isalnum(c) || c == '-' || c == '_') out.push_back(static_cast<char>(c));
        else out.push_back('_');
    }
    if (out.empty()) out = "peer";
    return out;
}

// 由冲突副本名还原原文件名：<stem> (conflict <label> <time>)<ext> → <stem><ext>
std::string conflict_original_name(const std::string& name) {
    const auto pos = name.find(" (conflict ");
    if (pos == std::string::npos) return name;
    const auto close = name.find(')', pos);
    if (close == std::string::npos) return name;
    return name.substr(0, pos) + name.substr(close + 1);
}

std::string conflict_original_path(const std::string& rel) {
    const std::string dir = dirname_of(rel);
    const std::string name = conflict_original_name(basename_of(rel));
    return dir.empty() ? name : dir + "/" + name;
}

} // namespace

// ---------------- 纯函数 ----------------

const char* sync_mode_name(SyncMode mode) {
    switch (mode) {
        case SyncMode::SendReceive: return "send_receive";
        case SyncMode::SendOnly: return "send_only";
        case SyncMode::ReceiveOnly: return "receive_only";
        case SyncMode::Mirror: return "mirror";
    }
    return "send_receive";
}

SyncMode SyncService::parse_mode(const std::string& mode) {
    if (mode == "send_only") return SyncMode::SendOnly;
    if (mode == "receive_only") return SyncMode::ReceiveOnly;
    if (mode == "mirror") return SyncMode::Mirror;
    return SyncMode::SendReceive;
}

std::vector<std::string> SyncService::split_ignore(const std::string& csv) {
    std::vector<std::string> out;
    std::stringstream ss(csv);
    std::string item;
    while (std::getline(ss, item, ',')) {
        item = trim(item);
        if (!item.empty()) out.push_back(item);
    }
    return out;
}

bool SyncService::ignore_match(const std::string& rel, const std::vector<std::string>& patterns) {
    const std::string base = basename_of(rel);
    bool ignored = false;
    for (const auto& raw : patterns) {
        std::string pat = trim(raw);
        if (pat.empty()) continue;
        bool negate = false;
        if (pat[0] == '!') {
            negate = true;
            pat.erase(pat.begin());
        }
        std::replace(pat.begin(), pat.end(), '\\', '/');
        if (pat.empty()) continue;
        const bool dir_only = pat.back() == '/';
        if (dir_only) pat.pop_back();
        if (pat.empty()) continue;

        bool hit = false;
        if (dir_only) {
            hit = dir_prefix_match(rel, pat);
        } else if (pat.find('/') == std::string::npos) {
            hit = glob_match(pat, base);
        } else {
            hit = glob_match(pat, rel);
        }
        if (hit) ignored = !negate;
    }
    return ignored;
}

Result SyncService::sanitize_rel_path(std::string& rel) {
    if (rel.empty()) return Result::fail("路径为空");
    if (rel.size() > 4096) return Result::fail("路径过长");
    std::replace(rel.begin(), rel.end(), '\\', '/');
    while (rel.rfind("./", 0) == 0) rel.erase(0, 2);
    if (rel.empty()) return Result::fail("路径为空");
    if (rel.front() == '/') return Result::fail("不允许绝对路径");
    if (rel.size() >= 2 && std::isalpha(static_cast<unsigned char>(rel[0])) && rel[1] == ':')
        return Result::fail("不允许盘符路径");
    if (rel.find(':') != std::string::npos) return Result::fail("路径含非法字符");

    size_t start = 0;
    while (start <= rel.size()) {
        const size_t pos = rel.find('/', start);
        const std::string seg =
            pos == std::string::npos ? rel.substr(start) : rel.substr(start, pos - start);
        if (seg.empty()) return Result::fail("路径含空段");
        if (seg == "." || seg == "..") return Result::fail("路径不允许包含 . 或 ..");
        auto sane = TransferService::sanitize_file_name(seg);
        if (!sane) return Result::fail("路径段非法: " + sane.error);
        if (pos == std::string::npos) break;
        start = pos + 1;
    }
    if (rel.rfind(std::string(kInternalDir) + "/", 0) == 0 || rel == kInternalDir)
        return Result::fail("路径指向内部目录");
    return Result::ok();
}

std::string SyncService::conflict_name(const std::string& rel, const std::string& device_name,
                                       std::int64_t timestamp) {
    const std::string dir = dirname_of(rel);
    const std::string base = basename_of(rel);
    const auto dot = base.find_last_of('.');
    const std::string stem = (dot == std::string::npos || dot == 0) ? base : base.substr(0, dot);
    const std::string ext = (dot == std::string::npos || dot == 0) ? "" : base.substr(dot);
    const std::string name = stem + " (conflict " + sanitize_device_label(device_name) + " " +
                             format_timestamp(timestamp) + ")" + ext;
    return dir.empty() ? name : dir + "/" + name;
}

namespace {

// newer/larger 冲突策略的胜者判定：true = 保留本端（响应方）版本
bool prefer_local(const SyncEntry& local, const SyncEntry& remote, const std::string& policy) {
    if (policy == "newer") {
        if (local.mtime != remote.mtime) return local.mtime > remote.mtime;
        if (local.size != remote.size) return local.size > remote.size;
        return local.hash < remote.hash; // 字典序小者胜，保证确定性
    }
    if (policy == "larger") {
        if (local.size != remote.size) return local.size > remote.size;
        if (local.mtime != remote.mtime) return local.mtime > remote.mtime;
        return local.hash < remote.hash;
    }
    return true;
}

} // namespace

SyncPlan SyncService::compute_plan(const std::vector<SyncEntry>& local,
                                   const std::vector<SyncEntry>& remote, const SyncIndex& base,
                                   SyncMode mode, const std::string& self_name,
                                   const std::string& peer_name, std::int64_t timestamp,
                                   std::vector<std::string>* conflicted_paths,
                                   const std::string& policy) {
    SyncPlan plan;
    std::map<std::string, const SyncEntry*> lmap;
    std::map<std::string, const SyncEntry*> rmap;
    for (const auto& e : local) lmap[e.path] = &e;
    for (const auto& e : remote) rmap[e.path] = &e;

    auto is_conflicted = [&](const std::string& path) {
        const auto* b = base.find(path);
        return b && b->conflicted;
    };
    auto add_conflict = [&](const std::string& path) {
        plan.conflicts.push_back(path);
        if (conflicted_paths) conflicted_paths->push_back(path);
    };
    auto make_item = [](const SyncEntry& e, const std::string& target) {
        SyncPlanItem item;
        item.path = e.path;
        item.target_path = target;
        item.size = e.size;
        item.mtime = e.mtime;
        item.hash = e.hash;
        return item;
    };
    auto live = [](const SyncEntry* e) { return e != nullptr && !e->deleted; };

    if (mode == SyncMode::SendOnly || mode == SyncMode::Mirror) {
        // 本端为权威源：推送新增/修改，并把本端删除传播到对端
        for (const auto& kv : lmap) {
            if (is_conflicted(kv.first)) continue;
            const SyncEntry& e = *kv.second;
            if (e.deleted) {
                const auto* b = base.find(kv.first);
                if (b && !b->deleted) plan.want_delete.push_back(kv.first);
                continue;
            }
            const auto r = rmap.find(kv.first);
            if (r == rmap.end() || r->second->deleted || r->second->hash != e.hash)
                plan.send.push_back(make_item(e, kv.first));
        }
        return plan;
    }
    if (mode == SyncMode::ReceiveOnly) {
        // 本端只接收：拉取对端新增/修改，并跟随对端删除（本地改过的保留）
        for (const auto& kv : rmap) {
            if (is_conflicted(kv.first)) continue;
            const SyncEntry& e = *kv.second;
            if (e.deleted) {
                const auto l = lmap.find(kv.first);
                if (l == lmap.end() || l->second->deleted) continue;
                const auto* b = base.find(kv.first);
                if (!b || b->deleted) continue;
                if (l->second->hash == b->hash) plan.send_delete.push_back(kv.first);
                continue;
            }
            const auto l = lmap.find(kv.first);
            if (l == lmap.end() || l->second->deleted || l->second->hash != e.hash)
                plan.want.push_back(make_item(e, kv.first));
        }
        return plan;
    }

    // send_receive：以索引为基线判断哪一侧发生了变化（含墓碑）
    std::set<std::string> paths;
    for (const auto& kv : lmap) paths.insert(kv.first);
    for (const auto& kv : rmap) paths.insert(kv.first);

    for (const auto& path : paths) {
        if (is_conflicted(path)) continue;
        const auto li = lmap.find(path);
        const auto ri = rmap.find(path);
        const SyncEntry* l = li == lmap.end() ? nullptr : li->second;
        const SyncEntry* r = ri == rmap.end() ? nullptr : ri->second;
        const SyncIndexEntry* b = base.find(path);
        const bool l_live = live(l);
        const bool r_live = live(r);

        // 对端独有：本端没有条目，说明是新增文件（或本端因忽略/超限跳过）
        if (!l) {
            if (r_live && (!b || b->hash != r->hash))
                plan.want.push_back(make_item(*r, path));
            continue;
        }
        // 本端独有：对端没有条目，说明是新增（或对端跳过）
        if (!r) {
            if (l_live && (!b || b->hash != l->hash))
                plan.send.push_back(make_item(*l, path));
            continue;
        }

        if (l_live && r_live) {
            if (l->hash == r->hash) continue;
            if (b && !b->deleted) {
                if (b->hash == l->hash) {
                    plan.want.push_back(make_item(*r, path));
                    continue;
                }
                if (b->hash == r->hash) {
                    plan.send.push_back(make_item(*l, path));
                    continue;
                }
            } else if (b && b->deleted) {
                // 基线是墓碑：双方都曾删除过，其中一方又重建了文件
                if (b->hash == l->hash) {
                    plan.want.push_back(make_item(*r, path));
                    continue;
                }
                if (b->hash == r->hash) {
                    plan.send.push_back(make_item(*l, path));
                    continue;
                }
            }
            if (policy == "newer" || policy == "larger") {
                if (prefer_local(*l, *r, policy))
                    plan.send.push_back(make_item(*l, path));
                else
                    plan.want.push_back(make_item(*r, path));
                continue;
            }
            add_conflict(path);
            plan.want.push_back(make_item(*r, conflict_name(path, peer_name, timestamp)));
            plan.send.push_back(make_item(*l, conflict_name(path, self_name, timestamp)));
            continue;
        }

        if (l_live && !r_live) {
            // 对端删除。本端未修改 → 跟随删除；本端修改/新增 → 修改优先，复活对端
            if (b && !b->deleted && b->hash == l->hash)
                plan.send_delete.push_back(path);
            else
                plan.send.push_back(make_item(*l, path));
            continue;
        }

        if (!l_live && r_live) {
            // 本端删除。对端未修改 → 传播删除；对端修改 → 修改优先，复活本端
            if (b && !b->deleted && b->hash == r->hash)
                plan.want_delete.push_back(path);
            else
                plan.want.push_back(make_item(*r, path));
            continue;
        }
        // 双方墓碑：保留墓碑，无需动作
    }
    return plan;
}

// ---------------- SyncIndex ----------------

const SyncIndexEntry* SyncIndex::find(const std::string& path) const {
    const auto it = entries.find(path);
    return it == entries.end() ? nullptr : &it->second;
}

void SyncIndex::set(const std::string& path, SyncIndexEntry entry) { entries[path] = std::move(entry); }

Expected<SyncIndex> SyncIndex::load(const std::string& path) {
    SyncIndex idx;
    if (!pal::file_exists(path)) return Expected<SyncIndex>::success(std::move(idx));
    auto data = pal::read_file(path);
    if (!data) return Expected<SyncIndex>::fail(data.error);
    ByteReader r(*data);
    const ByteBuf magic = r.raw(4);
    if (!r.ok() || std::string(magic.begin(), magic.end()) != "LBS2")
        return Expected<SyncIndex>::fail("同步索引版本不兼容（需 LBS2）");
    const std::uint32_t count = r.u32();
    if (!r.ok() || count > kMaxEntries) return Expected<SyncIndex>::fail("同步索引记录数异常");
    for (std::uint32_t i = 0; i < count; ++i) {
        SyncIndexEntry e;
        const std::string p = r.str();
        e.size = r.u64();
        e.mtime = r.i64();
        e.hash = r.bytes();
        e.conflicted = r.u8() != 0;
        e.deleted = r.u8() != 0;
        e.deleted_at = r.i64();
        if (!r.ok() || e.hash.size() != crypto::kSha256Len)
            return Expected<SyncIndex>::fail("同步索引格式损坏");
        idx.entries[p] = std::move(e);
    }
    return Expected<SyncIndex>::success(std::move(idx));
}

Result SyncIndex::save(const std::string& path) const {
    ByteWriter w;
    w.raw("LBS2", 4);
    w.u32(static_cast<std::uint32_t>(entries.size()));
    for (const auto& kv : entries) {
        w.str(kv.first);
        w.u64(kv.second.size);
        w.i64(kv.second.mtime);
        w.bytes(kv.second.hash);
        w.u8(kv.second.conflicted ? 1 : 0);
        w.u8(kv.second.deleted ? 1 : 0);
        w.i64(kv.second.deleted_at);
    }
    return pal::write_file_atomic(path, w.buf());
}

// ---------------- SyncService ----------------

SyncService::SyncService(const Identity& identity, TrustStore& trust, EventBus& events,
                         IDialer& dialer, const SyncConfig& config, std::string data_dir)
    : identity_(identity),
      trust_(trust),
      events_(events),
      dialer_(dialer),
      config_(config),
      data_dir_(std::move(data_dir)),
      ignore_(split_ignore(config.ignore)),
      versions_(config.folder, config.version_keep_count, config.version_keep_days,
                config.trash_max_mb) {
    auto loaded = SyncIndex::load(index_path());
    if (loaded) index_ = std::move(*loaded);
    index_loaded_ = true;
}

SyncService::~SyncService() { stop_auto(); }

std::string SyncService::index_path() const {
    return pal::join_path(data_dir_, "sync_index.bin");
}

std::size_t SyncService::index_size() const {
    std::lock_guard<std::mutex> lk(index_mu_);
    return index_.entries.size();
}

void SyncService::publish_started(const DeviceId& peer, const std::string& name) {
    events_.publish(SyncStarted{peer, name});
}

void SyncService::publish_failed(const DeviceId& peer, const std::string& name,
                                 const std::string& reason) {
    events_.publish(SyncFailed{peer, name, reason});
}

std::vector<SyncEntry> SyncService::scan() {
    std::vector<SyncEntry> out;
    if (config_.folder.empty()) return out;
    std::error_code ec;
    const auto root = to_path(config_.folder);
    if (!std::filesystem::is_directory(root, ec) || ec) return out;

    std::lock_guard<std::mutex> lk(index_mu_);
    std::filesystem::recursive_directory_iterator it(
        root, std::filesystem::directory_options::skip_permission_denied, ec);
    const std::filesystem::recursive_directory_iterator end;
    while (it != end) {
        std::error_code step_ec;
        const auto& p = it->path();
        std::string rel = to_utf8(p.lexically_relative(root));
        std::replace(rel.begin(), rel.end(), '\\', '/');
        if (rel.empty()) {
            it.increment(step_ec);
            continue;
        }
        const bool is_dir = it->is_directory(step_ec);
        // 内部目录（临时文件/索引）始终跳过，与用户忽略规则无关
        if (rel == kInternalDir || rel.rfind(std::string(kInternalDir) + "/", 0) == 0) {
            if (is_dir) it.disable_recursion_pending();
            it.increment(step_ec);
            continue;
        }
        if (ignore_match(rel, ignore_)) {
            if (is_dir) it.disable_recursion_pending();
            it.increment(step_ec);
            continue;
        }
        if (is_dir) {
            it.increment(step_ec);
            continue;
        }
        if (it->is_symlink(step_ec) || !it->is_regular_file(step_ec)) {
            it.increment(step_ec);
            continue;
        }
        const std::uint64_t size = it->file_size(step_ec);
        if (step_ec) {
            step_ec.clear();
            it.increment(step_ec);
            continue;
        }
        const std::uint64_t max_bytes =
            static_cast<std::uint64_t>(config_.max_file_size_mb) * 1024ull * 1024ull;
        if (size > max_bytes) {
            it.increment(step_ec);
            continue;
        }
        const auto mt = it->last_write_time(step_ec);
        if (step_ec) {
            step_ec.clear();
            it.increment(step_ec);
            continue;
        }
        SyncEntry e;
        e.path = rel;
        e.size = size;
        e.mtime = static_cast<std::int64_t>(mt.time_since_epoch().count());

        // 文件时间戳精度可能只有 1 秒：刚被修改的文件一律重新哈希，避免漏掉
        // “同一秒内等长改写”这类变化；只有“冷却”超过 2 秒的文件才复用索引哈希。
        const auto now = std::filesystem::file_time_type::clock::now();
        const bool stable = (now - mt) > std::chrono::seconds(2);
        const auto* prev = index_.find(rel);
        if (stable && prev && prev->size == e.size && prev->mtime == e.mtime &&
            prev->hash.size() == crypto::kSha256Len) {
            e.hash = prev->hash;
        } else {
            auto digest = hash_file(pal::join_path(config_.folder, rel));
            if (!digest) {
                it.increment(step_ec);
                continue;
            }
            e.hash = std::move(*digest);
        }
        out.push_back(std::move(e));
        it.increment(step_ec);
    }
    return out;
}

SyncIndex SyncService::prepare_local_state(const std::vector<SyncEntry>& live, std::int64_t now,
                                          std::vector<SyncEntry>* manifest,
                                          SyncIndex* baseline) {
    SyncIndex state;
    {
        std::lock_guard<std::mutex> lk(index_mu_);
        state = index_;
    }
    if (baseline) *baseline = state;

    std::set<std::string> live_paths;
    if (manifest) {
        manifest->clear();
        manifest->reserve(live.size() + state.entries.size());
    }
    for (const auto& e : live) {
        live_paths.insert(e.path);
        if (manifest) manifest->push_back(e);
    }

    // 索引中有、目录中没有的条目 → 墓碑（仅当文件确实不存在；忽略/超限导致跳过的保持原样）
    for (auto& kv : state.entries) {
        auto& b = kv.second;
        if (b.deleted || b.conflicted) continue;
        if (live_paths.count(kv.first)) continue;
        if (ignore_match(kv.first, ignore_)) continue;
        if (pal::file_exists(pal::join_path(config_.folder, kv.first))) continue;
        b.deleted = true;
        b.deleted_at = now;
    }

    // 墓碑（本次新发现的 + 之前遗留的）都放入清单，保证对端未确认时删除可重试
    if (manifest) {
        std::set<std::string> emitted;
        for (const auto& e : live) emitted.insert(e.path);
        for (const auto& kv : state.entries) {
            const auto& b = kv.second;
            if (!b.deleted || live_paths.count(kv.first)) continue;
            if (ignore_match(kv.first, ignore_)) continue;
            if (!emitted.insert(kv.first).second) continue;
            SyncEntry t;
            t.path = kv.first;
            t.size = b.size;
            t.mtime = b.mtime;
            t.hash = b.hash;
            t.deleted = true;
            t.deleted_at = b.deleted_at;
            manifest->push_back(std::move(t));
        }
    }
    // 注意：不把当前活跃文件写回基线。基线必须是“上次成功同步的状态”，
    // 否则 compute_plan 无法区分“本端修改”与“对端删除/未变”，会误覆盖或误删本地文件。
    return state;
}

Result SyncService::delete_local(const std::string& rel, std::int64_t now) {
    std::string safe = rel;
    if (auto ok = sanitize_rel_path(safe); !ok) return Result::fail(ok.error);
    const std::string path = pal::join_path(config_.folder, safe);
    if (!pal::file_exists(path)) return Result::ok();
    if (config_.trash_enabled) return versions_.archive(safe, now);
    return pal::remove_file(path);
}

Result SyncService::send_file_to(ByteStream& stream, const SessionKeys& keys, std::uint64_t& seq,
                                 const SyncPlanItem& item, SyncStats& stats) {
    std::ifstream in(to_path(pal::join_path(config_.folder, item.path)), std::ios::binary);
    if (!in) return Result::fail("打开本端文件失败: " + item.path);

    const auto started = std::chrono::steady_clock::now();
    const double kbps = static_cast<double>(config_.rate_limit_kbps);

    {
        ByteWriter w;
        w.u8(kFileStart);
        w.str(item.target_path);
        w.u64(item.size);
        w.i64(item.mtime);
        w.bytes(item.hash);
        if (!seal_send(stream, keys, seq, w.buf())) return Result::fail("发送文件头失败");
    }

    std::uint64_t offset = 0;
    ByteBuf block(kChunkSize, 0);
    while (offset < item.size) {
        const size_t want =
            static_cast<size_t>(std::min<std::uint64_t>(kChunkSize, item.size - offset));
        in.read(reinterpret_cast<char*>(block.data()), static_cast<std::streamsize>(want));
        if (static_cast<size_t>(in.gcount()) != want) return Result::fail("读取本端文件失败");
        ByteWriter w;
        w.u8(kFileChunk);
        w.u64(offset);
        w.bytes(ByteBuf(block.begin(), block.begin() + static_cast<long>(want)));
        if (!seal_send(stream, keys, seq, w.buf())) return Result::fail("发送文件块失败");
        offset += want;
        stats.bytes += want;
        // 发送限速：按本文件已发送字节数控制平均速率（KiB/s）
        if (kbps > 0) {
            const double allowed = static_cast<double>(offset) / (kbps * 1024.0);
            const double elapsed =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            if (elapsed < allowed)
                std::this_thread::sleep_for(std::chrono::duration<double>(allowed - elapsed));
        }
    }
    {
        ByteWriter w;
        w.u8(kFileEnd);
        if (!seal_send(stream, keys, seq, w.buf())) return Result::fail("发送文件尾失败");
    }

    auto resp = seal_recv(stream, keys, seq, kMaxMetaFrame);
    if (!resp) return Result::fail("等待对端落盘确认失败");
    ByteReader r(*resp);
    const std::uint8_t type = r.u8();
    if (type == kApplyFail) {
        (void)r.str();
        const std::string reason = r.str();
        return Result::fail(reason.empty() ? "对端落盘失败" : reason);
    }
    if (type != kApplyOk) return Result::fail("对端应答类型错误");
    (void)r.str();
    return Result::ok();
}

Result SyncService::recv_file_from(ByteStream& stream, const SessionKeys& keys, std::uint64_t& seq,
                                   const SyncPlanItem& item, ByteBuf& received_hash,
                                   SyncStats& stats) {
    auto fail_with = [&](const std::string& reason) -> Result {
        ByteWriter w;
        w.u8(kApplyFail);
        w.str(item.target_path);
        w.str(reason);
        seal_send(stream, keys, seq, w.buf());
        return Result::fail(reason);
    };

    auto head = seal_recv(stream, keys, seq, kMaxMetaFrame);
    if (!head) return Result::fail("等待文件头失败");
    std::string target;
    std::uint64_t size = 0;
    ByteBuf digest;
    {
        ByteReader r(*head);
        if (r.u8() != kFileStart) return fail_with("文件头类型错误");
        target = r.str();
        size = r.u64();
        (void)r.i64();
        digest = r.bytes();
        if (!r.ok()) return fail_with("文件头格式错误");
    }
    if (target != item.target_path) return fail_with("目标路径与计划不一致");
    if (size != item.size) return fail_with("文件大小与计划不一致");
    if (digest.size() != crypto::kSha256Len) return fail_with("文件哈希格式错误");
    const std::uint64_t max_bytes =
        static_cast<std::uint64_t>(config_.max_file_size_mb) * 1024ull * 1024ull;
    if (size > max_bytes) return fail_with("文件超过单文件大小上限");

    std::string rel = target;
    if (auto ok = sanitize_rel_path(rel); !ok) return fail_with(ok.error);

    const std::string tmp_dir = pal::join_path(config_.folder, std::string(kInternalDir) + "/tmp");
    if (auto r = pal::ensure_dir(tmp_dir); !r) return fail_with("创建临时目录失败");
    const std::string tmp_path = pal::join_path(tmp_dir, random_suffix() + ".part");

    std::ofstream out(to_path(tmp_path), std::ios::binary | std::ios::trunc);
    if (!out) return fail_with("创建临时文件失败");

    std::uint64_t offset = 0;
    while (true) {
        auto msg = seal_recv(stream, keys, seq, kChunkSize + kChunkSlack);
        if (!msg) {
            out.close();
            pal::remove_file(tmp_path);
            return Result::fail("连接中断");
        }
        ByteReader r(*msg);
        const std::uint8_t type = r.u8();
        if (type == kFileChunk) {
            const std::uint64_t off = r.u64();
            const ByteBuf data = r.bytes();
            if (!r.ok() || off != offset || offset + data.size() > size) {
                out.close();
                pal::remove_file(tmp_path);
                return fail_with("文件块偏移或大小非法");
            }
            if (!data.empty()) {
                out.write(reinterpret_cast<const char*>(data.data()),
                          static_cast<std::streamsize>(data.size()));
                if (!out) {
                    out.close();
                    pal::remove_file(tmp_path);
                    return fail_with("写入临时文件失败");
                }
            }
            offset += data.size();
        } else if (type == kFileEnd) {
            break;
        } else {
            out.close();
            pal::remove_file(tmp_path);
            return fail_with("文件块类型错误");
        }
    }
    out.flush();
    out.close();
    if (offset != size) {
        pal::remove_file(tmp_path);
        return fail_with("文件数据不完整");
    }

    auto actual = hash_file(tmp_path);
    if (!actual || *actual != digest || *actual != item.hash) {
        pal::remove_file(tmp_path);
        return fail_with("整文件 SHA-256 校验失败");
    }

    const std::string final_path = pal::join_path(config_.folder, rel);
    const std::string parent = dirname_of(final_path);
    if (!parent.empty()) {
        if (auto r = pal::ensure_dir(parent); !r) {
            pal::remove_file(tmp_path);
            return fail_with("创建目标目录失败");
        }
    }
    if (pal::file_exists(final_path)) {
        if (config_.trash_enabled) {
            if (auto r = versions_.archive(rel, now_secs()); !r) {
                pal::remove_file(tmp_path);
                return fail_with("归档被覆盖文件失败: " + r.error);
            }
        } else if (auto r = pal::remove_file(final_path); !r) {
            pal::remove_file(tmp_path);
            return fail_with("移除旧文件失败: " + r.error);
        }
    }
    if (auto rn = pal::rename_file(tmp_path, final_path); !rn) {
        pal::remove_file(tmp_path);
        return fail_with("落盘失败: " + rn.error);
    }
    received_hash = digest;
    stats.bytes += size;

    ByteWriter ok;
    ok.u8(kApplyOk);
    ok.str(item.target_path);
    if (!seal_send(stream, keys, seq, ok.buf())) return Result::fail("发送落盘确认失败");
    return Result::ok();
}

Result SyncService::sync_with(const DeviceId& peer, int timeout_ms, SyncStats* out) {
    SyncStats stats;
    if (!config_.enabled) {
        publish_failed(peer, "", "同步未启用");
        return Result::fail("同步未启用（[sync].enabled=false）");
    }
    if (config_.folder.empty()) {
        publish_failed(peer, "", "未配置同步文件夹");
        return Result::fail("未配置 [sync].folder");
    }
    auto record = trust_.get(peer);
    if (!record) {
        publish_failed(peer, "", "目标设备不在信任库中");
        return Result::fail("目标设备不在信任库中");
    }
    if ((record->permissions & kPermSync) == 0) {
        publish_failed(peer, record->name, "对端未授予同步权限");
        return Result::fail("未授予同步权限");
    }
    if (record->last_ip.empty()) {
        publish_failed(peer, record->name, "尚未学习到对端 IP，请先运行 daemon");
        return Result::fail("尚未学习到对端 IP");
    }
    if (auto r = pal::ensure_dir(config_.folder); !r) {
        publish_failed(peer, record->name, "同步文件夹不可用: " + r.error);
        return Result::fail("同步文件夹不可用: " + r.error);
    }

    const SyncMode mode = parse_mode(config_.mode);

    auto dialed = dialer_.dial(record->last_ip,
                               record->sync_port > 0 ? record->sync_port : 45716, timeout_ms);
    if (!dialed) {
        publish_failed(peer, record->name, "连接对端失败: " + dialed.error);
        return Result::fail("连接对端失败: " + dialed.error);
    }
    std::unique_ptr<ByteStream> stream = std::move(*dialed);

    auto sts = sts_client(*stream, identity_, trust_, peer);
    if (!sts) {
        stream->close();
        publish_failed(peer, record->name, sts.error);
        return Result::fail(sts.error);
    }
    const SessionKeys keys = std::move(sts->keys);
    // 会话锁在握手之后获取：两端同时发起时只会串行化，不会互相等待握手应答
    std::lock_guard<std::mutex> session(session_mu_);
    std::uint64_t seq = 0;

    auto abort = [&](const std::string& why) -> Result {
        stream->close();
        publish_failed(peer, record->name, why);
        return Result::fail(why);
    };

    publish_started(peer, record->name);

    if (!seal_send(*stream, keys, seq, encode_hello(mode))) return abort("发送同步握手失败");
    const std::int64_t now = now_secs();
    const auto local = scan();
    std::vector<SyncEntry> manifest;
    SyncIndex local_index = prepare_local_state(local, now, &manifest);
    if (manifest.size() > kMaxEntries) return abort("本端文件数超过 20 万上限");
    if (!seal_send(*stream, keys, seq, encode_manifest(manifest))) return abort("发送清单失败");

    auto plan_msg = seal_recv(*stream, keys, seq, kMaxMetaFrame);
    if (!plan_msg) return abort("等待同步计划失败");
    SyncPlan plan;
    {
        ByteReader r(*plan_msg);
        const std::uint8_t type = r.u8();
        if (type == kFail) {
            const std::string reason = r.str();
            return abort(reason.empty() ? "对端拒绝同步" : reason);
        }
        if (type != kPlan) return abort("对端应答类型错误");
        auto parsed = decode_plan(r);
        if (!parsed) return abort(parsed.error);
        plan = std::move(*parsed);
    }

    // 1) 对端需要的文件：本端推送
    for (const auto& item : plan.want) {
        if (auto r = send_file_to(*stream, keys, seq, item, stats); !r) {
            stream->close();
            publish_failed(peer, record->name, r.error);
            return r;
        }
        local_index.set(item.path, SyncIndexEntry{item.size, item.mtime, item.hash, false, false, 0});
        // 冲突副本路径在两端都标记，避免下一次同步把副本再当新文件互传
        if (item.target_path != item.path)
            local_index.set(item.target_path,
                            SyncIndexEntry{item.size, item.mtime, item.hash, true, false, 0});
        ++stats.pushed;
    }

    // 2) 本端需要的文件：接收并落盘
    std::set<std::string> received;
    for (const auto& item : plan.send) {
        ByteBuf got;
        if (auto r = recv_file_from(*stream, keys, seq, item, got, stats); !r) {
            stream->close();
            publish_failed(peer, record->name, r.error);
            return r;
        }
        received.insert(item.target_path);
        const std::int64_t actual_mtime =
            pal::file_mtime(pal::join_path(config_.folder, item.target_path));
        local_index.set(item.target_path,
                        SyncIndexEntry{item.size, actual_mtime, got,
                                       item.target_path != item.path, false, 0});
        ++stats.pulled;
    }

    // 3) 删除传播：对端要求本端删除
    for (const auto& path : plan.want_delete) {
        if (auto r = delete_local(path, now); !r) {
            stream->close();
            publish_failed(peer, record->name, r.error);
            return r;
        }
        auto it = local_index.entries.find(path);
        SyncIndexEntry e = it != local_index.entries.end() ? it->second : SyncIndexEntry{};
        e.deleted = true;
        e.deleted_at = now;
        e.conflicted = false;
        local_index.set(path, std::move(e));
        ++stats.deleted;
    }
    // plan.send_delete 表示对端自行删除，本端无需动作（本地条目已是墓碑）

    auto done = seal_recv(*stream, keys, seq, 65536);
    if (!done) return abort("等待对端完成确认失败");
    {
        ByteReader r(*done);
        const std::uint8_t type = r.u8();
        if (type == kFail) {
            const std::string reason = r.str();
            return abort(reason.empty() ? "对端同步失败" : reason);
        }
        if (type != kDone) return abort("对端完成确认类型错误");
        (void)r.u64();
        (void)r.u64();
        (void)r.u64();
        (void)r.u64();
        (void)r.u64();
        if (!r.ok()) return abort("对端完成确认格式错误");
    }

    stats.conflicts = plan.conflicts.size();
    for (const auto& path : plan.conflicts) {
        auto it = local_index.entries.find(path);
        if (it == local_index.entries.end()) {
            local_index.set(path, SyncIndexEntry{});
        }
        local_index.entries[path].conflicted = true;
    }
    // 会话结束后把未变化/已确认的活跃文件写入基线（接收过的路径以落盘结果为准）
    for (const auto& e : local) {
        if (received.count(e.path)) continue;
        auto it = local_index.entries.find(e.path);
        if (it != local_index.entries.end() && it->second.conflicted) continue;
        local_index.set(e.path, SyncIndexEntry{e.size, e.mtime, e.hash, false, false, 0});
    }
    {
        std::lock_guard<std::mutex> lk(index_mu_);
        index_ = local_index;
        if (auto saved = index_.save(index_path()); !saved)
            publish_failed(peer, record->name, "保存同步索引失败: " + saved.error);
    }
    versions_.prune(now);

    stream->close();
    events_.publish(SyncCompleted{peer, record->name, stats});
    if (out) *out = stats;
    return Result::ok();
}

void SyncService::serve(std::unique_ptr<ByteStream> stream) {
    if (!stream) return;
    auto sts = sts_server(*stream, identity_, trust_);
    if (!sts) {
        stream->close();
        return;
    }
    const SessionKeys keys = std::move(sts->keys);
    std::uint64_t seq = 0;
    const auto record = trust_.get(sts->peer_id);
    const std::string peer_name = record ? record->name : std::string("unknown");

    auto reject = [&](const std::string& why) {
        seal_send(*stream, keys, seq, encode_fail(why));
        stream->close();
        publish_failed(sts->peer_id, peer_name, why);
    };

    if (!record || (record->permissions & kPermSync) == 0) {
        reject("对端未授予同步权限");
        return;
    }
    if (!config_.enabled || config_.folder.empty()) {
        reject("本端未启用文件夹同步");
        return;
    }
    const SyncMode mode = parse_mode(config_.mode);

    auto hello = seal_recv(*stream, keys, seq, 65536);
    if (!hello) {
        stream->close();
        return;
    }
    {
        ByteReader r(*hello);
        if (r.u8() != kHello) {
            reject("同步协议错误");
            return;
        }
        const std::string m = r.str();
        if (!r.ok() || m != sync_mode_name(mode)) {
            reject("双方同步模式不一致（M6 要求两端 [sync].mode 相同）");
            return;
        }
    }

    auto manifest_msg = seal_recv(*stream, keys, seq, kMaxMetaFrame);
    if (!manifest_msg) {
        stream->close();
        return;
    }
    std::vector<SyncEntry> remote;
    {
        ByteReader r(*manifest_msg);
        if (r.u8() != kManifest) {
            reject("同步清单类型错误");
            return;
        }
        auto parsed = decode_manifest(r);
        if (!parsed) {
            reject(parsed.error);
            return;
        }
        remote = std::move(*parsed);
    }

    std::lock_guard<std::mutex> session(session_mu_);
    publish_started(sts->peer_id, peer_name);

    const std::int64_t now = now_secs();
    const auto local = scan();
    std::vector<SyncEntry> local_state;
    SyncIndex baseline;
    SyncIndex local_index = prepare_local_state(local, now, &local_state, &baseline);
    if (local_state.size() > kMaxEntries) {
        reject("本端文件数超过 20 万上限");
        return;
    }

    SyncPlan plan =
        compute_plan(local_state, remote, baseline, mode, identity_.name(), peer_name, now,
                     nullptr, config_.default_conflict_policy);

    SyncStats stats;
    if (!seal_send(*stream, keys, seq, encode_plan(plan))) {
        stream->close();
        publish_failed(sts->peer_id, peer_name, "发送同步计划失败");
        return;
    }

    auto save_index = [&] {
        std::lock_guard<std::mutex> lk(index_mu_);
        index_ = local_index;
        if (auto saved = index_.save(index_path()); !saved)
            publish_failed(sts->peer_id, peer_name, "保存同步索引失败: " + saved.error);
    };
    auto abort = [&](const std::string& why) {
        save_index();
        stream->close();
        publish_failed(sts->peer_id, peer_name, why);
    };

    // 1) 本端需要的文件：接收对端推送
    std::set<std::string> received;
    for (const auto& item : plan.want) {
        ByteBuf got;
        if (auto r = recv_file_from(*stream, keys, seq, item, got, stats); !r) {
            abort(r.error);
            return;
        }
        received.insert(item.target_path);
        const std::int64_t actual_mtime =
            pal::file_mtime(pal::join_path(config_.folder, item.target_path));
        local_index.set(item.target_path,
                        SyncIndexEntry{item.size, actual_mtime, got,
                                       item.target_path != item.path, false, 0});
        ++stats.pulled;
    }

    // 2) 对端需要的文件：本端推送
    for (const auto& item : plan.send) {
        if (auto r = send_file_to(*stream, keys, seq, item, stats); !r) {
            abort(r.error);
            return;
        }
        local_index.set(item.path, SyncIndexEntry{item.size, item.mtime, item.hash, false, false, 0});
        if (item.target_path != item.path)
            local_index.set(item.target_path,
                            SyncIndexEntry{item.size, item.mtime, item.hash, true, false, 0});
        ++stats.pushed;
    }

    // 3) 删除传播：本端自行删除（发起方也会删除，条目已是墓碑）
    for (const auto& path : plan.send_delete) {
        if (auto r = delete_local(path, now); !r) {
            abort(r.error);
            return;
        }
        auto it = local_index.entries.find(path);
        SyncIndexEntry e = it != local_index.entries.end() ? it->second : SyncIndexEntry{};
        e.deleted = true;
        e.deleted_at = now;
        e.conflicted = false;
        local_index.set(path, std::move(e));
        ++stats.deleted;
    }
    // plan.want_delete 表示发起方会删除，本端条目已是墓碑

    stats.conflicts = plan.conflicts.size();
    for (const auto& path : plan.conflicts) {
        auto it = local_index.entries.find(path);
        if (it == local_index.entries.end()) local_index.set(path, SyncIndexEntry{});
        local_index.entries[path].conflicted = true;
    }
    for (const auto& e : local) {
        if (received.count(e.path)) continue;
        auto it = local_index.entries.find(e.path);
        if (it != local_index.entries.end() && it->second.conflicted) continue;
        local_index.set(e.path, SyncIndexEntry{e.size, e.mtime, e.hash, false, false, 0});
    }

    {
        ByteWriter w;
        w.u8(kDone);
        w.u64(stats.pushed);
        w.u64(stats.pulled);
        w.u64(stats.conflicts);
        w.u64(stats.bytes);
        w.u64(stats.deleted);
        if (!seal_send(*stream, keys, seq, w.buf())) {
            abort("发送完成确认失败");
            return;
        }
    }

    save_index();
    versions_.prune(now);
    stream->close();
    events_.publish(SyncCompleted{sts->peer_id, peer_name, stats});
}

// ---------------- M7：冲突与版本库 ----------------

namespace {

// 以当前磁盘内容刷新基线条目（用于冲突解决后重建基线）
Result refresh_entry(const std::string& full_path, SyncIndexEntry& e) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(to_path(full_path), ec);
    if (ec) return Result::fail("读取文件信息失败: " + full_path);
    auto digest = hash_file(full_path);
    if (!digest) return Result::fail(digest.error);
    e.size = static_cast<std::uint64_t>(size);
    e.mtime = pal::file_mtime(full_path);
    e.hash = std::move(*digest);
    e.conflicted = false;
    e.deleted = false;
    e.deleted_at = 0;
    return Result::ok();
}

} // namespace

std::vector<SyncConflictInfo> SyncService::conflicts() const {
    std::map<std::string, std::vector<std::string>> copies;
    std::set<std::string> originals;
    {
        std::lock_guard<std::mutex> lk(index_mu_);
        for (const auto& kv : index_.entries) {
            if (!kv.second.conflicted) continue;
            const std::string orig = conflict_original_path(kv.first);
            if (orig != kv.first) {
                copies[orig].push_back(kv.first);
                originals.insert(orig);
            } else {
                originals.insert(kv.first);
            }
        }
    }

    std::vector<SyncConflictInfo> out;
    for (const auto& path : originals) {
        if (!pal::file_exists(pal::join_path(config_.folder, path))) continue;
        SyncConflictInfo info;
        info.path = path;
        const auto it = copies.find(path);
        if (it != copies.end()) {
            for (const auto& c : it->second)
                if (pal::file_exists(pal::join_path(config_.folder, c)))
                    info.copies.push_back(c);
        }
        if (info.copies.empty()) continue; // 没有副本可对比，留给人工处理
        out.push_back(std::move(info));
    }
    return out;
}

Result SyncService::resolve_conflict(const std::string& path, const std::string& action) {
    if (action != "keep_local" && action != "keep_remote" && action != "keep_both")
        return Result::fail("未知冲突处理动作（keep_local|keep_remote|keep_both）");
    std::string rel = path;
    if (auto ok = sanitize_rel_path(rel); !ok) return Result::fail(ok.error);
    const std::int64_t now = now_secs();

    std::vector<std::string> copies;
    {
        std::lock_guard<std::mutex> lk(index_mu_);
        for (const auto& kv : index_.entries) {
            if (!kv.second.conflicted) continue;
            if (conflict_original_path(kv.first) == rel && kv.first != rel)
                copies.push_back(kv.first);
        }
    }

    const std::string original_path = pal::join_path(config_.folder, rel);
    const bool original_exists = pal::file_exists(original_path);

    if (action == "keep_local") {
        if (!original_exists) return Result::fail("原文件已不存在，无法保留本端版本");
        for (const auto& c : copies) {
            const std::string full = pal::join_path(config_.folder, c);
            if (pal::file_exists(full)) {
                if (auto r = delete_local(c, now); !r) return Result::fail(r.error);
            }
        }
        SyncIndexEntry e;
        if (auto r = refresh_entry(original_path, e); !r) return Result::fail(r.error);
        std::lock_guard<std::mutex> lk(index_mu_);
        for (const auto& c : copies) index_.entries.erase(c);
        index_.set(rel, std::move(e));
        if (auto saved = index_.save(index_path()); !saved)
            return Result::fail("保存同步索引失败: " + saved.error);
        return Result::ok();
    }

    if (action == "keep_both") {
        if (!original_exists) return Result::fail("原文件已不存在，无法保留双方版本");
        std::lock_guard<std::mutex> lk(index_mu_);
        SyncIndexEntry e;
        if (auto r = refresh_entry(original_path, e); !r) return Result::fail(r.error);
        index_.set(rel, std::move(e));
        for (const auto& c : copies) {
            const std::string full = pal::join_path(config_.folder, c);
            if (!pal::file_exists(full)) {
                index_.entries.erase(c);
                continue;
            }
            SyncIndexEntry ce;
            if (auto r = refresh_entry(full, ce); !r) return Result::fail(r.error);
            index_.set(c, std::move(ce));
        }
        if (auto saved = index_.save(index_path()); !saved)
            return Result::fail("保存同步索引失败: " + saved.error);
        return Result::ok();
    }

    // keep_remote：用最新的冲突副本覆盖原路径，原文件先归档
    std::string chosen;
    std::int64_t chosen_mtime = 0;
    for (const auto& c : copies) {
        const std::string full = pal::join_path(config_.folder, c);
        if (!pal::file_exists(full)) continue;
        const std::int64_t mt = pal::file_mtime(full);
        if (chosen.empty() || mt > chosen_mtime) {
            chosen = c;
            chosen_mtime = mt;
        }
    }
    if (chosen.empty()) return Result::fail("没有可用的冲突副本: " + rel);

    SyncIndexEntry e;
    if (auto r = refresh_entry(pal::join_path(config_.folder, chosen), e); !r)
        return Result::fail(r.error);

    if (original_exists) {
        if (auto r = delete_local(rel, now); !r) return Result::fail(r.error);
    }
    const std::string chosen_full = pal::join_path(config_.folder, chosen);
    if (auto r = pal::rename_file(chosen_full, original_path); !r)
        return Result::fail("用冲突副本覆盖原文件失败: " + r.error);

    // 其余副本归档后清理
    for (const auto& c : copies) {
        if (c == chosen) continue;
        const std::string full = pal::join_path(config_.folder, c);
        if (pal::file_exists(full)) {
            if (auto r = delete_local(c, now); !r) return Result::fail(r.error);
        }
    }

    std::lock_guard<std::mutex> lk(index_mu_);
    for (const auto& c : copies) index_.entries.erase(c);
    index_.set(rel, std::move(e));
    if (auto saved = index_.save(index_path()); !saved)
        return Result::fail("保存同步索引失败: " + saved.error);
    return Result::ok();
}

std::vector<VersionInfo> SyncService::history(const std::string& path) const {
    if (path.empty()) return versions_.list();
    std::string rel = path;
    if (auto ok = sanitize_rel_path(rel); !ok) return {};
    return versions_.list(rel);
}

Result SyncService::restore_version(const std::string& path, const std::string& version_id) {
    std::string rel = path;
    if (auto ok = sanitize_rel_path(rel); !ok) return Result::fail(ok.error);
    // 恢复视为本端修改：不更新基线，让下一次同步把恢复后的内容传播出去
    return versions_.restore(rel, version_id, now_secs());
}

void SyncService::start_auto() {
    if (auto_thread_.joinable()) return;
    if (!config_.enabled || config_.folder.empty()) return;
    (void)pal::ensure_dir(config_.folder);
    auto_stop_ = false;
    dirty_ = false;
    watcher_ = make_file_watcher();
    if (watcher_) {
        watcher_->start(config_.folder, [this] { notify_fs_change(); });
    }
    auto_thread_ = std::thread([this] { auto_loop(); });
}

void SyncService::stop_auto() {
    auto_stop_ = true;
    if (watcher_) {
        watcher_->stop();
        watcher_.reset();
    }
    if (auto_thread_.joinable()) auto_thread_.join();
}

void SyncService::notify_fs_change() { dirty_ = true; }

void SyncService::auto_loop() {
    std::int64_t last_scan = now_secs();
    bool change_pending = false;
    std::int64_t change_at = 0;
    auto sync_all = [this] {
        for (const auto& rec : trust_.all()) {
            if ((rec.permissions & kPermSync) == 0 || rec.last_ip.empty()) continue;
            if (auto_stop_.load()) break;
            SyncStats ignored;
            (void)sync_with(rec.device_id, 5000, &ignored);
        }
    };

    while (!auto_stop_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (auto_stop_.load()) break;
        const std::int64_t now = now_secs();
        if (dirty_.exchange(false)) {
            change_pending = true;
            change_at = now;
        }
        // 事件防抖：最后一次变化后静默 1 秒再同步，避免编辑过程中的半成品被传输
        if (change_pending) {
            if (now - change_at < 1) continue;
            change_pending = false;
            last_scan = now;
            sync_all();
            continue;
        }
        if (now - last_scan >= config_.rescan_interval_sec) {
            last_scan = now;
            sync_all();
        }
    }
}

} // namespace lb
