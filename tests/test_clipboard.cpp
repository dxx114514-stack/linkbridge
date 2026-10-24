#include "core/clipboard/clipboard.h"
#include "core/config/config.h"

#include "test.h"

#include <string>

TEST(clipboard_policy_mime_and_size) {
    lb::ClipboardConfig cfg;
    cfg.max_bytes = 1024;
    const lb::ByteBuf digest = {0xAA, 0xBB};

    CHECK(lb::ClipboardService::evaluate(cfg, "text/plain", 100, digest, {}, {}, false) ==
          lb::ClipboardDecision::Accept);
    CHECK(lb::ClipboardService::evaluate(cfg, "image/dib", 1024, digest, {}, {}, false) ==
          lb::ClipboardDecision::Accept);
    CHECK(lb::ClipboardService::evaluate(cfg, "text/plain", 1025, digest, {}, {}, false) ==
          lb::ClipboardDecision::TooLarge);
    CHECK(lb::ClipboardService::evaluate(cfg, "image/png", 100, digest, {}, {}, false) ==
          lb::ClipboardDecision::UnsupportedType);
    CHECK(lb::ClipboardService::evaluate(cfg, "text/plain", 0, digest, {}, {}, false) ==
          lb::ClipboardDecision::UnsupportedType);
}

TEST(clipboard_policy_dedupe_and_loop) {
    lb::ClipboardConfig cfg;
    cfg.max_bytes = 4096;
    const lb::ByteBuf digest = {1, 2, 3, 4};
    const lb::ByteBuf other = {9, 9};

    CHECK(lb::ClipboardService::evaluate(cfg, "text/plain", 10, digest, digest, {}, false) ==
          lb::ClipboardDecision::Duplicate);
    CHECK(lb::ClipboardService::evaluate(cfg, "text/plain", 10, digest, other, digest, true) ==
          lb::ClipboardDecision::LoopEcho);
    // 回环窗口之外不再抑制
    CHECK(lb::ClipboardService::evaluate(cfg, "text/plain", 10, digest, other, digest, false) ==
          lb::ClipboardDecision::Accept);
}

TEST(clipboard_decision_names) {
    CHECK_STR_EQ(lb::clipboard_decision_name(lb::ClipboardDecision::Accept), "accept");
    CHECK_STR_EQ(lb::clipboard_decision_name(lb::ClipboardDecision::Duplicate), "duplicate");
    CHECK_STR_EQ(lb::clipboard_decision_name(lb::ClipboardDecision::LoopEcho), "loop-echo");
    CHECK_STR_EQ(lb::clipboard_decision_name(lb::ClipboardDecision::TooLarge), "too-large");
    CHECK_STR_EQ(lb::clipboard_decision_name(lb::ClipboardDecision::UnsupportedType),
                 "unsupported-type");
}

TEST(clipboard_config_validation) {
    lb::AppConfig cfg;
    cfg.device_name = "test";
    CHECK(lb::validate_config(cfg).success);

    cfg.clipboard.mode = "manual";
    CHECK(!lb::validate_config(cfg).success);

    cfg.clipboard.mode = "auto";
    cfg.clipboard.max_bytes = 100;
    CHECK(!lb::validate_config(cfg).success);

    cfg.clipboard.max_bytes = 16777216;
    cfg.network.clipboard_port = cfg.network.transfer_port;
    CHECK(!lb::validate_config(cfg).success);
}
