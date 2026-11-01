#include "core/trust/trust_store.h"

#include "core/common/bytes.h"
#include "core/platform/platform.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <sstream>

namespace lb {
namespace {

std::string iso_utc(std::int64_t ts) {
    if (ts <= 0) ts = now_secs();
    const std::time_t t = static_cast<std::time_t>(ts);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ", tm.tm_year + 1900,
                  tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

std::string changed_bits(std::uint32_t before, std::uint32_t after) {
    std::ostringstream os;
    bool first = true;
    for (const auto& info : permission_catalog()) {
        const bool was = (before & info.flag) != 0;
        const bool now = (after & info.flag) != 0;
        if (was == now) continue;
        if (!first) os << ",";
        first = false;
        os << info.key << (now ? "=on" : "=off");
    }
    return first ? std::string("-") : os.str();
}

} // namespace

const std::vector<PermissionInfo>& permission_catalog() {
    static const std::vector<PermissionInfo> catalog = {
        {kPermFileTransfer, "file_transfer", "文件接收", "允许对端向本机发送文件", true},
        {kPermClipboard, "clipboard", "剪贴板同步", "允许对端读写本机剪贴板内容", true},
        {kPermRemoteFs, "remote_fs", "远程文件浏览", "允许对端浏览/读取共享目录（预留）", false},
        {kPermScreenControl, "screen_control", "屏幕控制", "允许对端注入触摸与系统按键", false},
        {kPermScreenShare, "screen_share", "屏幕共享", "允许对端查看本机屏幕画面", true},
        {kPermSync, "sync", "文件夹同步", "允许与本机同步指定文件夹", true},
        {kPermNotifyMirror, "notify_mirror", "通知镜像", "允许对端把通知同步显示到本机", true},
        {kPermNotifyAction, "notify_action", "通知动作", "允许对端触发本机预设通知动作（敏感）", false},
    };
    return catalog;
}

const char* permission_name(std::uint32_t flag) {
    for (const auto& info : permission_catalog())
        if (info.flag == flag) return info.name;
    return "unknown";
}

std::string permission_mask_text(std::uint32_t mask) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%08x", static_cast<unsigned>(mask));
    return buf;
}

ByteBuf TrustRecord::to_bytes() const {
    ByteWriter w;
    w.str(device_id);
    w.str(name);
    w.u8(static_cast<std::uint8_t>(platform));
    w.bytes(public_key);
    w.str(last_ip);
    w.str(mac);
    w.str(ssh_public_key);
    w.u32(static_cast<std::uint32_t>(heartbeat_port));
    w.u32(static_cast<std::uint32_t>(ssh_port));
    w.u32(static_cast<std::uint32_t>(transfer_port));
    w.u32(static_cast<std::uint32_t>(clipboard_port));
    w.u32(static_cast<std::uint32_t>(screen_port));
    w.u32(static_cast<std::uint32_t>(sync_port));
    w.u32(static_cast<std::uint32_t>(notify_port));
    w.i64(paired_at);
    w.u32(permissions);
    return w.buf();
}

Expected<TrustRecord> TrustRecord::from_bytes(ByteReader& reader, bool legacy) {
    TrustRecord r;
    r.device_id = reader.str();
    r.name = reader.str();
    const auto p = reader.u8();
    r.platform = (p == 1) ? Platform::Linux : (p == 3) ? Platform::Android : Platform::Windows;
    r.public_key = reader.bytes();
    r.last_ip = reader.str();
    r.mac = reader.str();
    r.ssh_public_key = reader.str();
    r.heartbeat_port = static_cast<int>(reader.u32());
    r.ssh_port = static_cast<int>(reader.u32());
    r.transfer_port = static_cast<int>(reader.u32());
    r.clipboard_port = static_cast<int>(reader.u32());
    r.screen_port = static_cast<int>(reader.u32());
    r.sync_port = static_cast<int>(reader.u32());
    if (legacy) {
        r.notify_port = 45717;
    } else {
        r.notify_port = static_cast<int>(reader.u32());
        if (r.notify_port <= 0 || r.notify_port >= 65536) r.notify_port = 45717;
    }
    r.paired_at = reader.i64();
    r.permissions = reader.u32();
    if (!reader.ok() || r.device_id.empty() || r.public_key.size() != 65)
        return Expected<TrustRecord>::fail("受信任记录格式非法");
    return Expected<TrustRecord>::success(std::move(r));
}

TrustStore::TrustStore(const std::string& dir)
    : dir_(dir),
      path_(pal::join_path(dir, "trusted_devices.bin")),
      audit_path_(pal::join_path(dir, "permissions.log")) {}

Result TrustStore::load() {
    if (!pal::file_exists(path_)) {
        records_.clear();
        return Result::ok();
    }
    auto sealed = pal::read_file(path_);
    if (!sealed) return Result::fail("读取信任库失败: " + sealed.error);
    auto plain = pal::unprotect_secret(*sealed);
    if (!plain) return Result::fail("解密信任库失败（可能换了用户或机器）");

    ByteReader reader(*plain);
    const ByteBuf magic = reader.raw(4);
    const std::string version(magic.begin(), magic.end());
    const bool legacy = version == "LBT5";
    if (!reader.ok() || (!legacy && version != "LBT6"))
        return Result::fail("信任库版本不兼容，请重新配对");
    const std::uint32_t count = reader.u32();
    if (!reader.ok()) return Result::fail("信任库格式损坏");
    if (count > 4096) return Result::fail("信任库记录数异常");

    std::vector<TrustRecord> loaded;
    loaded.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        auto rec = TrustRecord::from_bytes(reader, legacy);
        if (!rec) return Result::fail(rec.error);
        loaded.push_back(std::move(*rec));
    }
    records_ = std::move(loaded);
    return Result::ok();
}

