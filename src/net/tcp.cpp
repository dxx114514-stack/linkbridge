#include "net/tcp.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstring>
#include <mutex>

namespace lb::net {
namespace {

// 单帧硬上限。真正按通道生效的限制是各调用点传入的 max_len；此处只兜底，
// 需容纳屏幕帧（[screen].max_bytes 上限 64 MiB）加加密开销。
constexpr size_t kMaxFrame = 64 * 1024 * 1024 + 64 * 1024;

bool send_all(SOCKET s, const std::uint8_t* data, size_t n) {
    size_t sent = 0;
    while (sent < n) {
        const int r = ::send(s, reinterpret_cast<const char*>(data + sent),
                             static_cast<int>(n - sent), 0);
        if (r <= 0) return false;
        sent += static_cast<size_t>(r);
    }
    return true;
}

bool recv_all(SOCKET s, std::uint8_t* data, size_t n) {
    size_t got = 0;
    while (got < n) {
        const int r = ::recv(s, reinterpret_cast<char*>(data + got), static_cast<int>(n - got), 0);
        if (r <= 0) return false;
        got += static_cast<size_t>(r);
    }
    return true;
}

void apply_timeout(SOCKET s, int timeout_ms) {
    if (timeout_ms <= 0) return;
    DWORD t = static_cast<DWORD>(timeout_ms);
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&t), sizeof(t));
    ::setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&t), sizeof(t));
}

std::string endpoint_to_string(const sockaddr_in& addr) {
    char buf[INET_ADDRSTRLEN] = {};
    ::inet_ntop(AF_INET, const_cast<in_addr*>(&addr.sin_addr), buf, sizeof(buf));
    return std::string(buf) + ":" + std::to_string(ntohs(addr.sin_port));
}

} // namespace

bool wsa_init() {
    static std::once_flag flag;
    static bool ok = false;
    std::call_once(flag, [] {
        WSADATA wsa{};
        ok = (WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
    });
    return ok;
}

// ---------------- TcpStream ----------------

TcpStream::TcpStream(std::uintptr_t sock, std::string peer, int timeout_ms)
    : sock_(sock), peer_(std::move(peer)) {
    apply_timeout(static_cast<SOCKET>(sock_), timeout_ms);
}

TcpStream::~TcpStream() { close(); }

void TcpStream::close() {
    if (closed_) return;
    closed_ = true;
    SOCKET s = static_cast<SOCKET>(sock_);
    if (s != INVALID_SOCKET) {
        ::shutdown(s, SD_BOTH);
        ::closesocket(s);
    }
    sock_ = static_cast<std::uintptr_t>(INVALID_SOCKET);
}

void TcpStream::set_timeouts(int timeout_ms) { apply_timeout(static_cast<SOCKET>(sock_), timeout_ms); }

Result TcpStream::write(const ByteBuf& data) {
    if (closed_) return Result::fail("连接已关闭");
    if (data.size() > kMaxFrame) return Result::fail("帧过大");

    std::uint8_t header[4] = {
        static_cast<std::uint8_t>((data.size() >> 24) & 0xff),
        static_cast<std::uint8_t>((data.size() >> 16) & 0xff),
        static_cast<std::uint8_t>((data.size() >> 8) & 0xff),
        static_cast<std::uint8_t>(data.size() & 0xff),
    };
    SOCKET s = static_cast<SOCKET>(sock_);
    if (!send_all(s, header, sizeof(header))) return Result::fail("发送长度失败");
    if (!data.empty() && !send_all(s, data.data(), data.size())) return Result::fail("发送数据失败");
    return Result::ok();
}

Expected<ByteBuf> TcpStream::read_frame(size_t max_len) {
    if (closed_) return Expected<ByteBuf>::fail("连接已关闭");
    std::uint8_t header[4];
    SOCKET s = static_cast<SOCKET>(sock_);
    if (!recv_all(s, header, sizeof(header))) return Expected<ByteBuf>::fail("读取长度失败");

    const std::uint32_t len =
        (static_cast<std::uint32_t>(header[0]) << 24) | (static_cast<std::uint32_t>(header[1]) << 16) |
        (static_cast<std::uint32_t>(header[2]) << 8) | static_cast<std::uint32_t>(header[3]);
    if (len > max_len || len > kMaxFrame)
        return Expected<ByteBuf>::fail("帧长度超限");

    ByteBuf data(len);
    if (len > 0 && !recv_all(s, data.data(), len)) return Expected<ByteBuf>::fail("读取数据失败");
    return Expected<ByteBuf>::success(std::move(data));
}

// ---------------- TcpDialer ----------------

Expected<std::unique_ptr<ByteStream>> TcpDialer::dial(const std::string& host, int port,
                                                     int timeout_ms) {
    if (!wsa_init()) return Expected<std::unique_ptr<ByteStream>>::fail("WSAStartup 失败");

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<u_short>(port));
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1)
        return Expected<std::unique_ptr<ByteStream>>::fail("IP 地址非法: " + host);

    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return Expected<std::unique_ptr<ByteStream>>::fail("创建 socket 失败");

    // 非阻塞 connect + select 实现超时
    u_long nonblocking = 1;
    ::ioctlsocket(s, FIONBIO, &nonblocking);

    const int rc = ::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (rc == SOCKET_ERROR) {
        if (WSAGetLastError() != WSAEWOULDBLOCK) {
            ::closesocket(s);
            return Expected<std::unique_ptr<ByteStream>>::fail("连接失败");
        }
        fd_set wset;
        FD_ZERO(&wset);
        FD_SET(s, &wset);
        timeval tv{};
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        const int sel = ::select(static_cast<int>(s) + 1, nullptr, &wset, nullptr, &tv);
        if (sel <= 0) {
            ::closesocket(s);
            return Expected<std::unique_ptr<ByteStream>>::fail("连接超时");
        }
        int err = 0;
        int len = sizeof(err);
        ::getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len);
        if (err != 0) {
            ::closesocket(s);
            return Expected<std::unique_ptr<ByteStream>>::fail("连接被拒绝");
        }
    }
    u_long blocking = 0;
    ::ioctlsocket(s, FIONBIO, &blocking);

    return Expected<std::unique_ptr<ByteStream>>::success(
        std::make_unique<TcpStream>(static_cast<std::uintptr_t>(s), endpoint_to_string(addr),
                                    timeout_ms));
}

