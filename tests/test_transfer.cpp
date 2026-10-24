#include "core/transfer/transfer.h"

#include "test.h"

#include <string>

TEST(crc32c_standard_vectors) {
    CHECK(lb::TransferService::crc32c(nullptr, 0) == 0u);
    const std::string check = "123456789";
    CHECK(lb::TransferService::crc32c(check.data(), check.size()) == 0xE3069283u);
    const std::string abc = "abc";
    CHECK(lb::TransferService::crc32c(abc.data(), abc.size()) == 0x364B3FB7u);
}

TEST(crc32c_incremental_matches_whole) {
    const lb::ByteBuf data = {0x00, 0xFF, 0x10, 0x7F, 0x80, 0x01, 0x02, 0x03};
    CHECK(lb::TransferService::crc32c(data) == lb::TransferService::crc32c(data.data(), data.size()));
}

TEST(sanitize_accepts_normal_names) {
    const char* names[] = {"report.pdf", "中文 文件名.txt", ".gitignore", "a b.txt", "x"};
    for (const char* n : names) {
        auto out = lb::TransferService::sanitize_file_name(n);
        CHECK(out.has_value());
        if (out) CHECK_STR_EQ(*out, n);
    }
}

TEST(sanitize_rejects_path_traversal) {
    const char* names[] = {"../evil.txt", "..", ".", "a/b.txt", "a\\b.txt", "C:evil.txt",
                           "/etc/passwd"};
    for (const char* n : names) {
        CHECK(!lb::TransferService::sanitize_file_name(n).has_value());
    }
}

TEST(sanitize_rejects_windows_reserved_and_control) {
    const char* names[] = {"CON", "con.txt", "PRN.log", "NUL", "LPT9.dat"};
    for (const char* n : names) {
        CHECK(!lb::TransferService::sanitize_file_name(n).has_value());
    }
    CHECK(!lb::TransferService::sanitize_file_name(std::string("bad\nname")).has_value());
    CHECK(!lb::TransferService::sanitize_file_name(std::string("bad\x7Fname")).has_value());
}

TEST(sanitize_rejects_bad_edges) {
    CHECK(!lb::TransferService::sanitize_file_name("").has_value());
    CHECK(!lb::TransferService::sanitize_file_name(std::string(201, 'a')).has_value());
    CHECK(!lb::TransferService::sanitize_file_name("trailing.").has_value());
    CHECK(!lb::TransferService::sanitize_file_name("trailing ").has_value());
    CHECK(lb::TransferService::sanitize_file_name(std::string(200, 'a')).has_value());
}
