// 概览 / 设备 / 配对 三个面板。
#include "gui/gui_panels.h"

#include "gui/gui_frame.h"
#include "gui/gui_util.h"

#include <wx/dirdlg.h>
#include <wx/filedlg.h>
#include <wx/statline.h>
#include <wx/textdlg.h>

#include <algorithm>

namespace lb::gui {

PanelBase::PanelBase(wxWindow* parent, Controller& ctl, MainFrame* frame)
    : wxPanel(parent, wxID_ANY), ctl_(ctl), frame_(frame) {}

// ---------------- 概览 ----------------

StatusPanel::StatusPanel(wxWindow* parent, Controller& ctl, MainFrame* frame)
    : PanelBase(parent, ctl, frame) {
    build_ui();
}

void StatusPanel::build_ui() {
    auto* root = new wxBoxSizer(wxVERTICAL);

    auto* svc = new wxStaticBoxSizer(wxHORIZONTAL, this, W("服务状态"));
    state_ = new wxStaticText(svc->GetStaticBox(), wxID_ANY, W("正在读取…"));
    {
        wxFont f = state_->GetFont();
        f.SetPointSize(f.GetPointSize() + 2);
        f.MakeBold();
        state_->SetFont(f);
    }
    svc->Add(state_, 0, wxALIGN_CENTER_VERTICAL | wxALL, 8);
    svc->AddStretchSpacer(1);
    start_btn_ = new wxButton(svc->GetStaticBox(), wxID_ANY, W("启动服务"));
    stop_btn_ = new wxButton(svc->GetStaticBox(), wxID_ANY, W("停止服务"));
    svc->Add(start_btn_, 0, wxALIGN_CENTER_VERTICAL | wxALL, 6);
    svc->Add(stop_btn_, 0, wxALIGN_CENTER_VERTICAL | wxALL, 6);
    root->Add(svc, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);

    auto* idbox = new wxStaticBoxSizer(wxVERTICAL, this, W("本机身份"));
    auto* grid = new wxFlexGridSizer(2, 8, 12);
    grid->AddGrowableCol(1, 1);
    auto add_row = [&](const wxString& label, wxStaticText*& target) {
        grid->Add(new wxStaticText(idbox->GetStaticBox(), wxID_ANY, label), 0,
                  wxALIGN_CENTER_VERTICAL);
        target = new wxStaticText(idbox->GetStaticBox(), wxID_ANY, "-");
        grid->Add(target, 1, wxEXPAND);
    };
    add_row(W("设备名"), name_);
    add_row(W("设备 ID"), id_);
    add_row(W("指纹"), fp_);
    idbox->Add(grid, 1, wxEXPAND | wxALL, 8);
    root->Add(idbox, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);

    auto* row = new wxBoxSizer(wxHORIZONTAL);
    auto* portbox = new wxStaticBoxSizer(wxVERTICAL, this, W("通道端口"));
    ports_ = new wxStaticText(portbox->GetStaticBox(), wxID_ANY, "-");
    portbox->Add(ports_, 1, wxEXPAND | wxALL, 8);
    row->Add(portbox, 1, wxEXPAND | wxRIGHT, 10);

    auto* statbox = new wxStaticBoxSizer(wxVERTICAL, this, W("运行统计"));
    stats_ = new wxStaticText(statbox->GetStaticBox(), wxID_ANY, "-");
    statbox->Add(stats_, 1, wxEXPAND | wxALL, 8);
    row->Add(statbox, 1, wxEXPAND);
    root->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);

    auto* dirbox = new wxStaticBoxSizer(wxVERTICAL, this, W("数据目录"));
    dirs_ = new wxStaticText(dirbox->GetStaticBox(), wxID_ANY, "-");
    dirbox->Add(dirs_, 1, wxEXPAND | wxALL, 8);
    root->Add(dirbox, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);

