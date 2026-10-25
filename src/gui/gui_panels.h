#pragma once
// 各功能面板声明。所有面板只通过 Controller 访问核心，不直接依赖网络/线程细节。
#include <wx/wx.h>
#include <wx/filepicker.h>
#include <wx/gauge.h>
#include <wx/html/htmlwin.h>
#include <wx/listctrl.h>
#include <wx/spinctrl.h>
#include <wx/statbmp.h>
#include <wx/timer.h>

#include <memory>
#include <string>
#include <vector>

#include "gui/gui_controller.h"

class wxListbook;

namespace lb::gui {

class MainFrame;

// 面板基类：统一持有控制器与主窗口引用
class PanelBase : public wxPanel {
public:
    PanelBase(wxWindow* parent, Controller& ctl, MainFrame* frame);
    virtual void refresh(const UiSnapshot& snap) { (void)snap; }
    virtual void on_event(const UiEvent& ev) { (void)ev; }

protected:
    Controller& ctl_;
    MainFrame* frame_;
};

class StatusPanel : public PanelBase {
public:
    StatusPanel(wxWindow* parent, Controller& ctl, MainFrame* frame);
    void refresh(const UiSnapshot& snap) override;
    void on_event(const UiEvent& ev) override;
    void append_log(const wxString& line);

private:
    void build_ui();
    wxStaticText* state_ = nullptr;
    wxStaticText* name_ = nullptr;
    wxStaticText* id_ = nullptr;
    wxStaticText* fp_ = nullptr;
    wxStaticText* ports_ = nullptr;
    wxStaticText* stats_ = nullptr;
    wxStaticText* dirs_ = nullptr;
    wxTextCtrl* log_ = nullptr;
    wxButton* start_btn_ = nullptr;
    wxButton* stop_btn_ = nullptr;
};

class DevicesPanel : public PanelBase {
public:
    DevicesPanel(wxWindow* parent, Controller& ctl, MainFrame* frame);
    void refresh(const UiSnapshot& snap) override;
    void on_event(const UiEvent& ev) override;
    std::string selected_id() const;

private:
    void build_ui();
    void reload_list();
    void on_send();
    void on_push_clip();
    void on_sync();
    void on_screen();
    void on_remove();

    wxListCtrl* list_ = nullptr;
    wxStaticText* hint_ = nullptr;
    std::vector<std::string> row_ids_;
};

class PairPanel : public PanelBase {
public:
    PairPanel(wxWindow* parent, Controller& ctl, MainFrame* frame);
    void refresh(const UiSnapshot& snap) override;
    void on_event(const UiEvent& ev) override;

private:
    void build_ui();
    void on_generate();
    void on_pair();
    void tick_countdown();

    wxStaticText* self_ = nullptr;
    wxStaticText* code_ = nullptr;
    wxStaticText* status_ = nullptr;
    wxTextCtrl* ip_ = nullptr;
    wxTextCtrl* input_code_ = nullptr;
    wxButton* gen_btn_ = nullptr;
    wxButton* pair_btn_ = nullptr;
    wxTimer countdown_;
    std::int64_t expires_at_ = 0;
    wxStaticText* auto_hint_ = nullptr;
};

class TransferPanel : public PanelBase {
public:
    TransferPanel(wxWindow* parent, Controller& ctl, MainFrame* frame);
    void refresh(const UiSnapshot& snap) override;
    void on_event(const UiEvent& ev) override;

private:
    void build_ui();
    void reload_targets();
    void on_pick();
    void on_send();

    wxChoice* target_ = nullptr;
    wxTextCtrl* path_ = nullptr;
    wxTextCtrl* remote_name_ = nullptr;
    wxButton* send_btn_ = nullptr;
    wxGauge* gauge_ = nullptr;
    wxStaticText* progress_ = nullptr;
    wxListCtrl* history_ = nullptr;
    std::vector<std::string> target_ids_;
    int history_rows_ = 0;
};

class ClipboardPanel : public PanelBase {
public:
    ClipboardPanel(wxWindow* parent, Controller& ctl, MainFrame* frame);
    void refresh(const UiSnapshot& snap) override;
    void on_event(const UiEvent& ev) override;

private:
    void build_ui();
    void reload_targets();
    void on_read();
    void on_push();