Result TrustStore::save() const {
    ByteWriter w;
    w.raw("LBT6", 4);
    w.u32(static_cast<std::uint32_t>(records_.size()));
    for (const auto& r : records_) {
        const ByteBuf b = r.to_bytes();
        w.raw(b.data(), b.size());
    }
    const ByteBuf sealed = pal::protect_secret(w.buf());
    if (sealed.empty()) return Result::fail("加密信任库失败");
    return pal::write_file_atomic(path_, sealed);
}

std::vector<TrustRecord> TrustStore::all() const { return records_; }

std::optional<TrustRecord> TrustStore::get(const DeviceId& id) const {
    for (const auto& r : records_)
        if (r.device_id == id) return r;
    return std::nullopt;
}

bool TrustStore::has(const DeviceId& id) const { return get(id).has_value(); }

std::size_t TrustStore::size() const { return records_.size(); }

Result TrustStore::upsert(const TrustRecord& record) {
    auto it = std::find_if(records_.begin(), records_.end(),
                           [&](const TrustRecord& r) { return r.device_id == record.device_id; });
    if (it != records_.end()) *it = record;
    else records_.push_back(record);
    return save();
}

Result TrustStore::remove(const DeviceId& id) {
    const auto before = records_.size();
    records_.erase(std::remove_if(records_.begin(), records_.end(),
                                  [&](const TrustRecord& r) { return r.device_id == id; }),
                   records_.end());
    if (records_.size() == before) return Result::fail("未找到该设备");
    return save();
}

Result TrustStore::update_last_ip(const DeviceId& id, const std::string& ip) {
    for (auto& r : records_) {
        if (r.device_id == id) {
            r.last_ip = ip;
            return save();
        }
    }
    return Result::fail("未找到该设备");
}

Result TrustStore::set_permissions(const DeviceId& id, std::uint32_t permissions) {
    auto it = std::find_if(records_.begin(), records_.end(),
                           [&](const TrustRecord& r) { return r.device_id == id; });
    if (it == records_.end()) return Result::fail("未找到该设备");

    const std::uint32_t before = it->permissions;
    if (before == permissions) return Result::ok();
    it->permissions = permissions;
    if (auto r = save(); !r) {
        it->permissions = before; // 落盘失败则回滚内存，避免内存与磁盘不一致
        return Result::fail("保存权限失败: " + r.error);
    }

    // 审计日志：不落任何密钥材料，只记录设备与掩码变化
    std::ostringstream line;
    line << iso_utc(0) << " | " << it->device_id << " | " << it->name << " | "
         << permission_mask_text(before) << " -> " << permission_mask_text(permissions) << " | "
         << changed_bits(before, permissions);

    std::ostringstream all;
    if (pal::file_exists(audit_path_)) {
        if (auto old = pal::read_file(audit_path_); old)
            all << std::string(old->begin(), old->end());
        if (!all.str().empty() && all.str().back() != '\n') all << "\n";
    }
    all << line.str() << "\n";

    // 只保留最近 500 行，避免无限增长
    std::vector<std::string> lines;
    std::istringstream in(all.str());
    std::string row;
    while (std::getline(in, row)) lines.push_back(row);
    if (lines.size() > 500) lines.erase(lines.begin(), lines.begin() + (lines.size() - 500));
    std::ostringstream trimmed;
    for (const auto& row : lines) trimmed << row << "\n";
    const std::string text = trimmed.str();
    pal::write_file_atomic(audit_path_, ByteBuf(text.begin(), text.end()));
    return Result::ok();
}

std::vector<std::string> TrustStore::permission_audit_log(std::size_t max_lines) const {
    std::vector<std::string> lines;
    if (!pal::file_exists(audit_path_)) return lines;
    auto data = pal::read_file(audit_path_);
    if (!data) return lines;
    const std::string text(data->begin(), data->end());
    std::istringstream in(text);
    std::string row;
    while (std::getline(in, row)) {
        if (!row.empty()) lines.push_back(row);
    }
    if (max_lines > 0 && lines.size() > max_lines)
        lines.erase(lines.begin(), lines.begin() + (lines.size() - max_lines));
    return lines;
}

} // namespace lb
