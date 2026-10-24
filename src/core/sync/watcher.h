#pragma once
// 文件监听平台抽象：Windows 用 ReadDirectoryChangesW，Linux 用 inotify，Android 用 FileObserver。
// 核心同步引擎只消费“有变化”的回调，由自身做 500ms 防抖与增量/全量扫描。
#include "core/common/types.h"

#include <functional>
#include <memory>
#include <string>

namespace lb {

class IFileWatcher {
public:
    virtual ~IFileWatcher() = default;

    // 递归监听 root 下的创建/修改/删除/重命名；回调可能来自后台线程，需快速返回。
    virtual Result start(const std::string& root, std::function<void()> on_change) = 0;
    virtual void stop() = 0;
};

std::shared_ptr<IFileWatcher> make_file_watcher();

} // namespace lb