    wxChoice* target_ = nullptr;
    wxStaticText* state_ = nullptr;
    wxTextCtrl* local_preview_ = nullptr;
    wxTextCtrl* push_text_ = nullptr;
    wxStaticText* result_ = nullptr;
    std::vector<std::string> target_ids_;
};

class ScreenPanel : public PanelBase {
public:
    ScreenPanel(wxWindow* parent, Controller& ctl, MainFrame* frame);
    void refresh(const UiSnapshot& snap) override;
    void on_event(const UiEvent& ev) override;
    void set_target(const std::string& device_id);

private:
    void build_ui();
    void reload_targets();
    void on_start();
    void on_stop();
    void tick_frame();
    void on_preview_down(wxMouseEvent& ev);
    void on_preview_up(wxMouseEvent& ev);
    void set_control_enabled(bool enabled);
    void send_key(ScreenKey key);
    void send_tap(int x, int y);
    void send_swipe(int x1, int y1, int x2, int y2);
    bool map_to_frame(int view_x, int view_y, int& out_x, int& out_y) const;

    wxChoice* target_ = nullptr;
    wxSpinCtrl* seconds_ = nullptr;
    wxButton* start_btn_ = nullptr;
    wxButton* stop_btn_ = nullptr;
    wxCheckBox* control_ = nullptr;
    wxStaticBitmap* preview_ = nullptr;
    wxStaticText* info_ = nullptr;
    wxButton* back_btn_ = nullptr;
    wxButton* home_btn_ = nullptr;
    wxButton* recents_btn_ = nullptr;
    wxTimer frame_timer_;
    std::vector<std::string> target_ids_;
    std::string out_dir_;
    bool viewing_ = false;
    int frame_w_ = 0;
    int frame_h_ = 0;
    int drag_x1_ = -1;
    int drag_y1_ = -1;
    int drag_x2_ = -1;
    int drag_y2_ = -1;
    std::shared_ptr<lb::ScreenInputQueue> input_;
};

class SyncPanel : public PanelBase {
public:
    SyncPanel(wxWindow* parent, Controller& ctl, MainFrame* frame);
    void refresh(const UiSnapshot& snap) override;
    void on_event(const UiEvent& ev) override;

private:
    void build_ui();
    void reload_targets();
    void reload_conflicts();
    void reload_history();
    void on_sync();
    void on_resolve(const std::string& action);
    void on_restore();
    void on_open_folder();

    wxChoice* target_ = nullptr;
    wxStaticText* state_ = nullptr;
    wxListCtrl* conflicts_ = nullptr;
    wxTextCtrl* history_path_ = nullptr;
    wxListCtrl* history_ = nullptr;
    wxStaticText* message_ = nullptr;
    std::vector<std::string> target_ids_;
    std::vector<std::string> conflict_paths_;
    std::vector<std::string> history_ids_;
    std::vector<std::string> history_paths_;
};

class SettingsPanel : public PanelBase {
public:
    SettingsPanel(wxWindow* parent, Controller& ctl, MainFrame* frame);
    void refresh(const UiSnapshot& snap) override;

private:
    void build_ui();
    void load_from_config();
    lb::AppConfig collect() const;
    void on_save(bool restart);
    void on_open_config();

    wxTextCtrl* device_name_ = nullptr;
    wxSpinCtrl* pair_port_ = nullptr;
    wxSpinCtrl* heartbeat_port_ = nullptr;
    wxSpinCtrl* transfer_port_ = nullptr;
    wxSpinCtrl* clipboard_port_ = nullptr;
    wxSpinCtrl* screen_port_ = nullptr;
    wxSpinCtrl* sync_port_ = nullptr;
    wxSpinCtrl* timeout_ms_ = nullptr;
    wxSpinCtrl* hb_interval_ = nullptr;
    wxCheckBox* clipboard_enabled_ = nullptr;
    wxChoice* clipboard_mode_ = nullptr;
    wxCheckBox* screen_enabled_ = nullptr;
    wxSpinCtrl* screen_fps_ = nullptr;
    wxCheckBox* sync_enabled_ = nullptr;
    wxDirPickerCtrl* sync_folder_ = nullptr;
    wxChoice* sync_mode_ = nullptr;
    wxChoice* conflict_policy_ = nullptr;
    wxSpinCtrl* rescan_sec_ = nullptr;
    wxTextCtrl* ignore_ = nullptr;
    wxCheckBox* auto_accept_ = nullptr;
    wxStaticText* config_path_ = nullptr;
    bool loaded_ = false;
};

class HelpPanel : public PanelBase {
public:
    HelpPanel(wxWindow* parent, Controller& ctl, MainFrame* frame);

private:
    wxHtmlWindow* html_ = nullptr;
};

} // namespace lb::gui