    auto* logbox = new wxStaticBoxSizer(wxVERTICAL, this, W("活动日志"));
    log_ = new wxTextCtrl(logbox->GetStaticBox(), wxID_ANY, wxEmptyString, wxDefaultPosition,
                          wxDefaultSize, wxTE_MULTILINE | wxTE_READONLY | wxBORDER_SIMPLE);
    logbox->Add(log_, 1, wxEXPAND | wxALL, 6);
    root->Add(logbox, 1, wxEXPAND | wxALL, 10);

    SetSizer(root);

    start_btn_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { frame_->start_service(); });
    stop_btn_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { frame_->stop_service(); });
}

void StatusPanel::append_log(const wxString& line) {
    if (!log_) return;
    log_->AppendText(W("[" + host_time_now() + "] ") + line + "\n");
    log_->ShowPosition(log_->GetLastPosition());
}

void StatusPanel::on_event(const UiEvent& ev) {
    wxString text;
    const wxString title = W(ev.title);
    if (ev.type == "pairing_ok")
        text = W("配对成功：") + title + W("（") + W(ev.detail) + W("）");
    else if (ev.type == "pairing_state")
        text = W("配对状态：") + title + (ev.detail.empty() ? wxString() : W(" — ") + W(ev.detail));
    else if (ev.type == "device_online")
        text = W("设备上线：") + title;
    else if (ev.type == "device_offline")
        text = W("设备离线：") + title;
    else if (ev.type == "transfer_progress")
        text = wxString::Format(W("%s：%s %llu%%"), ev.outgoing ? W("发送") : W("接收"), title,
                                static_cast<unsigned long long>(ev.b ? (ev.a * 100 / ev.b) : 100));
    else if (ev.type == "transfer_done")
        text = wxString::Format(W("%s完成：%s（%s）"), ev.outgoing ? W("发送") : W("接收"), title,
                                W(format_size(ev.a)));
    else if (ev.type == "transfer_failed")
        text = W("传输失败：") + title + W(" — ") + W(ev.detail);
    else if (ev.type == "clipboard_in")
        text = W("收到剪贴板：") + title + W("（") + W(ev.detail) + W("）");
    else if (ev.type == "clipboard_out")
        text = W("剪贴板已推送：") + title;
    else if (ev.type == "clipboard_fail")
        text = W("剪贴板失败：") + title + W(" — ") + W(ev.detail);
    else if (ev.type == "clipboard_skipped")
        text = W("剪贴板跳过：") + W(ev.detail);
    else if (ev.type == "screen_started")
        text = W("屏幕共享会话开始：") + title;
    else if (ev.type == "screen_ended")
        text = wxString::Format(W("屏幕共享结束：%s — %s（%llu 帧）"), title, W(ev.detail),
                                static_cast<unsigned long long>(ev.a));
    else if (ev.type == "screen_failed")
        text = W("屏幕共享失败：") + title + W(" — ") + W(ev.detail);
    else if (ev.type == "sync_started")
        text = W("开始同步：") + title;
    else if (ev.type == "sync_done")
        text = wxString::Format(W("同步完成：%s（发出 %llu、接收 %llu）"), title,
                                static_cast<unsigned long long>(ev.a),
                                static_cast<unsigned long long>(ev.b));
    else if (ev.type == "sync_summary")
        text = W("同步结果：") + W(ev.detail);
    else if (ev.type == "sync_failed")
        text = W("同步失败：") + title + W(" — ") + W(ev.detail);
    if (!text.empty()) append_log(text);
}

