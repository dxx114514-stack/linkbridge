#include "test.h"

#include "core/config/toml.h"

using namespace lb;

TEST(toml_parse_basic) {
    const std::string text = R"(
# 注释行
[network]
pair_port = 45710
bind_addr = "0.0.0.0"

[heartbeat]
adaptive = true
interval_sec = 30   # 行尾注释
)";
    auto table = TomlTable::parse(text);
    CHECK(table.has_value());
    if (!table) return;
    CHECK(table->get_int("network", "pair_port").value_or(0) == 45710);
    CHECK_STR_EQ(table->get_str("network", "bind_addr").value_or(""), "0.0.0.0");
    CHECK(table->get_bool("heartbeat", "adaptive").value_or(false));
    CHECK(table->get_int("heartbeat", "interval_sec").value_or(0) == 30);
}

TEST(toml_missing_key_returns_nullopt) {
    auto table = TomlTable::parse("[a]\nb = 1\n");
    CHECK(table.has_value());
    if (!table) return;
    CHECK(!table->get_int("a", "c").has_value());
    CHECK(!table->get_str("a", "b").has_value()); // 类型不匹配
}

TEST(toml_quoted_hash_is_not_comment) {
    auto table = TomlTable::parse("[a]\nk = \"x#y\"\n");
    CHECK(table.has_value());
    if (!table) return;
    CHECK_STR_EQ(table->get_str("a", "k").value_or(""), "x#y");
}

TEST(toml_invalid_line_reports_error) {
    auto table = TomlTable::parse("[a]\nthis is not valid\n");
    CHECK(!table.has_value());
}

TEST(toml_section_without_bracket_close) {
    CHECK(!TomlTable::parse("[a\n").has_value());
}
