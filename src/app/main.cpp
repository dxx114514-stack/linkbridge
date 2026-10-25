#include "core/config/config.h"
#include "core/node/node.h"
#include "core/pairing/pairing.h"
#include "core/platform/platform.h"

#if defined(_WIN32)
#include <windows.h>
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <ctime>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

std::atomic<bool> g_stop{false};
std::mutex g_console_mu;

std::string format_size(std::uint64_t bytes);

void on_signal(int) { g_stop.store(true); }

bool prompt_confirm(const lb::PeerInfo& peer, const std::string& fingerprint) {
    std::lock_guard<std::mutex> lk(g_console_mu);
    std::printf("\n[配对请求]\n  设备名 : %s\n  设备 ID: %s\n  平台   : %s\n  指纹   : %s\n",
                peer.name.c_str(), peer.device_id.c_str(), peer.platform.c_str(),
                fingerprint.c_str());
    std::printf("请与对端屏幕上的指纹逐位比对，一致才接受。接受配对? (y/N): ");
    std::fflush(stdout);
    std::string line;
    if (!std::getline(std::cin, line)) return false;
    return !line.empty() && (line[0] == 'y' || line[0] == 'Y');
}

bool prompt_clipboard(const lb::ClipboardContent& content, const std::string& from_name) {
    std::lock_guard<std::mutex> lk(g_console_mu);
    std::printf("\n[剪贴板] 来自 %s 的 %s（%s）\n", from_name.c_str(),
                content.mime == "text/plain" ? "文本" : "图片",
                format_size(content.data.size()).c_str());
    if (content.mime == "text/plain") {
        std::string preview(content.data.begin(), content.data.end());
        if (preview.size() > 200) preview = preview.substr(0, 200) + "...";
        std::printf("预览: %s\n", preview.c_str());
    }
    std::printf("接受并覆盖本地剪贴板? (y/N): ");
    std::fflush(stdout);
    std::string line;
    if (!std::getline(std::cin, line)) return false;
    return !line.empty() && (line[0] == 'y' || line[0] == 'Y');
}

bool prompt_screen(const std::string& from_name, const std::string& mode) {
    std::lock_guard<std::mutex> lk(g_console_mu);
    std::printf("\n[屏幕共享请求]\n  设备: %s\n  模式: %s\n", from_name.c_str(),
                mode == "view" ? "只读查看" : "远程控制（可注入触摸与按键）");
    std::printf("接受对方%s你的屏幕? (y/N): ", mode == "view" ? "查看" : "控制");
    std::fflush(stdout);
    std::string line;
    if (!std::getline(std::cin, line)) return false;
    return !line.empty() && (line[0] == 'y' || line[0] == 'Y');
}

void print_banner() {
    std::printf("LinkBridge 连桥 v0.1 (M1-M7: 身份/信任库/配对/心跳/文件互传/剪贴板/屏幕共享/文件夹同步完整版)\n");
    std::printf("------------------------------------------------------------\n");
}

std::string format_utc_time(std::int64_t ts) {
    if (ts <= 0) return "-";
    const std::time_t t = static_cast<std::time_t>(ts);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d", tm.tm_year + 1900,
                  tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

std::string format_size(std::uint64_t bytes) {
    char buf[64];
    if (bytes >= 1024ull * 1024 * 1024)
        std::snprintf(buf, sizeof(buf), "%.2f GiB", static_cast<double>(bytes) / (1024.0 * 1024 * 1024));
    else if (bytes >= 1024ull * 1024)
        std::snprintf(buf, sizeof(buf), "%.2f MiB", static_cast<double>(bytes) / (1024.0 * 1024));
    else if (bytes >= 1024)
        std::snprintf(buf, sizeof(buf), "%.1f KiB", static_cast<double>(bytes) / 1024.0);
    else
        std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(bytes));
    return buf;
}

