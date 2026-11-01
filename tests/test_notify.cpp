#include "core/common/bytes.h"
#include "core/config/config.h"
#include "core/notify/notify.h"
#include "core/trust/trust_store.h"

#include "test.h"

#include <map>
#include <string>

namespace {

lb::TrustRecord make_record() {
    lb::TrustRecord r;
    r.device_id = "0123456789abcdef";
    r.name = "测试设备";
    r.platform = lb::Platform::Android;
    r.public_key = lb::ByteBuf(65, 0x11);
    r.last_ip = "127.0.0.1";
    r.notify_port = 46000;
    r.permissions = lb::kPermNotifyMirror;
    return r;
}

} // namespace

TEST(notify_dedupe_window) {
    std::map<std::string, std::int64_t> seen;
    CHECK(lb::NotifyService::dedupe_accept(seen, "k", 1000, 20));
    CHECK(!lb::NotifyService::dedupe_accept(seen, "k", 1005, 20)); // 窗口内重复
    CHECK(lb::NotifyService::dedupe_accept(seen, "k", 1021, 20));  // 窗口外重新接受
    CHECK(lb::NotifyService::dedupe_accept(seen, "other", 1005, 20));

    std::map<std::string, std::int64_t> small;
    CHECK(lb::NotifyService::dedupe_accept(small, "k", 1000, 0)); // 窗口最小按 1 秒
    CHECK(!lb::NotifyService::dedupe_accept(small, "k", 1000, 0));
}

TEST(notify_clamp_text) {
    CHECK_STR_EQ(lb::NotifyService::clamp_text("hello", 16), "hello");
    CHECK_STR_EQ(lb::NotifyService::clamp_text("abcdefghij", 4), "abcd");
    CHECK_STR_EQ(lb::NotifyService::clamp_text("abc", 0), "");
}

TEST(notify_permission_catalog) {
    std::uint32_t union_default = 0;
    std::uint32_t union_all = 0;
    for (const auto& info : lb::permission_catalog()) {
        CHECK(info.flag != 0);
        CHECK((union_all & info.flag) == 0); // 每个权限必须是独立位
        union_all |= info.flag;
        if (info.default_granted) union_default |= info.flag;
    }
    CHECK(union_default == lb::kPermDefault);
    CHECK((lb::kPermDefault & lb::kPermNotifyMirror) != 0);
    CHECK((lb::kPermDefault & lb::kPermNotifyAction) == 0); // 动作权限默认关闭
    CHECK_STR_EQ(lb::permission_name(lb::kPermNotifyAction), "通知动作");
    CHECK(lb::permission_name(1u << 20) == std::string("unknown"));
    CHECK_STR_EQ(lb::permission_mask_text(0x7f), "0x0000007f");
}

TEST(notify_trust_record_roundtrip) {
    const lb::TrustRecord original = make_record();
    const lb::ByteBuf encoded = original.to_bytes();
    lb::ByteReader reader(encoded);
    auto parsed = lb::TrustRecord::from_bytes(reader);
    CHECK(parsed.ok());
    if (parsed) {
        CHECK_STR_EQ(parsed->device_id, original.device_id);
        CHECK(parsed->notify_port == 46000);
        CHECK(parsed->permissions == lb::kPermNotifyMirror);
        CHECK(parsed->public_key.size() == 65);
    }

    // 缺字段的截断数据必须被拒绝
    lb::ByteBuf bytes = original.to_bytes();
    bytes.resize(bytes.size() - 8);
    lb::ByteReader short_reader(bytes);
    CHECK(!lb::TrustRecord::from_bytes(short_reader).ok());
}

TEST(notify_config_validation) {
    lb::AppConfig cfg;
    CHECK(lb::validate_config(cfg));

    cfg.notify.action_mode = "tap";
    cfg.notify.action_x = -1;
    CHECK(!lb::validate_config(cfg));

    cfg.notify.action_x = 100;
    cfg.notify.action_y = 200;
    CHECK(lb::validate_config(cfg));

    cfg.notify.action_mode = "bogus";
    CHECK(!lb::validate_config(cfg));

    cfg.notify.action_mode = "open_app";
    cfg.network.notify_port = cfg.network.sync_port;
    CHECK(!lb::validate_config(cfg));
}
