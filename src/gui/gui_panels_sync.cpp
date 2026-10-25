// 文件夹同步 / 设置 / 帮助 三个面板。
#include "gui/gui_panels.h"

#include "gui/gui_frame.h"
#include "gui/gui_util.h"

#include <wx/scrolwin.h>
#include <wx/statline.h>

#include <algorithm>

namespace lb::gui {

// ---------------- 文件夹同步 ----------------

SyncPanel::SyncPanel(wxWindow* parent, Controller& ctl, MainFrame* frame)
    : PanelBase(parent, ctl, frame) {
    build_ui();
    reload_targets();
    reload_conflicts();
}

void SyncPanel::build_ui() {
    auto* root = new wxBoxSizer(wxVERTICAL);

    auto* statebox = new wxStaticBoxSizer(wxVERTICAL, this, W("同步状态"));
    state_ = new wxStaticText(statebox->GetStaticBox(), wxID_ANY, "-");
    statebox->Add(state_, 1, wxEXPAND | wxALL, 8);

    auto* ctl_row = new wxBoxSizer(wxHORIZONTAL);
    ctl_row->Add(new wxStaticText(statebox->GetStaticBox(), wxID_ANY, W("目标设备")), 0,
                 wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
    target_ = new wxChoice(statebox->GetStaticBox(), wxID_ANY);
    ctl_row->Add(target_, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 10);
    auto* sync_btn = new wxButton(statebox->GetStaticBox(), wxID_ANY, W("立即同步"));
    auto* open_btn = new wxButton(statebox->GetStaticBox(), wxID_ANY, W("打开同步文件夹"));
    ctl_row->Add(sync_btn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    ctl_row->Add(open_btn, 0, wxALIGN_CENTER_VERTICAL);
    statebox->Add(ctl_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
    root->Add(statebox, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);

    auto* conflict_box = new wxStaticBoxSizer(wxVERTICAL, this, W("待处理冲突"));
    conflicts_ = new wxListCtrl(conflict_box->GetStaticBox(), wxID_ANY, wxDefaultPosition,
                                wxSize(-1, 150), wxLC_REPORT | wxLC_SINGLE_SEL | wxLC_HRULES);
    conflicts_->AppendColumn(W("文件"), wxLIST_FORMAT_LEFT, 320);
    conflicts_->AppendColumn(W("冲突副本"), wxLIST_FORMAT_LEFT, 380);
    conflict_box->Add(conflicts_, 1, wxEXPAND | wxALL, 6);
    auto* act_row = new wxBoxSizer(wxHORIZONTAL);
    auto* keep_local = new wxButton(conflict_box->GetStaticBox(), wxID_ANY, W("保留本地"));
    auto* keep_remote = new wxButton(conflict_box->GetStaticBox(), wxID_ANY, W("保留副本"));
    auto* keep_both = new wxButton(conflict_box->GetStaticBox(), wxID_ANY, W("两者都保留"));
    auto* reload_btn = new wxButton(conflict_box->GetStaticBox(), wxID_ANY, W("刷新列表"));
    act_row->Add(keep_local, 0, wxRIGHT, 6);
    act_row->Add(keep_remote, 0, wxRIGHT, 6);
    act_row->Add(keep_both, 0, wxRIGHT, 6);
    act_row->AddStretchSpacer(1);
    act_row->Add(reload_btn, 0);
    conflict_box->Add(act_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 6);
    root->Add(conflict_box, 1, wxEXPAND | wxALL, 10);

    auto* hist_box = new wxStaticBoxSizer(wxVERTICAL, this, W("版本历史"));
    auto* path_row = new wxBoxSizer(wxHORIZONTAL);
    path_row->Add(new wxStaticText(hist_box->GetStaticBox(), wxID_ANY, W("文件相对路径")), 0,
                  wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
    history_path_ = new wxTextCtrl(hist_box->GetStaticBox(), wxID_ANY);
    history_path_->SetHint(W("例如 docs/report.md；留空则列出全部版本"));
    auto* query_btn = new wxButton(hist_box->GetStaticBox(), wxID_ANY, W("查询"));
    auto* restore_btn = new wxButton(hist_box->GetStaticBox(), wxID_ANY, W("恢复选中版本"));
    path_row->Add(history_path_, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    path_row->Add(query_btn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    path_row->Add(restore_btn, 0, wxALIGN_CENTER_VERTICAL);
    hist_box->Add(path_row, 0, wxEXPAND | wxALL, 6);

    history_ = new wxListCtrl(hist_box->GetStaticBox(), wxID_ANY, wxDefaultPosition, wxSize(-1, 140),
                              wxLC_REPORT | wxLC_SINGLE_SEL | wxLC_HRULES);
    history_->AppendColumn(W("归档时间"), wxLIST_FORMAT_LEFT, 140);
    history_->AppendColumn(W("文件"), wxLIST_FORMAT_LEFT, 360);
    history_->AppendColumn(W("大小"), wxLIST_FORMAT_LEFT, 100);
    history_->AppendColumn(W("版本"), wxLIST_FORMAT_LEFT, 200);
    hist_box->Add(history_, 1, wxEXPAND | wxALL, 6);
    root->Add(hist_box, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);

    message_ = new wxStaticText(this, wxID_ANY, W("选择设备与同步方式后点击「立即同步」。"));
    root->Add(message_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
    SetSizer(root);

    sync_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_sync(); });
    open_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_open_folder(); });
    reload_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { reload_conflicts(); });
    keep_local->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_resolve("keep_local"); });
    keep_remote->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_resolve("keep_remote"); });
    keep_both->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_resolve("keep_both"); });
    query_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { reload_history(); });
    restore_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_restore(); });
}