void StatusPanel::refresh(const UiSnapshot& snap) {
    state_->SetLabel(snap.running ? W("运行中：六个通道已监听")
                                  : W("已停止：点击右侧「启动服务」"));
    name_->SetLabel(W(snap.device_name));
    id_->SetLabel(W(snap.device_id));
    fp_->SetLabel(W(snap.fingerprint));
    ports_->SetLabel(W("配对 " + std::to_string(snap.pair_port) + " · 心跳 " +
                       std::to_string(snap.heartbeat_port) + " · 传输 " +
                       std::to_string(snap.transfer_port) + "\n剪贴板 " +
                       std::to_string(snap.clipboard_port) + " · 屏幕 " +
                       std::to_string(snap.screen_port) + " · 同步 " +
                       std::to_string(snap.sync_port)));
    stats_->SetLabel(
        W("受信任设备 " + std::to_string(snap.trusted) + " 台，在线 " + std::to_string(snap.online) +
          " 台\n心跳探测 成功 " + std::to_string(snap.probes_ok) + " / 失败 " +
          std::to_string(snap.probes_failed) + "\n剪贴板 " +
          (snap.clipboard_enabled ? "启用" : "停用") + "（" + snap.clipboard_mode + "）" +
          " · 屏幕 " + (snap.screen_enabled ? "启用" : "停用") + "\n文件夹同步 " +
          (snap.sync_enabled ? "启用" : "停用") + "，索引 " + std::to_string(snap.sync_index) +
          " 条，冲突 " + std::to_string(snap.sync_conflicts) + " 个"));
    dirs_->SetLabel(W("数据：" + snap.data_dir + "\n配置：" + snap.config_path + "\n接收目录：" +
                      snap.inbox_dir));
    start_btn_->Enable(!snap.running);
    stop_btn_->Enable(snap.running);
}

// ---------------- 设备 ----------------

DevicesPanel::DevicesPanel(wxWindow* parent, Controller& ctl, MainFrame* frame)
    : PanelBase(parent, ctl, frame) {
    build_ui();
    reload_list();
}

void DevicesPanel::build_ui() {
    auto* root = new wxBoxSizer(wxVERTICAL);
    hint_ = new wxStaticText(this, wxID_ANY,
                             W("在列表中选择一台受信任设备，再使用下方按钮发送文件、推送剪贴板、"
                               "同步文件夹或查看屏幕。"));
    root->Add(hint_, 0, wxEXPAND | wxALL, 10);

    list_ = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                           wxLC_REPORT | wxLC_SINGLE_SEL | wxLC_HRULES);
    list_->AppendColumn(W("设备名"), wxLIST_FORMAT_LEFT, 180);
    list_->AppendColumn(W("设备 ID"), wxLIST_FORMAT_LEFT, 170);
    list_->AppendColumn(W("平台"), wxLIST_FORMAT_LEFT, 90);
    list_->AppendColumn(W("状态"), wxLIST_FORMAT_LEFT, 90);
    list_->AppendColumn(W("最近 IP"), wxLIST_FORMAT_LEFT, 140);
    list_->AppendColumn(W("配对时间"), wxLIST_FORMAT_LEFT, 140);
    root->Add(list_, 1, wxEXPAND | wxLEFT | wxRIGHT, 10);

    auto* buttons = new wxBoxSizer(wxHORIZONTAL);
    auto* refresh_btn = new wxButton(this, wxID_ANY, W("刷新"));
    auto* send_btn = new wxButton(this, wxID_ANY, W("发送文件…"));
    auto* clip_btn = new wxButton(this, wxID_ANY, W("推送剪贴板…"));
    auto* sync_btn = new wxButton(this, wxID_ANY, W("立即同步"));
    auto* screen_btn = new wxButton(this, wxID_ANY, W("查看屏幕"));
    buttons->Add(refresh_btn, 0, wxRIGHT, 6);
    buttons->Add(send_btn, 0, wxRIGHT, 6);
    buttons->Add(clip_btn, 0, wxRIGHT, 6);
    buttons->Add(sync_btn, 0, wxRIGHT, 6);
    buttons->Add(screen_btn, 0);
    root->Add(buttons, 0, wxALL, 10);
    SetSizer(root);

    refresh_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { reload_list(); });
    send_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_send(); });
    clip_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_push_clip(); });
    sync_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_sync(); });
    screen_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_screen(); });
}

