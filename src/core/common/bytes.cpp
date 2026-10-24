#include "core/common/bytes.h"

#include <cstring>
#include <iomanip>
#include <sstream>

namespace lb {

// ---------------- ByteWriter ----------------

void ByteWriter::u8(std::uint8_t v) { buf_.push_back(v); }

void ByteWriter::u16(std::uint16_t v) {
    buf_.push_back(static_cast<std::uint8_t>(v >> 8));
    buf_.push_back(static_cast<std::uint8_t>(v));
}

void ByteWriter::u32(std::uint32_t v) {
    for (int i = 3; i >= 0; --i) buf_.push_back(static_cast<std::uint8_t>(v >> (i * 8)));
}

void ByteWriter::u64(std::uint64_t v) {
    for (int i = 7; i >= 0; --i) buf_.push_back(static_cast<std::uint8_t>(v >> (i * 8)));
}

void ByteWriter::i64(std::int64_t v) { u64(static_cast<std::uint64_t>(v)); }

void ByteWriter::raw(const void* p, size_t n) {
    const auto* b = static_cast<const std::uint8_t*>(p);
    buf_.insert(buf_.end(), b, b + n);
}

void ByteWriter::bytes(const ByteBuf& b) {
    u32(static_cast<std::uint32_t>(b.size()));
    if (!b.empty()) raw(b.data(), b.size());
}

void ByteWriter::str(const std::string& s) {
    u32(static_cast<std::uint32_t>(s.size()));
    if (!s.empty()) raw(s.data(), s.size());
}

// ---------------- ByteReader ----------------

void ByteReader::need(size_t k) {
    if (fail_) return;
    if (off_ + k > n_) { fail_ = true; return; }
}

std::uint8_t ByteReader::u8() {
    need(1);
    if (fail_) return 0;
    return p_[off_++];
}

std::uint16_t ByteReader::u16() {
    need(2);
    if (fail_) return 0;
    std::uint16_t v = static_cast<std::uint16_t>(p_[off_] << 8 | p_[off_ + 1]);
    off_ += 2;
    return v;
}

std::uint32_t ByteReader::u32() {
    need(4);
    if (fail_) return 0;
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v = (v << 8) | p_[off_ + i];
    off_ += 4;
    return v;
}

std::uint64_t ByteReader::u64() {
    need(8);
    if (fail_) return 0;
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | p_[off_ + i];
    off_ += 8;
    return v;
}

std::int64_t ByteReader::i64() { return static_cast<std::int64_t>(u64()); }

ByteBuf ByteReader::raw(size_t n) {
    need(n);
    if (fail_) return {};
    ByteBuf out(p_ + off_, p_ + off_ + n);
    off_ += n;
    return out;
}

ByteBuf ByteReader::bytes() {
    const std::uint32_t n = u32();
    if (fail_) return {};
    if (n > (64u * 1024u * 1024u)) { // 防恶意长度；与各通道 max_bytes 上限一致
        fail_ = true;
        return {};
    }
    return raw(n);
}

std::string ByteReader::str() {
    ByteBuf b = bytes();
    if (fail_) return {};
    return std::string(b.begin(), b.end());
}

// ---------------- hex / 工具 ----------------

std::string to_hex(const void* p, size_t n) {
    static const char* kDigits = "0123456789abcdef";
    const auto* b = static_cast<const std::uint8_t*>(p);
    std::string out;
    out.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        out.push_back(kDigits[b[i] >> 4]);
        out.push_back(kDigits[b[i] & 0x0f]);
    }
    return out;
}

std::string to_hex(const ByteBuf& b) { return to_hex(b.data(), b.size()); }

Expected<ByteBuf> from_hex(const std::string& s) {
    if (s.size() % 2 != 0) return Expected<ByteBuf>::fail("hex length odd");
    auto val = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    ByteBuf out;
    out.reserve(s.size() / 2);
    for (size_t i = 0; i < s.size(); i += 2) {
        int hi = val(s[i]), lo = val(s[i + 1]);
        if (hi < 0 || lo < 0) return Expected<ByteBuf>::fail("hex invalid char");
        out.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
    }
    return Expected<ByteBuf>::success(std::move(out));
}

bool ct_equal(const void* a, const void* b, size_t n) {
    const auto* x = static_cast<const std::uint8_t*>(a);
    const auto* y = static_cast<const std::uint8_t*>(b);
    volatile std::uint8_t diff = 0;
    for (size_t i = 0; i < n; ++i) diff |= static_cast<std::uint8_t>(x[i] ^ y[i]);
    return diff == 0;
}

} // namespace lb