void SyncPanel::reload_targets() {
    const wxString keep = target_->GetStringSelection();
    target_ids_.clear();
    target_->Clear();
    target_->Append(W("全部设备"));
    target_ids_.push_back("all");
    for (const auto& r : ctl_.trust()) {
        target_->Append(W(r.name + "  ·  " + r.device_id.substr(0, 8)));
        target_ids_.push_back(r.device_id);
    }
    if (!keep.empty() && target_->FindString(keep) != wxNOT_FOUND)
        target_->SetStringSelection(keep);
    else
        target_->SetSelection(0);
}

void SyncPanel::reload_conflicts() {
    const auto items = ctl_.conflicts();
    conflict_paths_.clear();
    conflicts_->DeleteAllItems();
    for (std::size_t i = 0; i < items.size(); ++i) {
        const long row = conflicts_->InsertItem(static_cast<long>(i), W(items[i].path));
        wxString copies;
        for (const auto& c : items[i].copies) {
            if (!copies.empty()) copies += ", ";
            copies += W(c);
        }
        conflicts_->SetItem(row, 1, copies);
        conflict_paths_.push_back(items[i].path);
    }
    if (items.empty()) message_->SetLabel(W("当前没有未解决的冲突。"));
}

void SyncPanel::reload_history() {
    const std::string path = U8(history_path_->GetValue());
    const auto items = ctl_.history(path);
    history_ids_.clear();
    history_paths_.clear();
    history_->DeleteAllItems();
    long row = 0;
    for (const auto& v : items) {
        const long idx = history_->InsertItem(row, W(format_ts(v.archived_at)));
        history_->SetItem(idx, 1, W(v.path));
        history_->SetItem(idx, 2, W(format_size(v.size)));
        history_->SetItem(idx, 3, W(v.id));
        history_ids_.push_back(v.id);
        history_paths_.push_back(v.path);
        ++row;
    }
    if (items.empty())
        message_->SetLabel(W("没有找到历史版本（同步覆盖/删除前会自动归档，受 version_keep_* 配置限制）。"));
    else
        message_->SetLabel(W("共 ") + wxString::Format("%zu", items.size()) + W(" 个历史版本。"));
}

void SyncPanel::refresh(const UiSnapshot& snap) {
    state_->SetLabel(
        W("文件夹同步 " + std::string(snap.sync_enabled ? "已启用" : "已停用") + "，模式：" +
          snap.sync_mode + "\n同步目录：" + (snap.sync_folder.empty() ? "（未配置）" : snap.sync_folder) +
          "\n索引 " + std::to_string(snap.sync_index) + " 条，未解决冲突 " +
          std::to_string(snap.sync_conflicts) + " 个"));
    if (target_ids_.size() != snap.trusted + 1) reload_targets();
    if (snap.sync_conflicts != static_cast<std::size_t>(conflicts_->GetItemCount()))
        reload_conflicts();
    Layout();
}

