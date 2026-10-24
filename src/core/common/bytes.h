#pragma once
// 线协议二进制编解码（大端序，带边界检查）与 hex 工具。
// 设计文档中控制面为 CBOR；v0 实现采用等价的紧凑自定义二进制编码，
// 仅存在于传输边界，核心模块只见解码后的字段，后续可整体替换为 CBOR。
#include "core/common/types.h"

namespace lb {

class ByteWriter {
public:
    void u8(std::uint8_t v);
    void u16(std::uint16_t v);
    void u32(std::uint32_t v);
    void u64(std::uint64_t v);
    void i64(std::int64_t v);
    void raw(const void* p, size_t n);
    void bytes(const ByteBuf& b);          // u32 长度前缀 + 内容
    void str(const std::string& s);        // 同 bytes
    const ByteBuf& buf() const { return buf_; }
private:
    ByteBuf buf_;
};

class ByteReader {
public:
    explicit ByteReader(const ByteBuf& b) : p_(b.data()), n_(b.size()) {}
    ByteReader(const void* p, size_t n) : p_(static_cast<const std::uint8_t*>(p)), n_(n) {}

    std::uint8_t  u8();
    std::uint16_t u16();
    std::uint32_t u32();
    std::uint64_t u64();
    std::int64_t  i64();
    ByteBuf       bytes();                 // 长度前缀读取（上限 64MB 防恶意长度）
    std::string   str();
    ByteBuf       raw(size_t n);           // 定长读取
    size_t remaining() const { return n_ - off_; }
    bool ok() const { return !fail_; }

private:
    const std::uint8_t* p_;
    size_t n_ = 0, off_ = 0;
    bool fail_ = false;
    void need(size_t k);
};

std::string to_hex(const void* p, size_t n);
std::string to_hex(const ByteBuf& b);
Expected<ByteBuf> from_hex(const std::string& s);

// 常量时间比较（校验 HMAC/指纹用）
bool ct_equal(const void* a, const void* b, size_t n);

} // namespace lb
