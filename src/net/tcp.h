#pragma once
// TCP 传输实现：唯一接触 WinSock 的模块。核心层只看到 ByteStream 接口。
#include "core/common/stream.h"

#include <cstdint>
#include <memory>

namespace lb::net {

// 进程内仅需初始化一次；内部使用 std::once_flag
bool wsa_init();

class TcpStream : public ByteStream {
public:
    explicit TcpStream(std::uintptr_t sock, std::string peer, int timeout_ms = 15000);
    ~TcpStream() override;

    Result write(const ByteBuf& data) override;
    Expected<ByteBuf> read_frame(size_t max_len = 65536) override;
    std::string peer() const override { return peer_; }
    void close() override;
    void set_timeouts(int timeout_ms);

private:
    std::uintptr_t sock_;
    std::string peer_;
    bool closed_ = false;
};

class TcpDialer : public IDialer {
public:
    Expected<std::unique_ptr<ByteStream>> dial(const std::string& host, int port,
                                               int timeout_ms) override;
};

class Listener {
public:
    ~Listener();
    static Expected<std::unique_ptr<Listener>> bind(const std::string& addr, int port, int backlog = 16);
    // 返回 nullptr 表示超时（非错误）
    Expected<std::unique_ptr<ByteStream>> accept(int timeout_ms);
    int port() const { return port_; }
    void close();

private:
    std::uintptr_t sock_;
    int port_ = 0;
};

} // namespace lb::net