void SyncPanel::on_event(const UiEvent& ev) {
    if (ev.type == "sync_done") {
        message_->SetLabel(W("同步完成：推送 ") + wxString::Format("%llu", (unsigned long long)ev.a) +
                           W(" 个，拉取 ") + wxString::Format("%llu", (unsigned long long)ev.b) + W(" 个。"));
        reload_conflicts();
    } else if (ev.type == "sync_failed") {
        message_->SetLabel(W("同步失败：") + W(ev.detail));
    } else if (ev.type == "sync_summary" && !ev.detail.empty()) {
        message_->SetLabel(W(ev.detail));
        reload_conflicts();
    }
}

void SyncPanel::on_sync() {
    if (!frame_->service_running()) {
        wxMessageBox(W("服务未启动，请先在「概览」页启动服务。"), W("文件夹同步"),
                     wxOK | wxICON_INFORMATION, this);
        return;
    }
    const int sel = target_->GetSelection();
    if (sel < 0 || sel >= static_cast<int>(target_ids_.size())) return;
    message_->SetLabel(W("正在同步…"));
    const std::string id = target_ids_[sel];
    ctl_.sync_once(id, [this](bool ok, const std::string& msg) {
        message_->SetLabel((ok ? W("同步完成：") : W("同步失败：")) + W(msg));
        frame_->append_log((ok ? W("同步完成：") : W("同步失败：")) + W(msg));
        reload_conflicts();
    });
}

void SyncPanel::on_resolve(const std::string& action) {
    const long row = conflicts_->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    if (row < 0 || row >= static_cast<long>(conflict_paths_.size())) {
        wxMessageBox(W("请先在冲突列表中选择一条记录。"), W("处理冲突"),
                     wxOK | wxICON_INFORMATION, this);
        return;
    }
    const std::string path = conflict_paths_[static_cast<std::size_t>(row)];
    ctl_.resolve_conflict(path, action, [this](bool ok, const std::string& msg) {
        message_->SetLabel((ok ? W("冲突已解决：") : W("冲突处理失败：")) + W(msg));
        frame_->append_log((ok ? W("冲突已解决：") : W("冲突处理失败：")) + W(msg));
        reload_conflicts();
    });
}

void SyncPanel::on_restore() {
    const long row = history_->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    if (row < 0 || row >= static_cast<long>(history_ids_.size())) {
        wxMessageBox(W("请先在版本列表中选择一条记录。"), W("恢复版本"),
                     wxOK | wxICON_INFORMATION, this);
        return;
    }
    const std::string path = history_paths_[static_cast<std::size_t>(row)];
    const std::string id = history_ids_[static_cast<std::size_t>(row)];
    if (wxMessageBox(W("将用所选历史版本覆盖本地文件：") + W(path) +
                         W("\n当前文件会先归档到版本库，是否继续？"),
                     W("恢复版本"), wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION, this) != wxID_YES)
        return;
    ctl_.restore_version(path, id, [this](bool ok, const std::string& msg) {
        message_->SetLabel((ok ? W("已恢复版本：") : W("恢复失败：")) + W(msg));
        frame_->append_log((ok ? W("已恢复版本：") : W("恢复失败：")) + W(msg));
        reload_history();
    });
}

void SyncPanel::on_open_folder() {
    const std::string folder = ctl_.config().sync.folder;
    if (folder.empty()) {
        wxMessageBox(W("尚未配置同步文件夹，请先到「设置」页选择同步目录。"), W("打开同步文件夹"),
                     wxOK | wxICON_INFORMATION, this);
        return;
    }
    if (!wxLaunchDefaultApplication(W(folder)))
        wxMessageBox(W("无法打开目录：") + W(folder), W("打开同步文件夹"),
                     wxOK | wxICON_ERROR, this);
}

// ---------------- 设置 ----------------

SettingsPanel::SettingsPanel(wxWindow* parent, Controller& ctl, MainFrame* frame)
    : PanelBase(parent, ctl, frame) {
    build_ui();
    load_from_config();
}

