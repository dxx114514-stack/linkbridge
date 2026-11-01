// Windows 通知后端：v0 不展示跨设备镜像通知（系统通知中心接口受限），
// 返回 nullptr 由 NotifyService 降级：通道仍可接收，但明确提示不支持。
#include "core/notify/backend.h"

namespace lb {

std::shared_ptr<INotifyBackend> make_system_notify_backend() { return nullptr; }

} // namespace lb
