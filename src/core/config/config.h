#pragma once
// 强类型配置：唯一来源是 TOML 配置文件（不使用环境变量）。
#include "core/common/types.h"

#include <string>

namespace lb {

struct NetworkConfig {
    int pair_port = 45710;
    int heartbeat_port = 45711;
    int ssh_port = 45712;
    int transfer_port = 45713; // M3 文件互传通道
    int clipboard_port = 45714; // M4 剪贴板同步通道
    int screen_port = 45715;    // M5 屏幕共享通道
    int sync_port = 45716;      // M6 文件夹同步通道
    std::string bind_addr = "0.0.0.0";
    int connect_timeout_ms = 5000;
};

struct HeartbeatConfig {
    int interval_sec = 30;         // 基础间隔，允许 10/30/60
    int max_interval_sec = 300;    // 自适应上限
    int max_failures = 3;          // 连续失败阈值
    bool adaptive = true;          // 稳定在线后逐步拉长间隔
    int offline_probe_sec = 60;    // 离线设备的探测间隔
};

struct TransferConfig {           // M3 使用，当前仅解析与校验
    int chunk_size = 1048576;
    int max_concurrent_tasks = 3;
    int rate_limit_kbps = 0;
};

struct ClipboardConfig {          // M4 剪贴板同步
    bool enabled = true;
    std::string mode = "confirm"; // auto | confirm
    int max_bytes = 16777216;     // 16 MiB
};

struct ScreenConfig {             // M5 屏幕共享（v0 仅只读查看）
    bool enabled = true;
    bool require_confirm_each_session = true;
    int max_fps = 2;              // 查看端请求帧率
    int max_bytes = 16777216;     // 单帧上限
};

struct SyncConfig {               // M6/M7 文件夹同步（单文件夹）
    bool enabled = false;
    std::string folder;           // 本端同步根目录；为空表示未配置
    std::string mode = "send_receive"; // send_receive | send_only | receive_only | mirror
    std::string ignore = "*.tmp,~$*,.git/,node_modules/,.linkbridge/";
    int max_file_size_mb = 4096;
    int rescan_interval_sec = 3600;
    std::string default_conflict_policy = "keep_both"; // keep_both | newer | larger
    int max_concurrent_files = 4; // 仅解析与校验，当前串行执行
    bool trash_enabled = true;    // 覆盖/删除前先归档到 .linkbridge/versions
    int version_keep_count = 5;   // 每个文件保留版本数（0 = 不按数量清理）
    int version_keep_days = 30;   // 版本保留天数（0 = 不按时间清理）
    int trash_max_mb = 2048;      // 版本库总容量上限（0 = 不限制）
    int rate_limit_kbps = 0;      // 同步发送限速（0 = 不限速）
};

struct AppConfig {
    std::string device_name;       // 空则由主机名填充
    NetworkConfig network;
    HeartbeatConfig heartbeat;
    TransferConfig transfer;
    ClipboardConfig clipboard;
    ScreenConfig screen;
    SyncConfig sync;
};

// 加载配置文件；文件不存在时返回默认配置并标记 created_default
Expected<AppConfig> load_config(const std::string& path, bool* created_default = nullptr);

// 校验配置合法性，返回错误描述
Result validate_config(const AppConfig& cfg);

// 生成默认配置文件内容（`linkbridge init` 使用）
std::string default_config_text();

std::string config_summary(const AppConfig& cfg);

} // namespace lb