void SettingsPanel::build_ui() {
    auto* scroll = new wxScrolledWindow(this, wxID_ANY);
    scroll->SetScrollRate(0, 12);

    auto* root = new wxBoxSizer(wxVERTICAL);
    auto* box_parent = scroll;

    // 身份
    {
        auto* box = new wxStaticBoxSizer(wxVERTICAL, box_parent, W("设备身份"));
        auto* grid = new wxFlexGridSizer(2, 6, 8);
        grid->AddGrowableCol(1, 1);
        grid->Add(new wxStaticText(box->GetStaticBox(), wxID_ANY, W("设备名")), 0,
                  wxALIGN_CENTER_VERTICAL);
        device_name_ = new wxTextCtrl(box->GetStaticBox(), wxID_ANY);
        device_name_->SetHint(W("留空则使用系统主机名"));
        grid->Add(device_name_, 1, wxEXPAND);
        box->Add(grid, 1, wxEXPAND | wxALL, 8);
        root->Add(box, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);
    }

    // 网络
    {
        auto* box = new wxStaticBoxSizer(wxVERTICAL, box_parent, W("通道端口与连接"));
        auto* grid = new wxFlexGridSizer(2, 6, 8);
        grid->AddGrowableCol(1, 1);
        auto add_port = [&](const wxString& label, wxSpinCtrl*& target) {
            grid->Add(new wxStaticText(box->GetStaticBox(), wxID_ANY, label), 0,
                      wxALIGN_CENTER_VERTICAL);
            target = new wxSpinCtrl(box->GetStaticBox(), wxID_ANY, wxEmptyString, wxDefaultPosition,
                                    wxSize(120, -1), wxSP_ARROW_KEYS, 1, 65535, 45710);
            grid->Add(target, 0, wxALIGN_CENTER_VERTICAL);
        };
        add_port(W("配对端口"), pair_port_);
        add_port(W("心跳端口"), heartbeat_port_);
        add_port(W("文件传输端口"), transfer_port_);
        add_port(W("剪贴板端口"), clipboard_port_);
        add_port(W("屏幕共享端口"), screen_port_);
        add_port(W("文件夹同步端口"), sync_port_);

        grid->Add(new wxStaticText(box->GetStaticBox(), wxID_ANY, W("连接超时（毫秒）")), 0,
                  wxALIGN_CENTER_VERTICAL);
        timeout_ms_ = new wxSpinCtrl(box->GetStaticBox(), wxID_ANY, wxEmptyString, wxDefaultPosition,
                                     wxSize(120, -1), wxSP_ARROW_KEYS, 500, 60000, 5000);
        grid->Add(timeout_ms_, 0, wxALIGN_CENTER_VERTICAL);

        grid->Add(new wxStaticText(box->GetStaticBox(), wxID_ANY, W("心跳间隔（秒）")), 0,
                  wxALIGN_CENTER_VERTICAL);
        hb_interval_ = new wxSpinCtrl(box->GetStaticBox(), wxID_ANY, wxEmptyString,
                                      wxDefaultPosition, wxSize(120, -1), wxSP_ARROW_KEYS, 5, 3600,
                                      30);
        grid->Add(hb_interval_, 0, wxALIGN_CENTER_VERTICAL);
        box->Add(grid, 1, wxEXPAND | wxALL, 8);
        box->Add(new wxStaticText(box->GetStaticBox(), wxID_ANY,
                                  W("七个端口必须互不相同；修改后需重启服务生效。")),
                 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
        root->Add(box, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);
    }

    // 剪贴板与屏幕
    {
        auto* box = new wxStaticBoxSizer(wxVERTICAL, box_parent, W("剪贴板与屏幕共享"));
        auto* grid = new wxFlexGridSizer(4, 6, 8);
        grid->AddGrowableCol(1, 1);
        grid->AddGrowableCol(3, 1);

        grid->Add(new wxStaticText(box->GetStaticBox(), wxID_ANY, W("剪贴板同步")), 0,
                  wxALIGN_CENTER_VERTICAL);
        clipboard_enabled_ = new wxCheckBox(box->GetStaticBox(), wxID_ANY, W("启用"));
        grid->Add(clipboard_enabled_, 0, wxALIGN_CENTER_VERTICAL);
        grid->Add(new wxStaticText(box->GetStaticBox(), wxID_ANY, W("接收模式")), 0,
                  wxALIGN_CENTER_VERTICAL);
        clipboard_mode_ = new wxChoice(box->GetStaticBox(), wxID_ANY);
        clipboard_mode_->Append(W("confirm（每次确认）"));
        clipboard_mode_->Append(W("auto（自动接收）"));
        grid->Add(clipboard_mode_, 1, wxEXPAND);

        grid->Add(new wxStaticText(box->GetStaticBox(), wxID_ANY, W("屏幕共享")), 0,
                  wxALIGN_CENTER_VERTICAL);
        screen_enabled_ = new wxCheckBox(box->GetStaticBox(), wxID_ANY, W("启用"));
        grid->Add(screen_enabled_, 0, wxALIGN_CENTER_VERTICAL);
        grid->Add(new wxStaticText(box->GetStaticBox(), wxID_ANY, W("采集帧率上限")), 0,
                  wxALIGN_CENTER_VERTICAL);
        screen_fps_ = new wxSpinCtrl(box->GetStaticBox(), wxID_ANY, wxEmptyString,
                                     wxDefaultPosition, wxSize(120, -1), wxSP_ARROW_KEYS, 1, 30, 2);
        grid->Add(screen_fps_, 0, wxALIGN_CENTER_VERTICAL);
        box->Add(grid, 1, wxEXPAND | wxALL, 8);
        root->Add(box, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);
    }

    // 文件夹同步
    {
        auto* box = new wxStaticBoxSizer(wxVERTICAL, box_parent, W("文件夹同步"));
        auto* grid = new wxFlexGridSizer(2, 6, 8);
        grid->AddGrowableCol(1, 1);

        grid->Add(new wxStaticText(box->GetStaticBox(), wxID_ANY, W("启用同步")), 0,
                  wxALIGN_CENTER_VERTICAL);
        sync_enabled_ = new wxCheckBox(box->GetStaticBox(), wxID_ANY, W("启用"));
        grid->Add(sync_enabled_, 0, wxALIGN_CENTER_VERTICAL);

        grid->Add(new wxStaticText(box->GetStaticBox(), wxID_ANY, W("同步文件夹")), 0,
                  wxALIGN_CENTER_VERTICAL);
        sync_folder_ = new wxDirPickerCtrl(box->GetStaticBox(), wxID_ANY, wxEmptyString,
                                           W("选择本端同步根目录"));
        grid->Add(sync_folder_, 1, wxEXPAND);

        grid->Add(new wxStaticText(box->GetStaticBox(), wxID_ANY, W("同步模式")), 0,
                  wxALIGN_CENTER_VERTICAL);
        sync_mode_ = new wxChoice(box->GetStaticBox(), wxID_ANY);
        sync_mode_->Append(W("send_receive（双向）"));
        sync_mode_->Append(W("send_only（只发送）"));
        sync_mode_->Append(W("receive_only（只接收）"));
        sync_mode_->Append(W("mirror（镜像）"));
        grid->Add(sync_mode_, 1, wxEXPAND);

        grid->Add(new wxStaticText(box->GetStaticBox(), wxID_ANY, W("冲突策略")), 0,
                  wxALIGN_CENTER_VERTICAL);
        conflict_policy_ = new wxChoice(box->GetStaticBox(), wxID_ANY);
        conflict_policy_->Append(W("keep_both（两者都留）"));
        conflict_policy_->Append(W("newer（保留较新）"));
        conflict_policy_->Append(W("larger（保留较大）"));
        grid->Add(conflict_policy_, 1, wxEXPAND);

        grid->Add(new wxStaticText(box->GetStaticBox(), wxID_ANY, W("自动扫描间隔（秒）")), 0,
                  wxALIGN_CENTER_VERTICAL);
        rescan_sec_ = new wxSpinCtrl(box->GetStaticBox(), wxID_ANY, wxEmptyString,
                                     wxDefaultPosition, wxSize(120, -1), wxSP_ARROW_KEYS, 5, 86400,
                                     3600);
        grid->Add(rescan_sec_, 0, wxALIGN_CENTER_VERTICAL);

        grid->Add(new wxStaticText(box->GetStaticBox(), wxID_ANY, W("忽略规则（逗号分隔）")), 0,
                  wxALIGN_CENTER_VERTICAL);
        ignore_ = new wxTextCtrl(box->GetStaticBox(), wxID_ANY);
        grid->Add(ignore_, 1, wxEXPAND);
        box->Add(grid, 1, wxEXPAND | wxALL, 8);
        root->Add(box, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);
    }

    // 测试选项
    {
        auto* box = new wxStaticBoxSizer(wxVERTICAL, box_parent, W("确认对话框"));
        auto_accept_ = new wxCheckBox(box->GetStaticBox(), wxID_ANY,
                                      W("自动接受配对/剪贴板/屏幕请求（测试用，跳过确认框）"));
        box->Add(auto_accept_, 0, wxEXPAND | wxALL, 8);
        root->Add(box, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);
    }

    // 配置文件与操作按钮
    {
        auto* box = new wxStaticBoxSizer(wxVERTICAL, box_parent, W("配置文件"));
        config_path_ = new wxStaticText(box->GetStaticBox(), wxID_ANY, "-");
        box->Add(config_path_, 0, wxEXPAND | wxALL, 8);
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        auto* open_cfg = new wxButton(box->GetStaticBox(), wxID_ANY, W("打开配置文件"));
        auto* reload_cfg = new wxButton(box->GetStaticBox(), wxID_ANY, W("放弃修改并刷新"));
        auto* save_btn = new wxButton(box->GetStaticBox(), wxID_ANY, W("保存"));
        auto* save_restart = new wxButton(box->GetStaticBox(), wxID_ANY, W("保存并重启服务"));
        row->Add(open_cfg, 0, wxRIGHT, 6);
        row->Add(reload_cfg, 0, wxRIGHT, 6);
        row->AddStretchSpacer(1);
        row->Add(save_btn, 0, wxRIGHT, 6);
        row->Add(save_restart, 0);
        box->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
        root->Add(box, 0, wxEXPAND | wxALL, 10);

        open_cfg->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_open_config(); });
        reload_cfg->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { load_from_config(); });
        save_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_save(false); });
        save_restart->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_save(true); });
    }

    scroll->SetSizer(root);
    scroll->FitInside();

    auto* outer = new wxBoxSizer(wxVERTICAL);
    outer->Add(scroll, 1, wxEXPAND);
    SetSizer(outer);
}