// ---------------- Listener ----------------

Listener::~Listener() { close(); }

void Listener::close() {
    SOCKET s = static_cast<SOCKET>(sock_);
    if (s != INVALID_SOCKET) {
        ::closesocket(s);
        sock_ = static_cast<std::uintptr_t>(INVALID_SOCKET);
    }
}

Expected<std::unique_ptr<Listener>> Listener::bind(const std::string& addr, int port, int backlog) {
    if (!wsa_init()) return Expected<std::unique_ptr<Listener>>::fail("WSAStartup 失败");

    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return Expected<std::unique_ptr<Listener>>::fail("创建监听 socket 失败");

    BOOL reuse = TRUE;
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(static_cast<u_short>(port));
    if (addr.empty() || addr == "0.0.0.0") {
        local.sin_addr.s_addr = INADDR_ANY;
    } else if (::inet_pton(AF_INET, addr.c_str(), &local.sin_addr) != 1) {
        ::closesocket(s);
        return Expected<std::unique_ptr<Listener>>::fail("监听地址非法: " + addr);
    }

    if (::bind(s, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == SOCKET_ERROR) {
        ::closesocket(s);
        return Expected<std::unique_ptr<Listener>>::fail("端口绑定失败（可能已被占用）");
    }
    if (::listen(s, backlog) == SOCKET_ERROR) {
        ::closesocket(s);
        return Expected<std::unique_ptr<Listener>>::fail("listen 失败");
    }

    sockaddr_in actual{};
    int len = sizeof(actual);
    ::getsockname(s, reinterpret_cast<sockaddr*>(&actual), &len);

    auto listener = std::make_unique<Listener>();
    listener->sock_ = static_cast<std::uintptr_t>(s);
    listener->port_ = ntohs(actual.sin_port);
    return Expected<std::unique_ptr<Listener>>::success(std::move(listener));
}

Expected<std::unique_ptr<ByteStream>> Listener::accept(int timeout_ms) {
    SOCKET s = static_cast<SOCKET>(sock_);
    if (s == INVALID_SOCKET) return Expected<std::unique_ptr<ByteStream>>::fail("监听已关闭");

    fd_set rset;
    FD_ZERO(&rset);
    FD_SET(s, &rset);
    timeval tv{};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    const int sel = ::select(static_cast<int>(s) + 1, &rset, nullptr, nullptr, &tv);
    if (sel == 0) return Expected<std::unique_ptr<ByteStream>>::success(nullptr); // 超时
    if (sel < 0) return Expected<std::unique_ptr<ByteStream>>::fail("select 失败");

    sockaddr_in peer{};
    int len = sizeof(peer);
    SOCKET client = ::accept(s, reinterpret_cast<sockaddr*>(&peer), &len);
    if (client == INVALID_SOCKET)
        return Expected<std::unique_ptr<ByteStream>>::success(nullptr);

    return Expected<std::unique_ptr<ByteStream>>::success(std::make_unique<TcpStream>(
        static_cast<std::uintptr_t>(client), endpoint_to_string(peer), 5000));
}

} // namespace lb::net
