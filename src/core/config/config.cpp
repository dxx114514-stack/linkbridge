#include "core/config/config.h"

#include "core/config/toml.h"
#include "core/platform/platform.h"

#include <sstream>

namespace lb {

std::string default_config_text() {
    return R"([identity]
# 设备名留空则使用系统主机名
device_name = ""

[network]
pair_port = 45710
heartbeat_port = 45711
ssh_port = 45712
transfer_port = 45713
clipboard_port = 45714
screen_port = 45715
sync_port = 45716
bind_addr = "0.0.0.0"
connect_timeout_ms = 5000

[heartbeat]
interval_sec = 30
max_interval_sec = 300
max_failures = 3
adaptive = true
offline_probe_sec = 60

[transfer]
chunk_size = 1048576
max_concurrent_tasks = 3
rate_limit_kbps = 0

[clipboard]
enabled = true
mode = "confirm"
max_bytes = 16777216

[screen]
enabled = true
require_confirm_each_session = true
max_fps = 2
max_bytes = 16777216

[sync]
enabled = false
folder = ""
mode = "send_receive"
ignore = "*.tmp,~$*,.git/,node_modules/,.linkbridge/"
max_file_size_mb = 4096
rescan_interval_sec = 3600
default_conflict_policy = "keep_both"
max_concurrent_files = 4
trash_enabled = true
version_keep_count = 5
version_keep_days = 30
trash_max_mb = 2048
rate_limit_kbps = 0
)";
}

Result validate_config(const AppConfig& cfg) {
    auto port_ok = [](int p) { return p > 0 && p < 65536; };
    if (!port_ok(cfg.network.pair_port)) return Result::fail("network.pair_port 非法");
    if (!port_ok(cfg.network.heartbeat_port)) return Result::fail("network.heartbeat_port 非法");
    if (!port_ok(cfg.network.ssh_port)) return Result::fail("network.ssh_port 非法");
    if (!port_ok(cfg.network.transfer_port)) return Result::fail("network.transfer_port 非法");
    if (!port_ok(cfg.network.clipboard_port)) return Result::fail("network.clipboard_port 非法");
    if (!port_ok(cfg.network.screen_port)) return Result::fail("network.screen_port 非法");
    if (!port_ok(cfg.network.sync_port)) return Result::fail("network.sync_port 非法");
    const int ports[] = {cfg.network.pair_port, cfg.network.heartbeat_port, cfg.network.ssh_port,
                         cfg.network.transfer_port, cfg.network.clipboard_port,
                         cfg.network.screen_port, cfg.network.sync_port};
    for (std::size_t i = 0; i < sizeof(ports) / sizeof(ports[0]); ++i)
        for (std::size_t j = i + 1; j < sizeof(ports) / sizeof(ports[0]); ++j)
            if (ports[i] == ports[j]) return Result::fail("七个端口不得重复");

    if (cfg.heartbeat.interval_sec < 5 || cfg.heartbeat.interval_sec > 3600)
        return Result::fail("heartbeat.interval_sec 需在 5..3600");
    if (cfg.heartbeat.max_interval_sec < cfg.heartbeat.interval_sec)
        return Result::fail("heartbeat.max_interval_sec 不得小于 interval_sec");
    if (cfg.heartbeat.max_failures < 1 || cfg.heartbeat.max_failures > 20)
        return Result::fail("heartbeat.max_failures 需在 1..20");
    if (cfg.heartbeat.offline_probe_sec < 5)
        return Result::fail("heartbeat.offline_probe_sec 不得小于 5");
    if (cfg.network.connect_timeout_ms < 500 || cfg.network.connect_timeout_ms > 60000)
        return Result::fail("network.connect_timeout_ms 需在 500..60000");
    if (cfg.transfer.chunk_size < 4096)
        return Result::fail("transfer.chunk_size 不得小于 4096");

    if (cfg.clipboard.mode != "auto" && cfg.clipboard.mode != "confirm")
        return Result::fail("clipboard.mode 仅支持 auto | confirm");
    if (cfg.clipboard.max_bytes < 1024 || cfg.clipboard.max_bytes > 67108864)
        return Result::fail("clipboard.max_bytes 需在 1024..67108864");
    if (cfg.screen.max_fps < 1 || cfg.screen.max_fps > 30)
        return Result::fail("screen.max_fps 需在 1..30");
    if (cfg.screen.max_bytes < 1024 || cfg.screen.max_bytes > 67108864)
        return Result::fail("screen.max_bytes 需在 1024..67108864");

    const auto& sync = cfg.sync;
    if (sync.mode != "send_receive" && sync.mode != "send_only" &&
        sync.mode != "receive_only" && sync.mode != "mirror")
        return Result::fail("sync.mode 仅支持 send_receive | send_only | receive_only | mirror");
    if (sync.default_conflict_policy != "keep_both" && sync.default_conflict_policy != "newer" &&
        sync.default_conflict_policy != "larger")
        return Result::fail("sync.default_conflict_policy 仅支持 keep_both | newer | larger");
    if (sync.max_file_size_mb < 1 || sync.max_file_size_mb > 65536)
        return Result::fail("sync.max_file_size_mb 需在 1..65536");
    if (sync.rescan_interval_sec < 5 || sync.rescan_interval_sec > 86400)
        return Result::fail("sync.rescan_interval_sec 需在 5..86400");
    if (sync.max_concurrent_files < 1 || sync.max_concurrent_files > 32)
        return Result::fail("sync.max_concurrent_files 需在 1..32");
    if (sync.version_keep_count < 0 || sync.version_keep_count > 1000)
        return Result::fail("sync.version_keep_count 需在 0..1000");
    if (sync.version_keep_days < 0 || sync.version_keep_days > 3650)
        return Result::fail("sync.version_keep_days 需在 0..3650");
    if (sync.trash_max_mb < 0 || sync.trash_max_mb > 1048576)
        return Result::fail("sync.trash_max_mb 需在 0..1048576");
    if (sync.rate_limit_kbps < 0 || sync.rate_limit_kbps > 1048576)
        return Result::fail("sync.rate_limit_kbps 需在 0..1048576");
    if (sync.enabled && sync.folder.empty())
        return Result::fail("sync.enabled 为真时必须配置 sync.folder");
    return Result::ok();
}

