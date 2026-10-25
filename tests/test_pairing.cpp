#include "test.h"

#include "core/common/event_bus.h"
#include "core/crypto/crypto.h"
#include "core/heartbeat/heartbeat.h"
#include "core/identity/identity.h"
#include "core/pairing/pairing.h"
#include "core/trust/trust_store.h"
#include "net/tcp.h"

#include <filesystem>

using namespace lb;

namespace {

std::string make_temp_dir(const char* tag) {
    const auto base = std::filesystem::temp_directory_path() /
                      (std::string("lb_test_") + tag + "_" + std::to_string(now_secs()));
    std::filesystem::remove_all(base);
    std::filesystem::create_directories(base);
    return base.string();
}

} // namespace

TEST(peerinfo_serialization_roundtrip) {
    PeerInfo info;
    info.device_id = "aabbccddeeff0011";
    info.name = "客厅电脑";
    info.platform = "windows";
    info.ip = "192.168.1.10";
    info.mac = "AA:BB:CC:DD:EE:FF";
    info.ssh_public_key = "ecdsa-sha2-nistp256";
    info.public_key = crypto::random_bytes(65);
    info.public_key[0] = 0x04;
    info.heartbeat_port = 45711;
    info.ssh_port = 45712;
    info.transfer_port = 45713;
    info.clipboard_port = 45714;
    info.screen_port = 45715;
    info.sync_port = 45716;

    auto parsed = PeerInfo::from_bytes(info.to_bytes());
    CHECK(parsed.has_value());
    if (!parsed) return;
    CHECK_STR_EQ(parsed->device_id, info.device_id);
    CHECK_STR_EQ(parsed->name, info.name);
    CHECK_STR_EQ(parsed->platform, info.platform);
    CHECK_STR_EQ(parsed->ip, info.ip);
    CHECK_STR_EQ(parsed->mac, info.mac);
    CHECK(parsed->public_key == info.public_key);
    CHECK(parsed->heartbeat_port == 45711);
    CHECK(parsed->ssh_port == 45712);
    CHECK(parsed->transfer_port == 45713);
    CHECK(parsed->clipboard_port == 45714);
    CHECK(parsed->screen_port == 45715);
    CHECK(parsed->sync_port == 45716);
}

TEST(peerinfo_rejects_short_public_key) {
    PeerInfo info;
    info.device_id = "id";
    info.public_key = ByteBuf(10, 0x04);
    CHECK(!PeerInfo::from_bytes(info.to_bytes()).has_value());
}

TEST(fingerprint_is_symmetric_and_stable) {
    const ByteBuf pub_a = crypto::random_bytes(65);
    const ByteBuf pub_b = crypto::random_bytes(65);
    const std::string fp1 = PairingService::compute_fingerprint("id-a", pub_a, "id-b", pub_b);
    const std::string fp2 = PairingService::compute_fingerprint("id-b", pub_b, "id-a", pub_a);
    CHECK_STR_EQ(fp1, fp2);
    CHECK(fp1.size() == 14); // XXXX-XXXX-XXXX
    CHECK(fp1[4] == '-');
    CHECK(fp1[9] == '-');
}

TEST(pairing_code_format_and_expiry) {
    const std::string dir = make_temp_dir("pairing");
    auto identity = Identity::load_or_create(dir, "test-device");
    CHECK(identity.has_value());
    if (!identity) return;

    TrustStore trust(dir);
    EventBus events;
    net::TcpDialer dialer;
    PairingService pairing(*identity, trust, events, dialer);

    const PairCode code = pairing.generate_code(300, 3);
    CHECK(code.value.size() == 6);
    for (char c : code.value) CHECK(c >= '0' && c <= '9');
    CHECK(code.attempts_left == 3);
    CHECK(code.expires_at > now_secs());

    const auto current = pairing.current_code();
    CHECK(current.has_value());
    if (current) CHECK_STR_EQ(current->value, code.value);

    CHECK(pairing.state() == PairState::CodeShown);
    pairing.invalidate_code();
    CHECK(!pairing.current_code().has_value());

    std::filesystem::remove_all(dir);
}

TEST(identity_is_stable_across_reload) {
    const std::string dir = make_temp_dir("identity");
    auto first = Identity::load_or_create(dir, "dev-a");
    CHECK(first.has_value());
    if (!first) return;
    const DeviceId id = first->device_id();
    const std::string fp = first->fingerprint();
    CHECK(!id.empty());
    CHECK(fp.size() == 14);

    auto second = Identity::load_or_create(dir, "dev-a");
    CHECK(second.has_value());
    if (!second) return;
    CHECK_STR_EQ(second->device_id(), id);
    CHECK_STR_EQ(second->fingerprint(), fp);
    CHECK(second->public_key() == first->public_key());

    std::filesystem::remove_all(dir);
}

TEST(trust_store_encrypted_roundtrip) {
    const std::string dir = make_temp_dir("trust");
    {
        TrustStore store(dir);
        CHECK(store.load().success);
        TrustRecord rec;
        rec.device_id = "deadbeef00000001";
        rec.name = "手机";
        rec.platform = Platform::Android;
        rec.public_key = ByteBuf(65, 0x04);
        rec.last_ip = "10.0.0.8";
        rec.mac = "11:22:33:44:55:66";
        rec.sync_port = 45716;
        rec.paired_at = now_secs();
        CHECK(store.upsert(rec).success);
    }
    {
        TrustStore store(dir);
        CHECK(store.load().success);
        CHECK(store.size() == 1);
        const auto rec = store.get("deadbeef00000001");
        CHECK(rec.has_value());
        if (rec) {
            CHECK_STR_EQ(rec->name, "手机");
            CHECK_STR_EQ(rec->last_ip, "10.0.0.8");
            CHECK(rec->platform == Platform::Android);
            CHECK(rec->sync_port == 45716);
        }
    }
    // 落盘内容必须是密文，不应包含明文设备名
    const std::string raw = dir + "/trusted_devices.bin";
    std::FILE* f = std::fopen(raw.c_str(), "rb");
    CHECK(f != nullptr);
    if (f) {
        std::string content;
        char buf[512];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) content.append(buf, n);
        std::fclose(f);
        CHECK(content.find("手机") == std::string::npos);
        CHECK(content.find("10.0.0.8") == std::string::npos);
    }
    std::filesystem::remove_all(dir);
}