void print_usage() {
    std::printf(
        "用法: linkbridge [--data-dir DIR] [--config FILE] [--name NAME] [--auto-accept] "
        "[--duration N] <命令>\n"
        "\n命令:\n"
        "  init              初始化配置与设备身份\n"
        "  show              显示本机身份、配置与受信任设备\n"
        "  devices           列出受信任设备\n"
        "  code              生成配对码并等待对端配对（默认 300 秒）\n"
        "  pair <ip> <码>     向指定设备发起配对\n"
        "  send <设备> <文件> [远端名]  向受信任设备发送文件\n"
        "  clip status       显示剪贴板同步配置与当前内容摘要\n"
        "  clip get          读取本地剪贴板\n"
        "  clip push <设备|all> <文本>  推送文本到指定设备（all=全部在线设备）\n"
        "  screen view <设备> [输出目录]  查看受信任设备屏幕（只读，最新帧存为 BMP）\n"
        "  screen status     显示屏幕共享配置与通道端口\n"
        "  sync status       显示文件夹同步配置与索引状态\n"
        "  sync once <设备|all>  立即与指定受信任设备执行一次同步（all=全部可同步设备）\n"
        "  sync conflicts    列出未解决的同步冲突（原文件与冲突副本）\n"
        "  sync resolve <路径> <keep_local|keep_remote|keep_both>  解决指定冲突\n"
        "  sync history [路径]   列出回收站中的历史版本\n"
        "  sync restore <路径> [版本ID]  恢复历史版本（省略版本ID 取最新，当前文件先归档）\n"
        "  daemon            启动监听/心跳/剪贴板/屏幕共享/文件夹同步，持续运行\n"
        "\n参数:\n"
        "  --data-dir DIR    数据目录（默认 %%LOCALAPPDATA%%\\LinkBridge）\n"
        "  --config FILE     配置文件（默认 <数据目录>\\linkbridge.toml）\n"
        "  --name NAME       覆盖设备名\n"
        "  --auto-accept     跳过人工确认（仅建议测试使用）\n"
        "  --duration N      运行/等待秒数\n");
}