void SettingsPanel::load_from_config() {
    const AppConfig& cfg = ctl_.config();
    device_name_->SetValue(W(cfg.device_name));
    pair_port_->SetValue(cfg.network.pair_port);
    heartbeat_port_->SetValue(cfg.network.heartbeat_port);
    transfer_port_->SetValue(cfg.network.transfer_port);
    clipboard_port_->SetValue(cfg.network.clipboard_port);
    screen_port_->SetValue(cfg.network.screen_port);
    sync_port_->SetValue(cfg.network.sync_port);
    timeout_ms_->SetValue(cfg.network.connect_timeout_ms);
    hb_interval_->SetValue(cfg.heartbeat.interval_sec);
    clipboard_enabled_->SetValue(cfg.clipboard.enabled);
    clipboard_mode_->SetSelection(cfg.clipboard.mode == "auto" ? 1 : 0);
    screen_enabled_->SetValue(cfg.screen.enabled);
    screen_fps_->SetValue(cfg.screen.max_fps);
    sync_enabled_->SetValue(cfg.sync.enabled);
    sync_folder_->SetPath(W(cfg.sync.folder));
    if (cfg.sync.mode == "send_only")
        sync_mode_->SetSelection(1);
    else if (cfg.sync.mode == "receive_only")
        sync_mode_->SetSelection(2);
    else if (cfg.sync.mode == "mirror")
        sync_mode_->SetSelection(3);
    else
        sync_mode_->SetSelection(0);
    if (cfg.sync.default_conflict_policy == "newer")
        conflict_policy_->SetSelection(1);
    else if (cfg.sync.default_conflict_policy == "larger")
        conflict_policy_->SetSelection(2);
    else
        conflict_policy_->SetSelection(0);
    rescan_sec_->SetValue(cfg.sync.rescan_interval_sec);
    ignore_->SetValue(W(cfg.sync.ignore));
    config_path_->SetLabel(W(ctl_.config_path()));
    loaded_ = true;
}

