#include "test.h"

#include "core/heartbeat/heartbeat.h"

using namespace lb;

TEST(heartbeat_policy_base_interval) {
    HeartbeatPolicy p;
    p.base_interval_sec = 30;
    p.max_interval_sec = 300;
    p.stable_probes = 10;

    CHECK(p.after_success(30, 0) == 30);
    CHECK(p.after_success(30, 9) == 30);
    CHECK(p.after_success(30, 10) == 60);
    CHECK(p.after_success(60, 10) == 120);
    CHECK(p.after_success(150, 10) == 300);
}

TEST(heartbeat_policy_caps_at_max) {
    HeartbeatPolicy p;
    p.base_interval_sec = 30;
    p.max_interval_sec = 300;
    CHECK(p.after_success(300, 100) == 300);
}

TEST(heartbeat_policy_resets_on_failure) {
    HeartbeatPolicy p;
    p.base_interval_sec = 60;
    CHECK(p.after_failure() == 60);
}

TEST(heartbeat_policy_offline_threshold) {
    HeartbeatPolicy p;
    p.max_failures = 3;
    CHECK(!p.should_mark_offline(0));
    CHECK(!p.should_mark_offline(1));
    CHECK(!p.should_mark_offline(2));
    CHECK(p.should_mark_offline(3));
    CHECK(p.should_mark_offline(4));
}
