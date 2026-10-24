// POSIX 传输实现（Linux / Android）：与 tcp.cpp 语义一致，仅替换 socket API。
// 帧格式：4 字节大端长度 + 负载；核心层只看到 ByteStream 接口。
#include "net/tcp.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <mutex>

namespace lb::net {
namespace {

constexpr size_t kMaxFrame = 64 * 1024 * 1024 + 64 * 1024;

bool send_all(int fd, const std::uint8_t* data, size_t n) {
    size_t sent = 0;
    while (sent < n) {
        const ssize_t r = ::send(fd, data + sent, n - sent, MSG_NOSIGNAL);
        if (r <= 0) {
            if (r < 0 && errno == EINTR) continue;
            return false;
        }
        sent += static_cast<size_t>(r);
    }
    return true;
}

bool recv_all(int fd, std::uint8_t* data, size_t n) {
    size_t got = 0;
    while (got < n) {
        const ssize_t r = ::recv(fd, data + got, n - got, 0);
        if (r <= 0) {
            if (r < 0 && errno == EINTR) continue;
            return false;
        }
        got += static_cast<size_t>(r);
    }
    return true;
}

void apply_timeout(int fd, int timeout_ms) {
    if (timeout_ms <= 0) return;
    timeval tv{};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

std::string endpoint_to_string(const sockaddr_in& addr) {
    char buf[INET_ADDRSTRLEN] = {};
    ::inet_ntop(AF_INET, const_cast<in_addr*>(&addr.sin_addr), buf, sizeof(buf));
    return std::string(buf) + ":" + std::to_string(ntohs(addr.sin_port));
}

void set_nonblocking(int fd, bool on) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) return;
    ::fcntl(fd, F_SETFL, on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK));
}

} // namespace

bool wsa_init() { return true; } // POSIX 无需初始化

// ---------------- TcpStream ----------------

TcpStream::TcpStream(std::uintptr_t sock, std::string peer, int timeout_ms)
    : sock_(sock), peer_(std::move(peer)) {
    apply_timeout(static_cast<int>(sock_), timeout_ms);
}

TcpStream::~TcpStream() { close(); }

void TcpStream::close() {
    if (closed_) return;
    closed_ = true;
    const int fd = static_cast<int>(sock_);
    if (fd >= 0) {
        ::shutdown(fd, SHUT_RDWR);
        ::close(fd);
    }
    sock_ = static_cast<std::uintptr_t>(-1);
}

void TcpStream::set_timeouts(int timeout_ms) { apply_timeout(static_cast<int>(sock_), timeout_ms); }

Result TcpStream::write(const ByteBuf& data) {
    if (closed_) return Result::fail("连接已关闭");
    if (data.size() > kMaxFrame) return Result::fail("帧过大");

    const std::uint8_t header[4] = {
        static_cast<std::uint8_t>((data.size() >> 24) & 0xff),
        static_cast<std::uint8_t>((data.size() >> 16) & 0xff),
        static_cast<std::uint8_t>((data.size() >> 8) & 0xff),
        static_cast<std::uint8_t>(data.size() & 0xff),
    };
    const int fd = static_cast<int>(sock_);
    if (!send_all(fd, header, sizeof(header))) return Result::fail("发送长度失败");
    if (!data.empty() && !send_all(fd, data.data(), data.size())) return Result::fail("发送数据失败");
    return Result::ok();
}

Expected<ByteBuf> TcpStream::read_frame(size_t max_len) {
    if (closed_) return Expected<ByteBuf>::fail("连接已关闭");
    std::uint8_t header[4];
    const int fd = static_cast<int>(sock_);
    if (!recv_all(fd, header, sizeof(header))) return Expected<ByteBuf>::fail("读取长度失败");

    const std::uint32_t len = (static_cast<std::uint32_t>(header[0]) << 24) |
                              (static_cast<std::uint32_t>(header[1]) << 16) |
                              (static_cast<std::uint32_t>(header[2]) << 8) |
                              static_cast<std::uint32_t>(header[3]);
    if (len > max_len || len > kMaxFrame) return Expected<ByteBuf>::fail("帧长度超限");

    ByteBuf data(len);
    if (len > 0 && !recv_all(fd, data.data(), len)) return Expected<ByteBuf>::fail("读取数据失败");
    return Expected<ByteBuf>::success(std::move(data));
}