std::optional<lb::TrustRecord> resolve_peer(lb::Node& node, const std::string& key) {
    const auto records = node.trust().all();
    for (const auto& r : records)
        if (r.device_id == key) return r;

    std::string lower = key;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (const auto& r : records) {
        std::string name = r.name;
        std::transform(name.begin(), name.end(), name.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (name == lower) return r;
    }
    return std::nullopt;
}

void print_devices(const lb::Node& node, bool live) {
    const auto devices = node.devices();
    if (devices.empty()) {
        std::printf("尚未添加受信任设备。使用 `code` + `pair` 完成首次配对。\n");
        return;
    }
    std::printf("%-18s %-14s %-10s %-18s %s\n", "设备名", "设备 ID", "平台", "状态", "最近 IP");
    for (const auto& d : devices) {
        const char* presence = d.presence == lb::Presence::Online
                                   ? "在线"
                                   : (d.presence == lb::Presence::Offline ? "离线" : "未知");
        std::printf("%-18s %-14s %-10s %-18s %s\n", d.name.c_str(), d.device_id.c_str(),
                    d.platform.c_str(), live ? presence : "需 daemon 实时判定",
                    d.last_ip.empty() ? "-" : d.last_ip.c_str());
    }
}

void print_trust_only(lb::Node& node) {
    const auto records = node.trust().all();
    if (records.empty()) {
        std::printf("尚未添加受信任设备。\n");
        return;
    }
    std::printf("%-18s %-14s %-10s %-20s %s\n", "设备名", "设备 ID", "平台", "最近 IP", "配对时间");
    for (const auto& r : records) {
        std::printf("%-18s %-14s %-10s %-20s %lld\n", r.name.c_str(), r.device_id.c_str(),
                    lb::platform_name(r.platform), r.last_ip.empty() ? "-" : r.last_ip.c_str(),
                    static_cast<long long>(r.paired_at));
    }
}

void wait_until(int seconds) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (!g_stop.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

std::vector<lb::Subscription> attach_event_logging(lb::Node& node) {
    // Subscription 析构即退订；调用方需持有返回值，且确保其先于 Node 销毁。
    std::vector<lb::Subscription> subs;
    subs.push_back(node.events().subscribe<lb::PairingCompleted>(
        [](const lb::PairingCompleted& ev) {
            std::lock_guard<std::mutex> lk(g_console_mu);
            std::printf("\n[配对成功] %s (%s)  指纹 %s\n", ev.peer.name.c_str(),
                        ev.peer.device_id.c_str(), ev.fingerprint.c_str());
        }));
    subs.push_back(node.events().subscribe<lb::PairingStateChanged>(
        [](const lb::PairingStateChanged& ev) {
            if (ev.state != lb::PairState::Failed) return;
            std::lock_guard<std::mutex> lk(g_console_mu);
            std::printf("\n[配对失败] %s\n", ev.message.c_str());
        }));
    subs.push_back(node.events().subscribe<lb::DeviceOnline>([](const lb::DeviceOnline& ev) {
        std::lock_guard<std::mutex> lk(g_console_mu);
        std::printf("\n[上线] %s (%s)\n", ev.name.c_str(), ev.device_id.c_str());
    }));
    subs.push_back(node.events().subscribe<lb::DeviceOffline>([](const lb::DeviceOffline& ev) {
        std::lock_guard<std::mutex> lk(g_console_mu);
        std::printf("\n[离线] %s (%s)\n", ev.name.c_str(), ev.device_id.c_str());
    }));
    subs.push_back(node.events().subscribe<lb::TransferProgress>(
        [](const lb::TransferProgress& ev) {
            std::lock_guard<std::mutex> lk(g_console_mu);
            static std::map<std::string, int> last_pct;
            const int pct = ev.total == 0
                                ? 100
                                : static_cast<int>(ev.transferred * 100 / ev.total);
            auto it = last_pct.find(ev.task_id);
            if (it != last_pct.end() && pct < 100 && pct - it->second < 10) return;
            last_pct[ev.task_id] = pct;
            if (pct >= 100) last_pct.erase(ev.task_id);
            std::printf("\n[%s] %s  %d%%  %s / %s", ev.outgoing ? "发送" : "接收",
                        ev.file_name.c_str(), pct, format_size(ev.transferred).c_str(),
                        format_size(ev.total).c_str());
            std::fflush(stdout);
        }));
    subs.push_back(node.events().subscribe<lb::TransferCompleted>(
        [](const lb::TransferCompleted& ev) {
            std::lock_guard<std::mutex> lk(g_console_mu);
            std::printf("\n[%s完成] %s  %s  → %s\n", ev.outgoing ? "发送" : "接收",
                        ev.file_name.c_str(), format_size(ev.size).c_str(), ev.path.c_str());
        }));
    subs.push_back(node.events().subscribe<lb::ClipboardReceived>(
        [](const lb::ClipboardReceived& ev) {
            std::lock_guard<std::mutex> lk(g_console_mu);
            if (ev.applied) {
                std::printf("\n[剪贴板] 已同步来自 %s 的%s（%s）\n", ev.from_name.c_str(),
                            ev.mime == "text/plain" ? "文本" : "图片",
                            format_size(ev.size).c_str());
            } else {
                std::printf("\n[剪贴板] 拒绝来自 %s 的内容: %s\n", ev.from_name.c_str(),
                            ev.reason.c_str());
            }
        }));
    subs.push_back(node.events().subscribe<lb::ClipboardSent>([](const lb::ClipboardSent& ev) {
        std::lock_guard<std::mutex> lk(g_console_mu);
        std::printf("\n[剪贴板] 已推送到 %s（%s，%s）\n", ev.to_name.c_str(),
                    ev.mime == "text/plain" ? "文本" : "图片",
                    format_size(ev.size).c_str());
    }));
    subs.push_back(node.events().subscribe<lb::ClipboardFailed>(
        [](const lb::ClipboardFailed& ev) {
            std::lock_guard<std::mutex> lk(g_console_mu);
            std::printf("\n[剪贴板失败] %s: %s\n", ev.peer_name.c_str(), ev.reason.c_str());
        }));
    subs.push_back(node.events().subscribe<lb::ClipboardSkipped>(
        [](const lb::ClipboardSkipped& ev) {
            std::lock_guard<std::mutex> lk(g_console_mu);
            std::printf("\n[剪贴板跳过] %s（%s）: %s\n",
                        ev.mime == "text/plain" ? "文本" : "图片",
                        format_size(ev.size).c_str(), ev.reason.c_str());
        }));
    subs.push_back(node.events().subscribe<lb::ScreenSessionStarted>(
        [](const lb::ScreenSessionStarted& ev) {
            std::lock_guard<std::mutex> lk(g_console_mu);
            std::printf("\n[屏幕共享] %s 正在查看你的屏幕（只读）。\n", ev.peer_name.c_str());
        }));
    subs.push_back(node.events().subscribe<lb::ScreenSessionEnded>(
        [](const lb::ScreenSessionEnded& ev) {
            std::lock_guard<std::mutex> lk(g_console_mu);
            std::printf("\n[屏幕共享结束] %s: %s，共发送 %llu 帧。\n", ev.peer_name.c_str(),
                        ev.reason.c_str(), static_cast<unsigned long long>(ev.frames));
        }));
    subs.push_back(node.events().subscribe<lb::ScreenFailed>([](const lb::ScreenFailed& ev) {
        std::lock_guard<std::mutex> lk(g_console_mu);
        std::printf("\n[屏幕共享失败] %s: %s\n", ev.peer_name.c_str(), ev.reason.c_str());
    }));
    subs.push_back(node.events().subscribe<lb::SyncStarted>([](const lb::SyncStarted& ev) {
        std::lock_guard<std::mutex> lk(g_console_mu);
        std::printf("\n[同步] 与 %s (%s) 开始同步...\n", ev.peer_name.c_str(), ev.peer.c_str());
    }));
    subs.push_back(node.events().subscribe<lb::SyncCompleted>([](const lb::SyncCompleted& ev) {
        std::lock_guard<std::mutex> lk(g_console_mu);
        std::printf("\n[同步完成] %s：发出 %llu 个、接收 %llu 个、冲突 %llu 个，共 %s\n",
                    ev.peer_name.c_str(), static_cast<unsigned long long>(ev.stats.pushed),
                    static_cast<unsigned long long>(ev.stats.pulled),
                    static_cast<unsigned long long>(ev.stats.conflicts),
                    format_size(ev.stats.bytes).c_str());
        if (ev.stats.conflicts > 0)
            std::printf("           冲突文件已双方保留，对端副本以 \"(conflict ...)\" 命名。\n");
    }));
    subs.push_back(node.events().subscribe<lb::SyncFailed>([](const lb::SyncFailed& ev) {
        std::lock_guard<std::mutex> lk(g_console_mu);
        std::printf("\n[同步失败] %s: %s\n",
                    ev.peer_name.empty() ? ev.peer.c_str() : ev.peer_name.c_str(),
                    ev.reason.c_str());
    }));
    return subs;
}

} // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif
    std::signal(SIGINT, on_signal);

    std::string data_dir;
    std::string config_path;
    std::string device_name;
    bool auto_accept = false;
    int duration = 300;
    std::vector<std::string> positional;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto need_value = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                std::printf("参数 %s 缺少取值\n", name);
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--data-dir") data_dir = need_value("--data-dir");
        else if (arg == "--config") config_path = need_value("--config");
        else if (arg == "--name") device_name = need_value("--name");
        else if (arg == "--auto-accept") auto_accept = true;
        else if (arg == "--duration") duration = std::stoi(need_value("--duration"));
        else if (arg == "-h" || arg == "--help") {
            print_usage();
            return 0;
        } else positional.push_back(arg);
    }

    if (positional.empty()) {
        print_usage();
        return 2;
    }
    const std::string command = positional[0];

    if (data_dir.empty()) data_dir = lb::pal::data_dir();
    if (config_path.empty()) config_path = lb::pal::join_path(data_dir, "linkbridge.toml");

    bool created_default = false;
    auto cfg = lb::load_config(config_path, &created_default);
    if (!cfg) {
        std::printf("配置错误: %s\n", cfg.error.c_str());
        return 1;
    }
    if (created_default) std::printf("已生成默认配置: %s\n", config_path.c_str());
    if (!device_name.empty()) cfg->device_name = device_name;

    lb::NodeOptions options;
    options.data_dir = data_dir;
    options.config = *cfg;
    options.device_name = cfg->device_name;

    auto node_result = lb::Node::create(options);
    if (!node_result) {
        std::printf("初始化失败: %s\n", node_result.error.c_str());
        return 1;
    }
    std::unique_ptr<lb::Node> node = std::move(*node_result);
    // 事件订阅句柄必须活到 Node 之后销毁（声明在 node 之后即可保证）。
    auto event_subs = attach_event_logging(*node);
    (void)event_subs;

    if (auto_accept) {
        node->pairing().set_auto_accept(true);
        node->clipboard().set_auto_accept(true);
        node->screen().set_auto_accept(true);
    } else {
        node->pairing().set_confirm_handler(prompt_confirm);
        node->clipboard().set_confirm_handler(prompt_clipboard);
        node->screen().set_confirm_handler(prompt_screen);
    }

    print_banner();

    if (command == "init") {
        std::printf("%s\n", node->identity_summary().c_str());
        std::printf("\n%s\n", lb::config_summary(node->config()).c_str());
        std::printf("\n初始化完成。\n");
        return 0;
    }

    if (command == "show") {
        std::printf("%s\n", node->identity_summary().c_str());
        std::printf("\n%s\n\n", lb::config_summary(node->config()).c_str());
        print_trust_only(*node);
        return 0;
    }

    if (command == "devices") {
        print_trust_only(*node);
        return 0;
    }

    if (command == "pair") {
        if (positional.size() < 3) {
            std::printf("用法: linkbridge pair <ip> <配对码>\n");
            return 2;
        }
        const std::string ip = positional[1];
        const std::string code = positional[2];
        std::printf("正在向 %s:%d 发起配对...\n", ip.c_str(), node->config().network.pair_port);
        const lb::Result r = node->pairing().pair_with(ip, node->config().network.pair_port, code);
        if (!r) {
            std::printf("配对失败: %s\n", r.error.c_str());
            return 1;
        }
        std::printf("配对成功，对方已加入受信任设备。\n");
        print_trust_only(*node);
        return 0;
    }

    if (command == "send") {
        if (positional.size() < 3) {
            std::printf("用法: linkbridge send <设备名|设备ID> <本地文件> [远端文件名]\n");
            return 2;
        }
        const std::string target = positional[1];
        const std::string local_path = positional[2];
        const std::string remote_name = positional.size() > 3 ? positional[3] : "";
        const auto peer = resolve_peer(*node, target);
        if (!peer) {
            std::printf("未找到受信任设备: %s（可用 devices 查看）\n", target.c_str());
            return 1;
        }
        std::printf("正在向 %s (%s) 发送 %s ...\n", peer->name.c_str(), peer->device_id.c_str(),
                    local_path.c_str());
        std::fflush(stdout);
        const lb::Result r =
            node->transfer().send_file(peer->device_id, local_path, remote_name,
                                       node->config().network.connect_timeout_ms);
        if (!r) {
            std::printf("\n发送失败: %s\n", r.error.c_str());
            return 1;
        }
        std::printf("\n发送完成。\n");
        return 0;
    }

    if (command == "clip") {
        if (positional.size() < 2) {
            std::printf("用法: linkbridge clip <status|get|push>\n");
            return 2;
        }
        const std::string sub = positional[1];
        if (sub == "status" || sub == "get") {
            const auto& cc = node->config().clipboard;
            if (sub == "status") {
                std::printf("剪贴板同步: %s  模式: %s  上限: %s  通道端口: %d\n",
                            cc.enabled ? "启用" : "停用", cc.mode.c_str(),
                            format_size(static_cast<std::uint64_t>(cc.max_bytes)).c_str(),
                            node->clipboard_port());
            }
            auto content = node->clipboard().read_local();
            if (!content) {
                std::printf("读取本地剪贴板失败: %s\n", content.error.c_str());
                return 1;
            }
            if (content->empty()) {
                std::printf("当前剪贴板为空，或为不支持的类型（仅文本与图片）。\n");
                return 0;
            }
            if (content->mime == "text/plain") {
                const std::string text(content->data.begin(), content->data.end());
                std::printf("%s\n", text.c_str());
            } else {
                std::printf("[图片] DIB 数据 %s\n", format_size(content->data.size()).c_str());
            }
            return 0;
        }
        if (sub == "push") {
            if (positional.size() < 4) {
                std::printf("用法: linkbridge clip push <设备名|设备ID|all> <文本>\n");
                return 2;
            }
            const std::string target = positional[2];
            const std::string text = positional[3];
            lb::ClipboardContent content{"text/plain",
                                         lb::ByteBuf(text.begin(), text.end())};
            const int timeout = node->config().network.connect_timeout_ms;
            if (target == "all") {
                const auto records = node->trust().all();
                int ok_count = 0;
                for (const auto& r : records) {
                    if ((r.permissions & lb::kPermClipboard) == 0 || r.last_ip.empty()) continue;
                    if (node->clipboard().push_to(r.device_id, content, timeout)) ++ok_count;
                }
                std::printf("已推送到 %d 台设备。\n", ok_count);
                return ok_count > 0 ? 0 : 1;
            }
            const auto peer = resolve_peer(*node, target);
            if (!peer) {
                std::printf("未找到受信任设备: %s\n", target.c_str());
                return 1;
            }
            if (const auto r = node->clipboard().push_to(peer->device_id, content, timeout); !r) {
                std::printf("推送失败: %s\n", r.error.c_str());
                return 1;
            }
            std::printf("已推送到 %s。\n", peer->name.c_str());
            return 0;
        }
        std::printf("未知 clip 子命令: %s\n", sub.c_str());
        return 2;
    }

    if (command == "screen") {
        if (positional.size() < 2) {
            std::printf("用法: linkbridge screen <status|view>\n");
            return 2;
        }
        const std::string sub = positional[1];
        const auto& sc = node->config().screen;
        if (sub == "status") {
            std::printf("屏幕共享: %s  每会话确认: %s  帧率: %d fps  单帧上限: %s  通道端口: %d\n",
                        sc.enabled ? "启用" : "停用",
                        sc.require_confirm_each_session ? "是" : "否", sc.max_fps,
                        format_size(static_cast<std::uint64_t>(sc.max_bytes)).c_str(),
                        node->screen_port());
            return 0;
        }
        if (sub == "view") {
            if (positional.size() < 3) {
                std::printf("用法: linkbridge screen view <设备名|设备ID> [输出目录]\n");
                return 2;
            }
            const auto peer = resolve_peer(*node, positional[2]);
            if (!peer) {
                std::printf("未找到受信任设备: %s\n", positional[2].c_str());
                return 1;
            }
            const std::string out_dir =
                positional.size() > 3 ? positional[3]
                                      : lb::pal::join_path(data_dir, "screen");
            std::printf("正在向 %s (%s) 请求只读屏幕查看，最长 %d 秒...\n",
                        peer->name.c_str(), peer->device_id.c_str(), duration);
            std::fflush(stdout);
            auto result = node->screen().view(peer->device_id, out_dir, duration * 1000);
            if (!result) {
                std::printf("\n查看失败: %s\n", result.error.c_str());
                return 1;
            }
            std::printf("\n共接收 %llu 帧，累计 %s，最新帧: %s\n",
                        static_cast<unsigned long long>(result->frames),
                        format_size(result->bytes).c_str(), result->last_frame_path.c_str());
            return result->frames > 0 ? 0 : 1;
        }
        std::printf("未知 screen 子命令: %s\n", sub.c_str());
        return 2;
    }

    if (command == "sync") {
        if (positional.size() < 2) {
            std::printf("用法: linkbridge sync <status|once>\n");
            return 2;
        }
        const std::string sub = positional[1];
        const auto& sc = node->config().sync;
        if (sub == "status") {
            std::printf("文件夹同步: %s  模式: %s  通道端口: %d\n",
                        sc.enabled ? "启用" : "停用", sc.mode.c_str(), node->sync_port());
            std::printf("同步文件夹: %s\n", sc.folder.empty() ? "(未配置)" : sc.folder.c_str());
            std::printf("扫描间隔: %d 秒  单文件上限: %s  冲突策略: %s\n",
                        sc.rescan_interval_sec,
                        format_size(static_cast<std::uint64_t>(sc.max_file_size_mb) * 1024 * 1024)
                            .c_str(),
                        sc.default_conflict_policy.c_str());
            std::printf("回收站: %s  保留版本: %d 个 / %d 天 / %s\n",
                        sc.trash_enabled ? "启用" : "停用", sc.version_keep_count,
                        sc.version_keep_days,
                        format_size(static_cast<std::uint64_t>(sc.trash_max_mb) * 1024 * 1024)
                            .c_str());
            std::printf("限速: %s\n",
                        sc.rate_limit_kbps > 0
                            ? (std::to_string(sc.rate_limit_kbps) + " KiB/s").c_str()
                            : "不限速");
            std::printf("忽略规则: %s\n", sc.ignore.c_str());
            std::printf("索引条目: %zu（%s）\n", node->sync().index_size(),
                        sc.folder.empty()
                            ? "未配置文件夹"
                            : (lb::pal::file_exists(sc.folder) ? "文件夹存在" : "文件夹不存在"));
            const auto pending = node->sync().conflicts();
            std::printf("未解决冲突: %zu\n", pending.size());
            return 0;
        }
        if (sub == "conflicts") {
            const auto list = node->sync().conflicts();
            if (list.empty()) {
                std::printf("没有未解决的同步冲突。\n");
                return 0;
            }
            for (const auto& c : list) {
                std::printf("冲突: %s\n", c.path.c_str());
                for (const auto& copy : c.copies) std::printf("  副本: %s\n", copy.c_str());
            }
            std::printf("\n用 `sync resolve <路径> <keep_local|keep_remote|keep_both>` 处理。\n");
            return 0;
        }
        if (sub == "resolve") {
            if (positional.size() < 4) {
                std::printf("用法: linkbridge sync resolve <路径> <keep_local|keep_remote|keep_both>\n");
                return 2;
            }
            const lb::Result res = node->sync().resolve_conflict(positional[2], positional[3]);
            if (!res) {
                std::printf("解决冲突失败: %s\n", res.error.c_str());
                return 1;
            }
            std::printf("冲突已解决: %s（%s）\n", positional[2].c_str(), positional[3].c_str());
            return 0;
        }
        if (sub == "history") {
            const std::string path = positional.size() >= 3 ? positional[2] : "";
            const auto list = node->sync().history(path);
            if (list.empty()) {
                std::printf("回收站为空%s。\n", path.empty() ? "" : ("（路径: " + path + "）").c_str());
                return 0;
            }
            std::printf("%-30s %-24s %-10s %s\n", "版本ID", "时间(UTC)", "大小", "路径");
            for (const auto& v : list)
                std::printf("%-30s %-24s %-10s %s\n", v.id.c_str(),
                            format_utc_time(v.archived_at).c_str(),
                            format_size(v.size).c_str(), v.path.c_str());
            return 0;
        }
        if (sub == "restore") {
            if (positional.size() < 3) {
                std::printf("用法: linkbridge sync restore <路径> [版本ID]\n");
                return 2;
            }
            const std::string version_id = positional.size() >= 4 ? positional[3] : "";
            const lb::Result res = node->sync().restore_version(positional[2], version_id);
            if (!res) {
                std::printf("恢复失败: %s\n", res.error.c_str());
                return 1;
            }
            std::printf("已恢复 %s%s；下一次同步会把恢复后的内容传播给对端。\n",
                        positional[2].c_str(),
                        version_id.empty() ? "（最新版本）" : ("（版本 " + version_id + "）").c_str());
            return 0;
        }
        if (sub == "once") {
            if (positional.size() < 3) {
                std::printf("用法: linkbridge sync once <设备名|设备ID|all>\n");
                return 2;
            }
            if (!sc.enabled) {
                std::printf("文件夹同步未启用（请在配置中设置 [sync].enabled = true 与 folder）。\n");
                return 1;
            }
            const std::string target = positional[2];
            const int timeout = node->config().network.connect_timeout_ms;
            std::vector<lb::TrustRecord> targets;
            if (target == "all") {
                for (const auto& r : node->trust().all())
                    if ((r.permissions & lb::kPermSync) != 0 && !r.last_ip.empty())
                        targets.push_back(r);
                if (targets.empty()) {
                    std::printf("没有可同步的设备（需已配对、授予同步权限并学习到 IP，"
                                "请先运行 daemon）。\n");
                    return 1;
                }
            } else {
                const auto peer = resolve_peer(*node, target);
                if (!peer) {
                    std::printf("未找到受信任设备: %s\n", target.c_str());
                    return 1;
                }
                targets.push_back(*peer);
            }

            int ok_count = 0;
            for (const auto& r : targets) {
                std::printf("正在与 %s (%s) 同步...\n", r.name.c_str(), r.device_id.c_str());
                std::fflush(stdout);
                lb::SyncStats stats;
                const lb::Result res = node->sync().sync_with(r.device_id, timeout, &stats);
                if (!res) {
                    std::printf("同步失败: %s\n", res.error.c_str());
                    continue;
                }
                std::printf("同步完成：发出 %llu 个、接收 %llu 个、删除 %llu 个、冲突 %llu 个，共 %s\n",
                            static_cast<unsigned long long>(stats.pushed),
                            static_cast<unsigned long long>(stats.pulled),
                            static_cast<unsigned long long>(stats.deleted),
                            static_cast<unsigned long long>(stats.conflicts),
                            format_size(stats.bytes).c_str());
                ++ok_count;
            }
            return ok_count > 0 ? 0 : 1;
        }
        std::printf("未知 sync 子命令: %s\n", sub.c_str());
        return 2;
    }

    if (command == "code") {
        const lb::PairCode code = node->pairing().generate_code(300, 3);
        std::printf("配对码: %s\n", code.value.c_str());
        std::printf("有效期 %d 秒，单次有效（最多 3 次尝试）。请在对端执行:\n",
                    300);
        std::printf("    linkbridge --data-dir <%s> pair <本机IP> %s\n", data_dir.c_str(),
                    code.value.c_str());
        std::printf("本机 IP 可用 `ipconfig` 查看；等待对端连接中...\n\n");
        std::fflush(stdout);

        if (auto r = node->start(); !r) {
            std::printf("启动监听失败: %s\n", r.error.c_str());
            return 1;
        }

        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(duration);
        while (!g_stop.load() && std::chrono::steady_clock::now() < deadline) {
            if (node->pairing().state() == lb::PairState::Done) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        const lb::PairState final_state = node->pairing().state();
        node->stop();
        if (final_state == lb::PairState::Done) {
            std::printf("\n配对完成。\n");
            print_trust_only(*node);
            return 0;
        }
        std::printf("\n未完成配对（超时或取消）：%s\n", node->pairing().last_error().c_str());
        return 1;
    }

    if (command == "daemon") {
        if (auto r = node->start(); !r) {
            std::printf("启动失败: %s\n", r.error.c_str());
            return 1;
        }
        std::printf("服务已启动；配对端口 %d，心跳端口 %d，传输端口 %d，剪贴板端口 %d，"
                    "屏幕共享端口 %d，文件夹同步端口 %d。\n",
                    node->pair_port(), node->heartbeat_port(), node->transfer_port(),
                    node->clipboard_port(), node->screen_port(), node->sync_port());
        std::printf("心跳间隔 %d 秒；剪贴板同步 %s（模式 %s）；屏幕共享 %s；文件夹同步 %s。\n",
                    node->config().heartbeat.interval_sec,
                    node->config().clipboard.enabled ? "启用" : "停用",
                    node->config().clipboard.mode.c_str(),
                    node->config().screen.enabled ? "启用（查看/控制）" : "停用",
                    node->config().sync.enabled ? "启用" : "停用");
        if (node->config().sync.enabled) {
            std::printf("同步文件夹: %s（模式 %s，每 %d 秒全量扫描）\n",
                        node->config().sync.folder.c_str(), node->config().sync.mode.c_str(),
                        node->config().sync.rescan_interval_sec);
        }
        std::printf("接收目录: %s\n", node->transfer().inbox_dir().c_str());
        std::printf("按 Ctrl+C 退出。\n\n");

        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(duration);
        std::int64_t last_report = 0;
        while (!g_stop.load() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            const std::int64_t now = lb::now_secs();
            if (now - last_report >= 10) {
                last_report = now;
                std::lock_guard<std::mutex> lk(g_console_mu);
                print_devices(*node, true);
                std::printf("  [心跳统计] 成功 %llu 次 / 失败 %llu 次\n\n",
                            static_cast<unsigned long long>(node->heartbeat().probes_ok()),
                            static_cast<unsigned long long>(node->heartbeat().probes_failed()));
            }
        }
        node->stop();
        std::printf("\n服务已停止。\n");
        return 0;
    }

    std::printf("未知命令: %s\n", command.c_str());
    print_usage();
    return 2;
}
