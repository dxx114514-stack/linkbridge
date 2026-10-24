#pragma once
// 字节流抽象：核心层通过该接口收发数据，不接触任何 Socket 类型。
#include "core/common/types.h"

#include <memory>

namespace lb {

class ByteStream {
public:
    virtual ~ByteStream() = default;
    virtual Result write(const ByteBuf& data) = 0;
    virtual Expected<ByteBuf> read_frame(size_t max_len = 65536) = 0; // 长度前缀分帧
    virtual std::string peer() const = 0;
    virtual void close() = 0;
};

// 出站拨号器：由传输层注入，核心模块借此发起连接
struct IDialer {
    virtual ~IDialer() = default;
    virtual Expected<std::unique_ptr<ByteStream>> dial(const std::string& host, int port,
                                                       int timeout_ms) = 0;
};

} // namespace lb