// ---------------- TcpDialer ----------------

Expected<std::unique_ptr<ByteStream>> TcpDialer::dial(const std::string& host, int port,
                                                     int timeout_ms) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(port));
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1)
        return Expected<std::unique_ptr<ByteStream>>::fail("IP 地址非法: " + host);

    const int fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) return Expected<std::unique_ptr<ByteStream>>::fail("创建 socket 失败");

    set_nonblocking(fd, true);
    const int rc = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (rc != 0) {
        if (errno != EINPROGRESS) {
            ::close(fd);
            return Expected<std::unique_ptr<ByteStream>>::fail("连接失败");
        }
        pollfd p{};
        p.fd = fd;
        p.events = POLLOUT;
        const int sel = ::poll(&p, 1, timeout_ms);
        if (sel <= 0) {
            ::close(fd);
            return Expected<std::unique_ptr<ByteStream>>::fail("连接超时");
        }
        int err = 0;
        socklen_t len = sizeof(err);
        ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
        if (err != 0) {
            ::close(fd);
            return Expected<std::unique_ptr<ByteStream>>::fail("连接被拒绝");
        }
    }
    set_nonblocking(fd, false);

    return Expected<std::unique_ptr<ByteStream>>::success(std::make_unique<TcpStream>(
        static_cast<std::uintptr_t>(fd), endpoint_to_string(addr), timeout_ms));
}

// ---------------- Listener ----------------

Listener::~Listener() { close(); }

void Listener::close() {
    const int fd = static_cast<int>(sock_);
    if (fd >= 0) {
        ::close(fd);
        sock_ = static_cast<std::uintptr_t>(-1);
    }
}

Expected<std::unique_ptr<Listener>> Listener::bind(const std::string& addr, int port, int backlog) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) return Expected<std::unique_ptr<Listener>>::fail("创建监听 socket 失败");

    const int reuse = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(static_cast<std::uint16_t>(port));
    if (addr.empty() || addr == "0.0.0.0") {
        local.sin_addr.s_addr = INADDR_ANY;
    } else if (::inet_pton(AF_INET, addr.c_str(), &local.sin_addr) != 1) {
        ::close(fd);
        return Expected<std::unique_ptr<Listener>>::fail("监听地址非法: " + addr);
    }

    if (::bind(fd, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) {
        ::close(fd);
        return Expected<std::unique_ptr<Listener>>::fail("端口绑定失败（可能已被占用）");
    }
    if (::listen(fd, backlog) != 0) {
        ::close(fd);
        return Expected<std::unique_ptr<Listener>>::fail("listen 失败");
    }

    sockaddr_in actual{};
    socklen_t len = sizeof(actual);
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&actual), &len);

    auto listener = std::make_unique<Listener>();
    listener->sock_ = static_cast<std::uintptr_t>(fd);
    listener->port_ = ntohs(actual.sin_port);
    return Expected<std::unique_ptr<Listener>>::success(std::move(listener));
}

Expected<std::unique_ptr<ByteStream>> Listener::accept(int timeout_ms) {
    const int fd = static_cast<int>(sock_);
    if (fd < 0) return Expected<std::unique_ptr<ByteStream>>::fail("监听已关闭");

    pollfd p{};
    p.fd = fd;
    p.events = POLLIN;
    const int sel = ::poll(&p, 1, timeout_ms);
    if (sel == 0) return Expected<std::unique_ptr<ByteStream>>::success(nullptr); // 超时
    if (sel < 0) return Expected<std::unique_ptr<ByteStream>>::fail("poll 失败");

    sockaddr_in peer{};
    socklen_t len = sizeof(peer);
    const int client = ::accept(fd, reinterpret_cast<sockaddr*>(&peer), &len);
    if (client < 0) return Expected<std::unique_ptr<ByteStream>>::success(nullptr);

    return Expected<std::unique_ptr<ByteStream>>::success(std::make_unique<TcpStream>(
        static_cast<std::uintptr_t>(client), endpoint_to_string(peer), 5000));
}

} // namespace lb::net
