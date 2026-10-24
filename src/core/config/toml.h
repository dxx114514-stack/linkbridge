#pragma once
// 极简 TOML 子集解析：支持 [section]、key = int/bool/string、# 注释。
// 仅用于加载本机配置；不追求完整 TOML 规范（不做数组/表数组/多行字符串）。
#include "core/common/types.h"

#include <map>
#include <optional>

namespace lb {

struct TomlValue {
    enum class Type { Int, Bool, Str } type = Type::Str;
    std::int64_t i = 0;
    bool b = false;
    std::string s;
};

class TomlTable {
public:
    static Expected<TomlTable> parse(const std::string& text);
    static Expected<TomlTable> parse_file(const std::string& path);

    std::optional<std::int64_t> get_int(const std::string& section, const std::string& key) const;
    std::optional<bool> get_bool(const std::string& section, const std::string& key) const;
    std::optional<std::string> get_str(const std::string& section, const std::string& key) const;

    // 用于诊断：返回所有解析到的 "section.key"
    std::vector<std::string> keys() const;

private:
    std::map<std::string, TomlValue> values_; // key = "section.key"
};

} // namespace lb