std::string DevicesPanel::selected_id() const {
    const long row = list_->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    if (row < 0 || row >= static_cast<long>(row_ids_.size())) return {};
    return row_ids_[static_cast<std::size_t>(row)];
}

void DevicesPanel::reload_list() {
    const std::string keep = selected_id();
    const auto records = ctl_.trust();
    const auto statuses = ctl_.devices();
    row_ids_.clear();
    list_->DeleteAllItems();
    int row = 0;
    for (const auto& r : records) {
        const long idx = list_->InsertItem(row, W(r.name));
        list_->SetItem(idx, 1, W(r.device_id));
        list_->SetItem(idx, 2, W(platform_name(r.platform)));
        Presence p = Presence::Unknown;
        for (const auto& s : statuses)
            if (s.device_id == r.device_id) p = s.presence;
        list_->SetItem(idx, 3, p == Presence::Online ? W("在线")
                                    : (p == Presence::Offline ? W("离线") : W("未知")));
        list_->SetItem(idx, 4, W(r.last_ip.empty() ? "-" : r.last_ip));
        list_->SetItem(idx, 5, W(format_ts(r.paired_at)));
        row_ids_.push_back(r.device_id);
        if (r.device_id == keep) {
            list_->SetItemState(idx, wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED,
                                wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED);
        }
        ++row;
    }
    if (records.empty())
        hint_->SetLabel(W("还没有受信任设备。请先到「配对」页生成配对码（本机）或用配对码发起配对。"));
    else
        hint_->SetLabel(W("共 ") + wxString::Format("%zu", records.size()) +
                        W(" 台受信任设备；选中后可发送文件、推送剪贴板、同步或查看屏幕。"));
}

void DevicesPanel::refresh(const UiSnapshot& snap) {
    if (snap.trusted != row_ids_.size()) reload_list();
}

void DevicesPanel::on_event(const UiEvent& ev) {
    if (ev.type == "pairing_ok" || ev.type == "device_online" || ev.type == "device_offline")
        reload_list();
}

void DevicesPanel::on_send() {
    const std::string id = selected_id();
    if (id.empty()) {
        wxMessageBox(W("请先在列表中选择一台设备。"), W("发送文件"), wxOK | wxICON_INFORMATION, this);
        return;
    }
    wxFileDialog dlg(this, W("选择要发送的文件"), wxEmptyString, wxEmptyString, "*.*",
                     wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dlg.ShowModal() != wxID_OK) return;
    const wxString path = dlg.GetPath();
    frame_->append_log(W("开始发送：") + wxFileName(path).GetFullName());
    ctl_.send_file(id, U8(path), "", [this, path](bool ok, const std::string& msg) {
        frame_->append_log((ok ? W("发送完成：") : W("发送失败：")) + wxFileName(path).GetFullName() +
                           (ok ? wxString() : (W(" — ") + W(msg))));
    });
}

void DevicesPanel::on_push_clip() {
    const std::string id = selected_id();
    if (id.empty()) {
        wxMessageBox(W("请先在列表中选择一台设备。"), W("推送剪贴板"), wxOK | wxICON_INFORMATION,
                     this);
        return;
    }
    wxTextEntryDialog dlg(this, W("输入要推送到对方剪贴板的文本："), W("推送剪贴板"));
    if (dlg.ShowModal() != wxID_OK) return;
    const std::string text = U8(dlg.GetValue());
    if (text.empty()) return;
    ctl_.push_clipboard(id, text, [this](bool ok, const std::string& msg) {
        frame_->append_log((ok ? W("剪贴板已推送") : W("剪贴板推送失败：")) + W(msg));
    });
}

void DevicesPanel::on_sync() {
    const std::string id = selected_id();
    if (id.empty()) {
        wxMessageBox(W("请先在列表中选择一台设备。"), W("立即同步"), wxOK | wxICON_INFORMATION, this);
        return;
    }
    ctl_.sync_once(id, [this](bool ok, const std::string& msg) {
        frame_->append_log((ok ? W("同步完成：") : W("同步失败：")) + W(msg));
    });
}