AppConfig SettingsPanel::collect() const {
    AppConfig cfg = ctl_.config();
    cfg.device_name = U8(device_name_->GetValue());
    cfg.network.pair_port = pair_port_->GetValue();
    cfg.network.heartbeat_port = heartbeat_port_->GetValue();
    cfg.network.transfer_port = transfer_port_->GetValue();
    cfg.network.clipboard_port = clipboard_port_->GetValue();
    cfg.network.screen_port = screen_port_->GetValue();
    cfg.network.sync_port = sync_port_->GetValue();
    cfg.network.connect_timeout_ms = timeout_ms_->GetValue();
    cfg.heartbeat.interval_sec = hb_interval_->GetValue();
    cfg.clipboard.enabled = clipboard_enabled_->GetValue();
    cfg.clipboard.mode = clipboard_mode_->GetSelection() == 1 ? "auto" : "confirm";
    cfg.screen.enabled = screen_enabled_->GetValue();
    cfg.screen.max_fps = screen_fps_->GetValue();
    cfg.sync.enabled = sync_enabled_->GetValue();
    cfg.sync.folder = U8(sync_folder_->GetPath());
    static const char* kModes[] = {"send_receive", "send_only", "receive_only", "mirror"};
    const int mode = sync_mode_->GetSelection();
    cfg.sync.mode = kModes[mode >= 0 && mode < 4 ? mode : 0];
    static const char* kPolicies[] = {"keep_both", "newer", "larger"};
    const int pol = conflict_policy_->GetSelection();
    cfg.sync.default_conflict_policy = kPolicies[pol >= 0 && pol < 3 ? pol : 0];
    cfg.sync.rescan_interval_sec = rescan_sec_->GetValue();
    cfg.sync.ignore = U8(ignore_->GetValue());
    return cfg;
}

