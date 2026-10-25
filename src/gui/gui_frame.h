#pragma once
// 主窗口：左侧导航（wxListbook，使用系统原生列表控件）+ 状态栏 + 菜单。
#include "gui/gui_controller.h"

#include <wx/wx.h>
#include <wx/listbook.h>
#include <wx/timer.h>

namespace lb::gui {

class StatusPanel;
class DevicesPanel;
class PairPanel;
class TransferPanel;
class ClipboardPanel;
class ScreenPanel;
class SyncPanel;
class SettingsPanel;
class HelpPanel;

class MainFrame : public wxFrame {
public:
    explicit MainFrame(Controller& ctl);
    ~MainFrame() override;

    void start_service();
    void stop_service();
    void restart_service();
    bool service_running() const { return ctl_.running(); }

    void append_log(const wxString& text);
    void flash_status(const wxString& text);
    void select_page(int index);
    void open_screen_for(const std::string& device_id);
    // 截图（跨平台）：用于无显示环境下的界面渲染验证
    bool capture_to(const std::string& path);

private:
    void build_ui();
    void build_menu();
    void refresh_ui();
    void dispatch_event(const UiEvent& ev);
    void on_close(wxCloseEvent& ev);

    Controller& ctl_;
    wxListbook* book_ = nullptr;
    StatusPanel* status_ = nullptr;
    DevicesPanel* devices_ = nullptr;
    PairPanel* pair_ = nullptr;
    TransferPanel* transfer_ = nullptr;
    ClipboardPanel* clipboard_ = nullptr;
    ScreenPanel* screen_ = nullptr;
    SyncPanel* sync_ = nullptr;
    SettingsPanel* settings_ = nullptr;
    HelpPanel* help_ = nullptr;
    wxTimer refresh_timer_;
    wxString flash_;
    std::int64_t flash_until_ = 0;
};

} // namespace lb::gui
