#pragma once
// 平台能力抽象：文件系统、路径、设备信息、操作系统密钥保护。
// core 只依赖本头文件，具体实现在 pal/<平台>。
#include "core/common/types.h"

#include <string>

namespace lb::pal {

// ---- 文件与目录 ----
Result ensure_dir(const std::string& path);
bool file_exists(const std::string& path);
Expected<ByteBuf> read_file(const std::string& path);
Result write_file_atomic(const std::string& path, const ByteBuf& data); // 先写 .tmp 再替换
Result remove_file(const std::string& path);
Result rename_file(const std::string& from, const std::string& to); // 跨目录移动/落盘改名
std::string join_path(const std::string& a, const std::string& b);
std::int64_t file_mtime(const std::string& path);

// ---- 标准目录 ----
std::string data_dir();            // 应用数据目录（不存在则创建）
std::string default_config_path(); // 配置文件路径

#if defined(__ANDROID__)
// Android 没有可用的 HOME/环境变量：进程启动时由 JNI 注入应用私有目录。
// files_dir 同时作为 data_dir()；cache_dir 供临时文件使用。
void android_set_paths(const std::string& files_dir, const std::string& cache_dir);
std::string cache_dir();
#endif

// ---- 设备信息 ----
std::string hostname();
std::string primary_mac();                        // 首个非虚拟网卡 MAC
std::string local_ip_for(const std::string& peer); // 到该对端将使用的本机地址
std::string system_type();                        // "windows" / "linux" / "android"

// ---- 操作系统级密钥保护（Windows=DPAPI，Linux=libsecret，Android=Keystore）----
ByteBuf protect_secret(const ByteBuf& plain);
Expected<ByteBuf> unprotect_secret(const ByteBuf& sealed);

} // namespace lb::pal
