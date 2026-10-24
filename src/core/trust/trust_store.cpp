#include "core/trust/trust_store.h"

#include "core/common/bytes.h"
#include "core/platform/platform.h"

#include <algorithm>

namespace lb {

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
    w.i64(paired_at);
    w.u32(permissions);
    return w.buf();
}

Expected<TrustRecord> TrustRecord::from_bytes(ByteReader& reader) {
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
    r.paired_at = reader.i64();
    r.permissions = reader.u32();
    if (!reader.ok() || r.device_id.empty() || r.public_key.size() != 65)
        return Expected<TrustRecord>::fail("受信任记录格式非法");
    return Expected<TrustRecord>::success(std::move(r));
}

TrustStore::TrustStore(const std::string& dir) : path_(pal::join_path(dir, "trusted_devices.bin")) {}

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
    if (!reader.ok() || std::string(magic.begin(), magic.end()) != "LBT5")
        return Result::fail("信任库版本不兼容，请重新配对");
    const std::uint32_t count = reader.u32();
    if (!reader.ok()) return Result::fail("信任库格式损坏");
    if (count > 4096) return Result::fail("信任库记录数异常");

    std::vector<TrustRecord> loaded;
    loaded.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        auto rec = TrustRecord::from_bytes(reader);
        if (!rec) return Result::fail(rec.error);
        loaded.push_back(std::move(*rec));
    }
    records_ = std::move(loaded);
    return Result::ok();
}

Result TrustStore::save() const {
    ByteWriter w;
    w.raw("LBT5", 4);
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

} // namespace lb