Expected<AppConfig> load_config(const std::string& path, bool* created_default) {
    AppConfig cfg;
    if (created_default) *created_default = false;

    if (!pal::file_exists(path)) {
        const auto slash = path.find_last_of("/\\");
        if (slash != std::string::npos && slash > 0) {
            if (auto r = pal::ensure_dir(path.substr(0, slash)); !r)
                return Expected<AppConfig>::fail("创建配置目录失败: " + r.error);
        }
        const std::string text = default_config_text();
        ByteBuf data(text.begin(), text.end());
        if (auto r = pal::write_file_atomic(path, data); !r)
            return Expected<AppConfig>::fail("写入默认配置失败: " + r.error);
        if (created_default) *created_default = true;
        cfg.device_name = pal::hostname();
        return Expected<AppConfig>::success(std::move(cfg));
    }

    auto table = TomlTable::parse_file(path);
    if (!table) return Expected<AppConfig>::fail("解析配置失败: " + table.error);
    const TomlTable& t = *table;

    if (auto v = t.get_str("identity", "device_name"); v && !v->empty()) cfg.device_name = *v;
    else cfg.device_name = pal::hostname();

    if (auto v = t.get_int("network", "pair_port")) cfg.network.pair_port = static_cast<int>(*v);
    if (auto v = t.get_int("network", "heartbeat_port"))
        cfg.network.heartbeat_port = static_cast<int>(*v);
    if (auto v = t.get_int("network", "ssh_port")) cfg.network.ssh_port = static_cast<int>(*v);
    if (auto v = t.get_int("network", "transfer_port"))
        cfg.network.transfer_port = static_cast<int>(*v);
    if (auto v = t.get_int("network", "clipboard_port"))
        cfg.network.clipboard_port = static_cast<int>(*v);
    if (auto v = t.get_int("network", "screen_port"))
        cfg.network.screen_port = static_cast<int>(*v);
    if (auto v = t.get_int("network", "sync_port"))
        cfg.network.sync_port = static_cast<int>(*v);
    if (auto v = t.get_str("network", "bind_addr")) cfg.network.bind_addr = *v;
    if (auto v = t.get_int("network", "connect_timeout_ms"))
        cfg.network.connect_timeout_ms = static_cast<int>(*v);

    if (auto v = t.get_int("heartbeat", "interval_sec"))
        cfg.heartbeat.interval_sec = static_cast<int>(*v);
    if (auto v = t.get_int("heartbeat", "max_interval_sec"))
        cfg.heartbeat.max_interval_sec = static_cast<int>(*v);
    if (auto v = t.get_int("heartbeat", "max_failures"))
        cfg.heartbeat.max_failures = static_cast<int>(*v);
    if (auto v = t.get_bool("heartbeat", "adaptive")) cfg.heartbeat.adaptive = *v;
    if (auto v = t.get_int("heartbeat", "offline_probe_sec"))
        cfg.heartbeat.offline_probe_sec = static_cast<int>(*v);

    if (auto v = t.get_int("transfer", "chunk_size"))
        cfg.transfer.chunk_size = static_cast<int>(*v);
    if (auto v = t.get_int("transfer", "max_concurrent_tasks"))
        cfg.transfer.max_concurrent_tasks = static_cast<int>(*v);
    if (auto v = t.get_int("transfer", "rate_limit_kbps"))
        cfg.transfer.rate_limit_kbps = static_cast<int>(*v);

    if (auto v = t.get_bool("clipboard", "enabled")) cfg.clipboard.enabled = *v;
    if (auto v = t.get_str("clipboard", "mode")) cfg.clipboard.mode = *v;
    if (auto v = t.get_int("clipboard", "max_bytes"))
        cfg.clipboard.max_bytes = static_cast<int>(*v);

    if (auto v = t.get_bool("screen", "enabled")) cfg.screen.enabled = *v;
    if (auto v = t.get_bool("screen", "require_confirm_each_session"))
        cfg.screen.require_confirm_each_session = *v;
    if (auto v = t.get_int("screen", "max_fps")) cfg.screen.max_fps = static_cast<int>(*v);
    if (auto v = t.get_int("screen", "max_bytes"))
        cfg.screen.max_bytes = static_cast<int>(*v);

    if (auto v = t.get_bool("sync", "enabled")) cfg.sync.enabled = *v;
    if (auto v = t.get_str("sync", "folder")) cfg.sync.folder = *v;
    if (auto v = t.get_str("sync", "mode")) cfg.sync.mode = *v;
    if (auto v = t.get_str("sync", "ignore")) cfg.sync.ignore = *v;
    if (auto v = t.get_int("sync", "max_file_size_mb"))
        cfg.sync.max_file_size_mb = static_cast<int>(*v);
    if (auto v = t.get_int("sync", "rescan_interval_sec"))
        cfg.sync.rescan_interval_sec = static_cast<int>(*v);
    if (auto v = t.get_str("sync", "default_conflict_policy"))
        cfg.sync.default_conflict_policy = *v;
    if (auto v = t.get_int("sync", "max_concurrent_files"))
        cfg.sync.max_concurrent_files = static_cast<int>(*v);
    if (auto v = t.get_bool("sync", "trash_enabled")) cfg.sync.trash_enabled = *v;
    if (auto v = t.get_int("sync", "version_keep_count"))
        cfg.sync.version_keep_count = static_cast<int>(*v);
    if (auto v = t.get_int("sync", "version_keep_days"))
        cfg.sync.version_keep_days = static_cast<int>(*v);
    if (auto v = t.get_int("sync", "trash_max_mb"))
        cfg.sync.trash_max_mb = static_cast<int>(*v);
    if (auto v = t.get_int("sync", "rate_limit_kbps"))
        cfg.sync.rate_limit_kbps = static_cast<int>(*v);

    if (auto r = validate_config(cfg); !r) return Expected<AppConfig>::fail(r.error);
    return Expected<AppConfig>::success(std::move(cfg));
}

std::string config_summary(const AppConfig& cfg) {
    std::ostringstream os;
    os << "设备名: " << cfg.device_name << "\n"
       << "配对端口: " << cfg.network.pair_port << "  心跳端口: " << cfg.network.heartbeat_port
       << "  SSH 端口: " << cfg.network.ssh_port
       << "  传输端口: " << cfg.network.transfer_port
       << "  剪贴板端口: " << cfg.network.clipboard_port
       << "  屏幕共享端口: " << cfg.network.screen_port
       << "  同步端口: " << cfg.network.sync_port << "\n"
       << "心跳: 基础 " << cfg.heartbeat.interval_sec << "s / 上限 " << cfg.heartbeat.max_interval_sec
       << "s / 失败阈值 " << cfg.heartbeat.max_failures
       << " / 自适应 " << (cfg.heartbeat.adaptive ? "开" : "关");
    if (cfg.sync.enabled) {
        os << "\n同步: 文件夹 " << cfg.sync.folder << " / 模式 " << cfg.sync.mode;
    }
    return os.str();
}

} // namespace lb
