// GUI 入口：wxApp 装配 Controller 与主窗口，并支持无头截图验证。
#include "gui/gui_controller.h"
#include "gui/gui_frame.h"
#include "gui/gui_util.h"

#include <wx/image.h>
#include <wx/timer.h>
#include <wx/wx.h>

#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#ifdef __WXMSW__
#include <psapi.h>
#endif

namespace {

struct LaunchOptions {
    std::string data_dir;
    std::string config_path;
    std::string device_name;
    std::string screenshot;
    int page = 0;
    bool no_start = false;
    bool auto_accept = false;
    int exit_after_ms = 0;
    int cycles = 1;
    bool show_help = false;
};

int page_index(const wxString& name) {
    const struct {
        const char* key;
        int index;
    } kPages[] = {{"overview", 0},  {"status", 0},  {"devices", 1},  {"pair", 2},
                  {"transfer", 3},  {"clipboard", 4}, {"screen", 5}, {"sync", 6},
                  {"settings", 7},  {"help", 8}};
    for (const auto& p : kPages)
        if (name.IsSameAs(wxString::FromUTF8(p.key), false)) return p.index;
    long value = 0;
    if (name.ToLong(&value) && value >= 0 && value <= 8) return static_cast<int>(value);
    return 0;
}

const char* kUsage =
    "LinkBridge 连桥 图形界面\n\n"
    "用法：linkbridge-gui [选项]\n\n"
    "  --data-dir <目录>      指定数据目录（默认：用户数据目录）\n"
    "  --config <文件>        指定 TOML 配置文件（默认：数据目录/linkbridge.toml）\n"
    "  --name <名称>          覆盖本机设备名（仅本次运行）\n"
    "  --page <页>            启动时显示的页面：overview/devices/pair/transfer/\n"
    "                         clipboard/screen/sync/settings/help 或 0-8\n"
    "  --no-start             启动时不自动启动服务\n"
    "  --auto-accept          自动接受配对/剪贴板/屏幕请求（测试用）\n"
    "  --screenshot <png>     渲染完成后截取窗口区域并保存为 PNG，随后退出\n"
    "  --wait-ms <毫秒>       截图前等待时间（默认 1200）\n"
    "  --exit-after <毫秒>    指定时间后自动退出（用于冒烟测试）\n"
    "  --cycle <次数>         重复创建/销毁主窗口指定次数（句柄与内存泄漏检查）\n"
    "  --help                 显示本说明\n";

} // namespace

namespace {

// 资源统计：GDI/USER 句柄数 + 进程私有内存，用于重复开关窗口的泄漏检查
void log_resources(const char* tag) {
#ifdef __WXMSW__
    PROCESS_MEMORY_COUNTERS pmc{};
    ::GetProcessMemoryInfo(::GetCurrentProcess(), &pmc, sizeof(pmc));
    std::fprintf(stderr, "linkbridge-gui: [%s] gdi=%lu user=%lu private_kb=%lu\n", tag,
                 static_cast<unsigned long>(::GetGuiResources(::GetCurrentProcess(), GR_GDIOBJECTS)),
                 static_cast<unsigned long>(::GetGuiResources(::GetCurrentProcess(), GR_USEROBJECTS)),
                 static_cast<unsigned long>(pmc.PagefileUsage / 1024));
#else
    (void)tag;
#endif
}

} // namespace

class GuiApp : public wxApp {
public:
    bool OnInit() override;
    int OnRun() override;
    int OnExit() override;

private:
    bool parse_args(LaunchOptions* opt);
    void schedule_shutdown();

    std::unique_ptr<lb::gui::Controller> ctl_;
    lb::gui::MainFrame* frame_ = nullptr;
    wxTimer shot_timer_;
    std::string screenshot_path_;
    bool shot_requested_ = false;
    bool shot_ok_ = true;
    bool shot_done_ = false;
    bool raised_ = false;
    int cycles_left_ = 1;
    int cycle_page_ = 0;
    int cycle_delay_ms_ = 1200;
    bool auto_start_ = true;
    bool resources_logged_ = false;
};