void SettingsPanel::refresh(const UiSnapshot& snap) {
    if (!loaded_) load_from_config();
    if (config_path_->GetLabel().empty() || config_path_->GetLabel() == "-")
        config_path_->SetLabel(W(snap.config_path));
}

void SettingsPanel::on_save(bool restart) {
    const AppConfig cfg = collect();
    std::string error;
    if (!ctl_.save_config(cfg, &error)) {
        wxMessageBox(W("保存失败：") + W(error), W("设置"), wxOK | wxICON_ERROR, this);
        return;
    }
    ctl_.set_auto_accept(auto_accept_->GetValue());
    frame_->append_log(W("[设置] 已保存到 ") + W(ctl_.config_path()));
    if (restart) {
        frame_->restart_service();
    } else if (wxMessageBox(W("配置已保存。端口与开关类配置需要重启服务后生效，是否立即重启？"),
                            W("设置"), wxYES_NO | wxICON_QUESTION, this) == wxID_YES) {
        frame_->restart_service();
    }
    load_from_config();
}

void SettingsPanel::on_open_config() {
    const std::string path = ctl_.config_path();
    if (!wxLaunchDefaultApplication(W(path)))
        wxMessageBox(W("无法调用系统默认程序打开：") + W(path) + W("\n请手动用文本编辑器打开。"),
                     W("打开配置文件"), wxOK | wxICON_INFORMATION, this);
}

// ---------------- 帮助 ----------------

