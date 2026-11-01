// Linux 通知后端：v0 不展示跨设备镜像通知（可选 libnotify 后续实现），
// 返回 nullptr 由 NotifyService 降级并给出明确提示。
#include "core/notify/backend.h"

namespace lb {

std::shared_ptr<INotifyBackend> make_system_notify_backend() { return nullptr; }

} // namespace lb
