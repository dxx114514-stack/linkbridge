#include "core/config/toml.h"

#include "core/platform/platform.h"

#include <cctype>
#include <sstream>

namespace lb {
namespace {

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

// 去掉引号外的行尾注释
std::string strip_comment(const std::string& s) {
    bool in_str = false;
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '"') in_str = !in_str;
        else if (c == '#' && !in_str) return s.substr(0, i);
    }
    return s;
}

Expected<TomlValue> parse_value(const std::string& raw) {
    TomlValue v;
    if (raw.empty()) return Expected<TomlValue>::fail("empty value");
    if (raw.front() == '"' && raw.back() == '"' && raw.size() >= 2) {
        v.type = TomlValue::Type::Str;
        v.s = raw.substr(1, raw.size() - 2);
        return Expected<TomlValue>::success(std::move(v));
    }
    if (raw == "true" || raw == "false") {
        v.type = TomlValue::Type::Bool;
        v.b = (raw == "true");
        return Expected<TomlValue>::success(std::move(v));
    }
    try {
        v.type = TomlValue::Type::Int;
        v.i = std::stoll(raw);
        return Expected<TomlValue>::success(std::move(v));
    } catch (...) {
        return Expected<TomlValue>::fail("unsupported value: " + raw);
    }
}

} // namespace

Expected<TomlTable> TomlTable::parse(const std::string& text) {
    TomlTable table;
    std::string section;
    std::istringstream in(text);
    std::string line;
    int line_no = 0;

    while (std::getline(in, line)) {
        ++line_no;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string s = trim(strip_comment(line));
        if (s.empty()) continue;

        if (s.front() == '[') {
            if (s.back() != ']') return Expected<TomlTable>::fail("line " + std::to_string(line_no) + ": bad section");
            section = trim(s.substr(1, s.size() - 2));
            if (section.empty()) return Expected<TomlTable>::fail("line " + std::to_string(line_no) + ": empty section");
            continue;
        }

        const size_t eq = s.find('=');
        if (eq == std::string::npos)
            return Expected<TomlTable>::fail("line " + std::to_string(line_no) + ": missing '='");
        const std::string key = trim(s.substr(0, eq));
        const std::string value_text = trim(s.substr(eq + 1));
        if (key.empty() || value_text.empty())
            return Expected<TomlTable>::fail("line " + std::to_string(line_no) + ": empty key/value");

        auto parsed = parse_value(value_text);
        if (!parsed) return Expected<TomlTable>::fail(parsed.error);

        const std::string full = section.empty() ? key : section + "." + key;
        table.values_[full] = *parsed;
    }
    return Expected<TomlTable>::success(std::move(table));
}

Expected<TomlTable> TomlTable::parse_file(const std::string& path) {
    auto data = pal::read_file(path);
    if (!data) return Expected<TomlTable>::fail(data.error);
    return parse(std::string(data->begin(), data->end()));
}

std::optional<std::int64_t> TomlTable::get_int(const std::string& section,
                                               const std::string& key) const {
    const auto it = values_.find(section + "." + key);
    if (it == values_.end() || it->second.type != TomlValue::Type::Int) return std::nullopt;
    return it->second.i;
}

std::optional<bool> TomlTable::get_bool(const std::string& section, const std::string& key) const {
    const auto it = values_.find(section + "." + key);
    if (it == values_.end() || it->second.type != TomlValue::Type::Bool) return std::nullopt;
    return it->second.b;
}

std::optional<std::string> TomlTable::get_str(const std::string& section,
                                              const std::string& key) const {
    const auto it = values_.find(section + "." + key);
    if (it == values_.end() || it->second.type != TomlValue::Type::Str) return std::nullopt;
    return it->second.s;
}

std::vector<std::string> TomlTable::keys() const {
    std::vector<std::string> out;
    out.reserve(values_.size());
    for (const auto& kv : values_) out.push_back(kv.first);
    return out;
}

} // namespace lb