HelpPanel::HelpPanel(wxWindow* parent, Controller& ctl, MainFrame* frame)
    : PanelBase(parent, ctl, frame) {
    auto* root = new wxBoxSizer(wxVERTICAL);
    html_ = new wxHtmlWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                             wxHW_SCROLLBAR_AUTO);
    html_->SetPage(W(R"(<html><body>
<h2>LinkBridge 连桥 — 操作指引</h2>
<p>LinkBridge 在局域网内连接你的多台设备，提供文件互传、剪贴板同步、文件夹同步与只读屏幕共享。
所有通道都经过双向认证与端到端加密。</p>

<h3>一、快速开始（两台设备）</h3>
<ol>
<li>两台设备分别启动本程序，首次启动会自动创建配置与设备身份。</li>
<li>在「概览」页确认服务状态为「运行中」；若已停止，点击「启动服务」或按 <b>F5</b>。</li>
<li>在 A 机的「配对」页点击「生成配对码」，记录屏幕上的 6 位数字。</li>
<li>在 B 机的「配对」页填写 A 机的局域网 IP 和配对码，点击「发起配对」。</li>
<li>双方会弹出确认框并显示指纹，逐位比对一致后点击「接受」。</li>
<li>配对成功后，设备会出现在「设备」页的受信任列表中。</li>
</ol>

<h3>二、文件传输</h3>
<ol>
<li>打开「文件传输」页，在「目标设备」中选择已配对的设备。</li>
<li>点击「浏览…」选择文件，或直接粘贴完整路径；可修改对方保存的文件名。</li>
<li>点击「开始发送」，进度条显示实时进度，完成后记录出现在下方列表。</li>
<li>接收方文件默认保存到「概览」页显示的接收目录（数据目录下的 inbox）。</li>
</ol>

<h3>三、剪贴板同步</h3>
<ul>
<li>「读取本地剪贴板」：查看当前剪贴板内容与类型。</li>
<li>输入文本、选择目标（可「全部设备」）后点击「推送剪贴板」。</li>
<li>接收方按其配置处理：confirm 模式会弹窗预览并请求确认，auto 模式自动写入。</li>
<li>也支持在「设备」页选中设备后直接「推送剪贴板…」。</li>
</ul>

<h3>四、屏幕共享与控制</h3>
<ol>
<li>打开「屏幕共享与控制」页，选择目标设备与查看时长（3–120 秒）。</li>
<li>点击「开始查看」，对方会收到确认请求；接受后画面每 0.5 秒刷新。</li>
<li>勾选「远程控制」再开始：点击画面发送单击，按住拖动发送滑动；「返回/主页/最近任务」注入系统按键。</li>
<li>对方必须支持输入注入（Android 需在系统设置中启用「连桥屏幕控制」无障碍服务），否则控制模式会被拒绝。</li>
<li>「停止预览」只停止本地刷新，会话会在设定时长后自然结束；最新一帧保存在数据目录的 <b>screen/latest.bmp</b>。</li>
</ol>

<h3>五、文件夹同步</h3>
<ol>
<li>先在「设置」页勾选「启用同步」，选择本端同步文件夹、模式与冲突策略，保存并重启服务。</li>
<li>回到「文件夹同步」页，选择目标设备后点击「立即同步」。</li>
<li>「待处理冲突」列出原文件与冲突副本，可分别选择保留本地、保留副本或两者都保留。</li>
<li>「版本历史」可查询并恢复被覆盖或删除前的历史版本。</li>
</ol>

<h3>六、设置</h3>
<ul>
<li>修改端口、超时、心跳间隔、剪贴板/屏幕开关与同步参数后点击「保存」。</li>
<li>端口、开关类配置需要重启服务生效，可直接使用「保存并重启服务」。</li>
<li>「自动接受请求」仅用于测试，会跳过所有确认对话框，请谨慎开启。</li>
<li>「打开配置文件」可查看原始 TOML；命令行与 GUI 共用同一份配置。</li>
</ul>

<h3>七、快捷键</h3>
<table border="1" cellpadding="4" cellspacing="0">
<tr><td><b>F5</b></td><td>启动服务</td></tr>
<tr><td><b>Shift+F5</b></td><td>停止服务</td></tr>
<tr><td><b>F1</b></td><td>打开本页操作指引</td></tr>
<tr><td><b>Alt+F4</b></td><td>退出程序</td></tr>
</table>

<h3>八、常见问题</h3>
<ul>
<li><b>看不到对方设备</b>：确认两台设备在同一局域网；防火墙放行七个 TCP 端口
（默认 45710–45716）；核对填写的 IP 是否为对方局域网地址。</li>
<li><b>配对码无效</b>：配对码 5 分钟内有效且单次使用，过期请重新生成。</li>
<li><b>服务启动失败</b>：通常是端口被占用，到「设置」修改端口后保存并重启。</li>
<li><b>屏幕没有画面</b>：对方需启用屏幕共享，并在弹窗中接受本次会话请求。</li>
<li><b>同步提示未启用</b>：到「设置」启用同步并选择有效目录，重启服务后再试。</li>
</ul>

<h3>九、命令行对照</h3>
<p>GUI 与命令行共用同一套核心实现，命令行可完成同样的操作：
<b>linkbridge pair / send / clip / screen / sync / code / daemon</b>。
需要自动化或脚本化时可直接使用命令行，行为与界面一致。</p>
</body></html>)"));
    root->Add(html_, 1, wxEXPAND);
    SetSizer(root);
}

} // namespace lb::gui
