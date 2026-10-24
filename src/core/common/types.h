#pragma once
// LinkBridge 核心公共类型。核心层禁止 include 任何网络/系统 Socket 头文件。
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace lb {

using ByteBuf = std::vector<std::uint8_t>;
using DeviceId = std::string; // hex(SHA256(identity_pub))[0:16]

// 轻量 Expected：核心层统一错误返回（不抛异常穿透模块边界）
template <typename T>
struct Expected {
    std::optional<T> value;
    std::string error;
    bool ok() const { return value.has_value(); }
    bool has_value() const { return value.has_value(); }
    explicit operator bool() const { return value.has_value(); }
    bool operator!() const { return !value.has_value(); }
    const T& operator*() const { return *value; }
    T& operator*() { return *value; }
    T* operator->() { return &*value; }
    const T* operator->() const { return &*value; }
    static Expected fail(std::string e) { return Expected{std::nullopt, std::move(e)}; }
    static Expected success(T v) { return Expected{std::move(v), {}}; }
};

// void 特化场景用 Result
struct Result {
    bool success = false;
    std::string error;
    static Result ok() { return {true, {}}; }
    static Result fail(std::string e) { return {false, std::move(e)}; }
    explicit operator bool() const { return success; }
};

// 设备平台标识
enum class Platform : std::uint8_t { Linux = 1, Windows = 2, Android = 3 };

inline const char* platform_name(Platform p) {
    switch (p) {
        case Platform::Linux: return "linux";
        case Platform::Windows: return "windows";
        case Platform::Android: return "android";
    }
    return "unknown";
}

// 在线状态
enum class Presence : std::uint8_t { Unknown = 0, Online, Offline };

inline std::int64_t now_secs() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// 事件订阅句柄（析构自动退订）
class Subscription {
public:
    Subscription() = default;
    explicit Subscription(std::function<void()> unsub) : unsub_(std::move(unsub)) {}
    ~Subscription() { if (unsub_) unsub_(); }
    Subscription(Subscription&& o) noexcept : unsub_(std::move(o.unsub_)) { o.unsub_ = nullptr; }
    Subscription& operator=(Subscription&& o) noexcept {
        if (this != &o) { if (unsub_) unsub_(); unsub_ = std::move(o.unsub_); o.unsub_ = nullptr; }
        return *this;
    }
    Subscription(const Subscription&) = delete;
    Subscription& operator=(const Subscription&) = delete;
private:
    std::function<void()> unsub_;
};

} // namespace lb