void DevicesPanel::on_screen() {
    const std::string id = selected_id();
    if (id.empty()) {
        wxMessageBox(W("请先在列表中选择一台设备。"), W("查看屏幕"), wxOK | wxICON_INFORMATION, this);
        return;
    }
    frame_->open_screen_for(id);
}

// ---------------- 配对 ----------------

PairPanel::PairPanel(wxWindow* parent, Controller& ctl, MainFrame* frame)
    : PanelBase(parent, ctl, frame), countdown_(this) {
    build_ui();
}

void PairPanel::build_ui() {
    auto* root = new wxBoxSizer(wxVERTICAL);

    auto* selfbox = new wxStaticBoxSizer(wxVERTICAL, this, W("本机信息"));
    self_ = new wxStaticText(selfbox->GetStaticBox(), wxID_ANY, "-");
    selfbox->Add(self_, 1, wxEXPAND | wxALL, 8);
    root->Add(selfbox, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);

    auto* codebox = new wxStaticBoxSizer(wxVERTICAL, this, W("本机作为响应方：生成配对码"));
    auto* hint = new wxStaticText(
        codebox->GetStaticBox(), wxID_ANY,
        W("点击生成 6 位配对码，在 5 分钟内让对方输入该码发起配对。\n"
          "本机 IP 可在 PowerShell 执行 ipconfig 查看（局域网内互通即可）。"));
    codebox->Add(hint, 0, wxEXPAND | wxALL, 8);
    auto* code_row = new wxBoxSizer(wxHORIZONTAL);
    gen_btn_ = new wxButton(codebox->GetStaticBox(), wxID_ANY, W("生成配对码"));
    code_ = new wxStaticText(codebox->GetStaticBox(), wxID_ANY, "------");
    {
        wxFont f = code_->GetFont();
        f.SetPointSize(f.GetPointSize() + 16);
        f.MakeBold();
        code_->SetFont(f);
    }
    code_row->Add(gen_btn_, 0, wxALIGN_CENTER_VERTICAL | wxALL, 8);
    code_row->Add(code_, 0, wxALIGN_CENTER_VERTICAL | wxALL, 8);
    code_row->AddStretchSpacer(1);
    codebox->Add(code_row, 0, wxEXPAND);
    status_ = new wxStaticText(codebox->GetStaticBox(), wxID_ANY, W("等待生成配对码"));
    codebox->Add(status_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
    root->Add(codebox, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);

    auto* pairbox = new wxStaticBoxSizer(wxVERTICAL, this, W("本机作为发起方：输入对端信息"));
    auto* grid = new wxFlexGridSizer(2, 8, 8);
    grid->AddGrowableCol(1, 1);
    grid->Add(new wxStaticText(pairbox->GetStaticBox(), wxID_ANY, W("对端 IP")), 0,
              wxALIGN_CENTER_VERTICAL);
    ip_ = new wxTextCtrl(pairbox->GetStaticBox(), wxID_ANY);
    ip_->SetHint("192.168.1.20");
    grid->Add(ip_, 1, wxEXPAND);
    grid->Add(new wxStaticText(pairbox->GetStaticBox(), wxID_ANY, W("配对码")), 0,
              wxALIGN_CENTER_VERTICAL);
    input_code_ = new wxTextCtrl(pairbox->GetStaticBox(), wxID_ANY, wxEmptyString, wxDefaultPosition,
                                 wxDefaultSize, 0, wxTextValidator(wxFILTER_DIGITS));
    input_code_->SetMaxLength(6);
    input_code_->SetHint(W("对方屏幕上显示的 6 位数字"));
    grid->Add(input_code_, 1, wxEXPAND);
    pairbox->Add(grid, 1, wxEXPAND | wxALL, 8);
    pair_btn_ = new wxButton(pairbox->GetStaticBox(), wxID_ANY, W("发起配对"));
    pairbox->Add(pair_btn_, 0, wxALIGN_RIGHT | wxLEFT | wxRIGHT | wxBOTTOM, 8);
    root->Add(pairbox, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);

    auto* notebox = new wxStaticBoxSizer(wxVERTICAL, this, W("说明"));
    auto* note = new wxStaticText(notebox->GetStaticBox(), wxID_ANY,
                                  W("配对时会弹出确认框并显示双向指纹，请与对方屏幕逐位比对后再接受。\n"
                                    "配对成功后设备进入受信任库，可用于文件互传、剪贴板、同步与屏幕共享。"));
    notebox->Add(note, 1, wxEXPAND | wxALL, 8);
    root->Add(notebox, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);
    root->AddStretchSpacer(1);
    SetSizer(root);

    gen_btn_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_generate(); });
    pair_btn_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_pair(); });
    countdown_.Bind(wxEVT_TIMER, [this](wxTimerEvent&) { tick_countdown(); });
}