bool GuiApp::parse_args(LaunchOptions* opt) {
    wxInitAllImageHandlers();
    std::vector<wxString> args;
    for (int i = 1; i < argc; ++i) args.push_back(argv[i]);

    auto value_of = [&](const char* key, wxString* out) -> bool {
        const wxString name = wxString::FromUTF8(key);
        const wxString prefix = name + "=";
        for (std::size_t i = 0; i < args.size(); ++i) {
            if (args[i].StartsWith(prefix)) {
                *out = args[i].Mid(prefix.length());
                return true;
            }
            if (args[i] == name && i + 1 < args.size()) {
                *out = args[i + 1];
                return true;
            }
        }
        return false;
    };
    auto flag_of = [&](const char* key) {
        const wxString name = wxString::FromUTF8(key);
        for (const auto& a : args)
            if (a == name) return true;
        return false;
    };

    wxString value;
    if (flag_of("--help") || flag_of("-h")) {
        opt->show_help = true;
        return true;
    }
    if (value_of("--data-dir", &value)) opt->data_dir = lb::gui::U8(value);
    if (value_of("--config", &value)) opt->config_path = lb::gui::U8(value);
    if (value_of("--name", &value)) opt->device_name = lb::gui::U8(value);
    if (value_of("--screenshot", &value)) {
        opt->screenshot = lb::gui::U8(value);
        opt->exit_after_ms = 1200;
    }
    if (value_of("--wait-ms", &value)) {
        long ms = 0;
        if (value.ToLong(&ms) && ms >= 0) opt->exit_after_ms = static_cast<int>(ms);
    }
    if (value_of("--exit-after", &value)) {
        long ms = 0;
        if (value.ToLong(&ms) && ms > 0) opt->exit_after_ms = static_cast<int>(ms);
    }
    if (value_of("--page", &value)) opt->page = page_index(value);
    if (value_of("--cycle", &value)) {
        long n = 0;
        if (value.ToLong(&n) && n > 0) opt->cycles = static_cast<int>(n);
    }
    opt->no_start = flag_of("--no-start");
    opt->auto_accept = flag_of("--auto-accept");
    return true;
}

bool GuiApp::OnInit() {
    SetAppName("LinkBridge");
    LaunchOptions opt;
    if (!parse_args(&opt)) return false;
    if (opt.show_help) {
        wxMessageBox(lb::gui::W(kUsage), lb::gui::W("LinkBridge 连桥 图形界面"),
                     wxOK | wxICON_INFORMATION);
        return false;
    }

    ctl_ = std::make_unique<lb::gui::Controller>();
    ctl_->set_ui_post([](std::function<void()> fn) {
        if (wxTheApp) wxTheApp->CallAfter(std::move(fn));
    });

    std::string error;
    if (!ctl_->initialize(opt.data_dir, opt.config_path, opt.device_name, &error)) {
        wxMessageBox(lb::gui::W("初始化失败：" + error), lb::gui::W("LinkBridge 连桥"),
                     wxOK | wxICON_ERROR);
        return false;
    }
    if (opt.auto_accept) ctl_->set_auto_accept(true);

    frame_ = new lb::gui::MainFrame(*ctl_);
    frame_->Show();
    frame_->select_page(opt.page);
    if (!opt.no_start) frame_->start_service();

    shot_requested_ = !opt.screenshot.empty();
    screenshot_path_ = opt.screenshot;
    cycles_left_ = opt.cycles;
    cycle_page_ = opt.page;
    cycle_delay_ms_ = opt.exit_after_ms > 0 ? opt.exit_after_ms : 1200;
    auto_start_ = !opt.no_start;
    if (!screenshot_path_.empty() || opt.exit_after_ms > 0 || opt.cycles > 1) {
        shot_timer_.SetOwner(this);
        Bind(wxEVT_TIMER, [this](wxTimerEvent&) { schedule_shutdown(); }, shot_timer_.GetId());
        shot_timer_.StartOnce(cycle_delay_ms_);
    }
    return true;
}

void GuiApp::schedule_shutdown() {
    if (frame_ && !screenshot_path_.empty() && !shot_done_) {
        if (!raised_) {
            // 先置顶并获得焦点，等待系统完成重绘后再抓屏（PrintWindow 失败时的兜底）
            raised_ = true;
            frame_->Show();
            frame_->Raise();
            frame_->SetFocus();
            shot_timer_.StartOnce(600);
            return;
        }
        shot_done_ = true;
        frame_->Update();
        shot_ok_ = frame_->capture_to(screenshot_path_);
        std::fprintf(stderr, "linkbridge-gui: screenshot %s -> %s\n",
                     shot_ok_ ? "ok" : "failed", screenshot_path_.c_str());
    }
    if (!resources_logged_) {
        resources_logged_ = true;
        log_resources("baseline");
    }
    if (frame_) {
        frame_->Close(true);
        frame_ = nullptr;
    }
    if (cycles_left_ > 1) {
        --cycles_left_;
        frame_ = new lb::gui::MainFrame(*ctl_);
        frame_->Show();
        frame_->select_page(cycle_page_);
        if (auto_start_) frame_->start_service();
        shot_done_ = false;
        raised_ = false;
        shot_timer_.StartOnce(cycle_delay_ms_);
        return;
    }
    log_resources("final");
}

int GuiApp::OnRun() {
    const int rc = wxApp::OnRun();
    if (shot_requested_ && !shot_ok_) return 2;
    return rc;
}

int GuiApp::OnExit() {
    if (ctl_) {
        ctl_->set_closing(true);
        ctl_->set_ui_post(nullptr);
        ctl_->set_event_sink(nullptr);
        ctl_->stop();
        ctl_->wait_all_tasks();
    }
    return wxApp::OnExit();
}

wxIMPLEMENT_APP(GuiApp);
