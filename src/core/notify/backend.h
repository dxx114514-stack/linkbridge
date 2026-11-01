#pragma once
// 通知平台抽象：核心层只依赖本接口。
// Android 通过 JNI 调用 Kotlin NotifyBridge（系统通知展示 + 预设动作）；
// Windows/Linux 没有通知后端时返回 nullptr，功能自动降级并给出明确提示。
#include "core/common/types.h"

#include <memory>
#include <string>

namespace lb {

// 一条可跨设备镜像的通知。字段全部来自系统通知，不包含正文以外的隐私数据。
struct NotifyMessage {
    std::string id;        // 原始通知 key（系统内唯一，用于去重与动作定位）
    std::string app;       // 来源包名
    std::string app_name;  // 来源应用显示名
    std::string title;
    std::string text;
    std::int64_t posted_at = 0; // 原始通知发出时间（秒）
    int priority = 1;           // 0 低 / 1 普通 / 2 高
};

enum class NotifyActionMode : std::uint8_t {
    OpenApp = 0, // 打开来源应用（最安全、无需额外权限）
    Tap = 1,     // 在来源设备按预设坐标点击（需 Shizuku 或无障碍注入）
    None = 2,    // 仅镜像，不执行动作
};

struct NotifyAction {
    NotifyActionMode mode = NotifyActionMode::OpenApp;
    int x = 0;
    int y = 0;
};

class INotifyBackend {
public:
    virtual ~INotifyBackend() = default;

    // 在本机展示镜像通知；点击镜像通知时由平台层回调 NotifyService::request_action。
    virtual Result show(const NotifyMessage& msg, const std::string& source_device_id,
                        const std::string& source_name) = 0;

    // 在本机执行通知预设动作（来源设备侧调用）。
    virtual Result act(const NotifyMessage& msg, const NotifyAction& action) = 0;

    // 当前平台是否能处理通知镜像/动作
    virtual bool available() const = 0;
    virtual std::string unavailable_reason() const = 0;
};

// 平台实现：Android 返回 JNI 后端；Windows/Linux 返回 nullptr。
std::shared_ptr<INotifyBackend> make_system_notify_backend();

} // namespace lb