void PairPanel::refresh(const UiSnapshot& snap) {
    self_->SetLabel(W("设备名：" + snap.device_name + "\n设备 ID：" + snap.device_id + "\n指纹：" +
                      snap.fingerprint + "\n平台：" + snap.platform + "   服务：" +
                      (snap.running ? "运行中" : "已停止")));
}

void PairPanel::on_generate() {
    if (!frame_->service_running()) {
        frame_->start_service();
        if (!frame_->service_running()) return;
    }
    const auto code = ctl_.generate_code(300);
    code_->SetLabel(W(code.value));
    expires_at_ = code.expires_at;
    countdown_.Start(1000);
    status_->SetLabel(W("配对码已生成，剩余 " + std::to_string(std::max<std::int64_t>(
                                                    0, expires_at_ - now_secs())) +
                        " 秒"));
    frame_->append_log(W("已生成配对码（5 分钟有效，单次使用）"));
    tick_countdown();
}

void PairPanel::tick_countdown() {
    if (expires_at_ == 0) return;
    const std::int64_t left = expires_at_ - now_secs();
    if (left <= 0) {
        countdown_.Stop();
        expires_at_ = 0;
        code_->SetLabel("------");
        status_->SetLabel(W("配对码已过期，请重新生成"));
        return;
    }
    status_->SetLabel(W("配对码有效期剩余 " + std::to_string(left) + " 秒；" +
                        "对端在「配对 - 发起配对」中输入该码"));
}

void PairPanel::on_pair() {
    const std::string ip = U8(ip_->GetValue());
    const std::string code = U8(input_code_->GetValue());
    if (ip.empty() || code.empty()) {
        wxMessageBox(W("请填写对端 IP 与配对码。"), W("发起配对"), wxOK | wxICON_INFORMATION, this);
        return;
    }
    if (!frame_->service_running()) {
        frame_->start_service();
        if (!frame_->service_running()) return;
    }
    pair_btn_->Enable(false);
    status_->SetLabel(W("正在与对端握手…"));
    ctl_.pair_with(ip, code, [this](bool ok, const std::string& msg) {
        pair_btn_->Enable(true);
        status_->SetLabel((ok ? W("配对成功：") : W("配对失败：")) + W(msg));
        frame_->append_log((ok ? W("配对成功：") : W("配对失败：")) + W(msg));
    });
}

void PairPanel::on_event(const UiEvent& ev) {
    if (ev.type == "pairing_state") {
        if (ev.title == "Failed")
            status_->SetLabel(W("配对失败：") + W(ev.detail));
        else if (ev.title == "Done")
            status_->SetLabel(W("配对完成"));
    } else if (ev.type == "pairing_ok") {
        countdown_.Stop();
        expires_at_ = 0;
        code_->SetLabel("------");
        status_->SetLabel(W("配对成功：") + W(ev.title));
    }
}

} // namespace lb::gui
