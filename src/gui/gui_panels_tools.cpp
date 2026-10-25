// 文件传输 / 剪贴板 / 屏幕共享 三个面板。
#include "gui/gui_panels.h"

#include "core/platform/platform.h"
#include "gui/gui_frame.h"
#include "gui/gui_util.h"

#include <wx/filedlg.h>
#include <wx/filename.h>
#include <wx/image.h>

#include <algorithm>
#include <cmath>

namespace lb::gui {

// ---------------- 文件传输 ----------------

TransferPanel::TransferPanel(wxWindow* parent, Controller& ctl, MainFrame* frame)
    : PanelBase(parent, ctl, frame) {
    build_ui();
    reload_targets();
}

void TransferPanel::build_ui() {
    auto* root = new wxBoxSizer(wxVERTICAL);

    auto* sendbox = new wxStaticBoxSizer(wxVERTICAL, this, W("发送文件"));
    auto* grid = new wxFlexGridSizer(2, 8, 8);
    grid->AddGrowableCol(1, 1);

    grid->Add(new wxStaticText(sendbox->GetStaticBox(), wxID_ANY, W("目标设备")), 0,
              wxALIGN_CENTER_VERTICAL);
    target_ = new wxChoice(sendbox->GetStaticBox(), wxID_ANY);
    grid->Add(target_, 1, wxEXPAND);

    grid->Add(new wxStaticText(sendbox->GetStaticBox(), wxID_ANY, W("本地文件")), 0,
              wxALIGN_CENTER_VERTICAL);
    auto* path_row = new wxBoxSizer(wxHORIZONTAL);
    path_ = new wxTextCtrl(sendbox->GetStaticBox(), wxID_ANY);
    path_->SetHint(W("选择要发送的文件，或直接粘贴完整路径"));
    auto* browse = new wxButton(sendbox->GetStaticBox(), wxID_ANY, W("浏览…"));
    path_row->Add(path_, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    path_row->Add(browse, 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(path_row, 1, wxEXPAND);

    grid->Add(new wxStaticText(sendbox->GetStaticBox(), wxID_ANY, W("对方文件名")), 0,
              wxALIGN_CENTER_VERTICAL);
    remote_name_ = new wxTextCtrl(sendbox->GetStaticBox(), wxID_ANY);
    remote_name_->SetHint(W("留空则沿用本地文件名"));
    grid->Add(remote_name_, 1, wxEXPAND);

    sendbox->Add(grid, 0, wxEXPAND | wxALL, 8);

    auto* prog_row = new wxBoxSizer(wxHORIZONTAL);
    gauge_ = new wxGauge(sendbox->GetStaticBox(), wxID_ANY, 1000);
    progress_ = new wxStaticText(sendbox->GetStaticBox(), wxID_ANY, W("就绪"));
    prog_row->Add(gauge_, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 10);
    prog_row->Add(progress_, 0, wxALIGN_CENTER_VERTICAL);
    sendbox->Add(prog_row, 0, wxEXPAND | wxLEFT | wxRIGHT, 8);

    send_btn_ = new wxButton(sendbox->GetStaticBox(), wxID_ANY, W("开始发送"));
    sendbox->Add(send_btn_, 0, wxALIGN_RIGHT | wxALL, 8);
    root->Add(sendbox, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);

    auto* histbox = new wxStaticBoxSizer(wxVERTICAL, this, W("传输记录（新记录在最上方）"));
    history_ = new wxListCtrl(histbox->GetStaticBox(), wxID_ANY, wxDefaultPosition, wxDefaultSize,
                              wxLC_REPORT | wxLC_SINGLE_SEL | wxLC_HRULES);
    history_->AppendColumn(W("时间"), wxLIST_FORMAT_LEFT, 130);
    history_->AppendColumn(W("方向"), wxLIST_FORMAT_LEFT, 70);
    history_->AppendColumn(W("文件"), wxLIST_FORMAT_LEFT, 280);
    history_->AppendColumn(W("大小"), wxLIST_FORMAT_LEFT, 100);
    history_->AppendColumn(W("结果"), wxLIST_FORMAT_LEFT, 220);
    histbox->Add(history_, 1, wxEXPAND | wxALL, 6);
    root->Add(histbox, 1, wxEXPAND | wxALL, 10);

    SetSizer(root);
    browse->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_pick(); });
    send_btn_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_send(); });
}

void TransferPanel::reload_targets() {
    const wxString keep = target_->GetStringSelection();
    target_ids_.clear();
    target_->Clear();
    for (const auto& r : ctl_.trust()) {
        target_->Append(W(r.name + "  ·  " + r.device_id.substr(0, 8)));
        target_ids_.push_back(r.device_id);
    }
    if (target_->IsEmpty()) {
        target_->Append(W("（暂无受信任设备）"));
        target_ids_.push_back(std::string());
    } else if (!keep.empty() && target_->FindString(keep) != wxNOT_FOUND) {
        target_->SetStringSelection(keep);
    } else {
        target_->SetSelection(0);
    }
    const bool usable = !target_ids_.empty() && !target_ids_[0].empty();
    send_btn_->Enable(usable);
    if (!usable) progress_->SetLabel(W("请先到「配对」页添加受信任设备"));
}

void TransferPanel::refresh(const UiSnapshot& snap) {
    if (target_ids_.size() != snap.trusted ||
        (snap.trusted > 0 && target_ids_.size() == 1 && target_ids_[0].empty()))
        reload_targets();
}

void TransferPanel::on_pick() {
    wxFileDialog dlg(this, W("选择要发送的文件"), wxEmptyString, wxEmptyString, "*.*",
                     wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dlg.ShowModal() != wxID_OK) return;
    path_->SetValue(dlg.GetPath());
    if (remote_name_->IsEmpty()) remote_name_->SetValue(dlg.GetFilename());
}

void TransferPanel::on_send() {
    const int sel = target_->GetSelection();
    if (sel < 0 || sel >= static_cast<int>(target_ids_.size()) || target_ids_[sel].empty()) {
        wxMessageBox(W("请先选择目标设备；若列表为空，请先完成配对。"), W("发送文件"),
                     wxOK | wxICON_INFORMATION, this);
        return;
    }
    const std::string path = U8(path_->GetValue());
    if (path.empty()) {
        wxMessageBox(W("请选择要发送的本地文件。"), W("发送文件"), wxOK | wxICON_INFORMATION, this);
        return;
    }
    const wxFileName fn(W(path));
    if (!fn.FileExists()) {
        wxMessageBox(W("文件不存在：") + W(path), W("发送文件"), wxOK | wxICON_ERROR, this);
        return;
    }
    const std::string name = U8(remote_name_->GetValue());
    send_btn_->Enable(false);
    gauge_->SetValue(0);
    progress_->SetLabel(W("正在发送…"));
    const std::string id = target_ids_[sel];
    ctl_.send_file(id, path, name, [this, fn](bool ok, const std::string& msg) {
        send_btn_->Enable(true);
        if (!ok) {
            progress_->SetLabel(W("发送失败：") + W(msg));
            gauge_->SetValue(0);
        }
        frame_->append_log((ok ? W("发送完成：") : W("发送失败：")) + fn.GetFullName() +
                           (ok ? wxString() : (W(" — ") + W(msg))));
    });
    frame_->append_log(W("开始发送：") + fn.GetFullName());
}

void TransferPanel::on_event(const UiEvent& ev) {
    if (ev.type == "transfer_progress") {
        if (!ev.outgoing) return;
        const int permille =
            ev.b ? static_cast<int>(std::min<std::uint64_t>(1000, ev.a * 1000 / ev.b)) : 1000;
        gauge_->SetValue(permille);
        progress_->SetLabel(wxString::Format(
            "%llu%%  %s / %s", static_cast<unsigned long long>(ev.b ? (ev.a * 100 / ev.b) : 100),
            W(format_size(ev.a)), W(format_size(ev.b))));
    } else if (ev.type == "transfer_done") {
        gauge_->SetValue(1000);
        progress_->SetLabel(W("发送完成"));
        send_btn_->Enable(true);
        const long row = history_->InsertItem(0, W(host_time_now()));
        history_->SetItem(row, 1, ev.outgoing ? W("发送") : W("接收"));
        history_->SetItem(row, 2, W(ev.title));
        history_->SetItem(row, 3, W(format_size(ev.a)));
        history_->SetItem(row, 4, ev.outgoing ? W("已完成") : (W("已保存到 ") + W(ev.detail)));
    } else if (ev.type == "transfer_failed") {
        gauge_->SetValue(0);
        progress_->SetLabel(W("发送失败：") + W(ev.detail));
        send_btn_->Enable(true);
        const long row = history_->InsertItem(0, W(host_time_now()));
        history_->SetItem(row, 1, ev.outgoing ? W("发送") : W("接收"));
        history_->SetItem(row, 2, W(ev.title));
        history_->SetItem(row, 3, "-");
        history_->SetItem(row, 4, W("失败：") + W(ev.detail));
    }
}

// ---------------- 剪贴板 ----------------

ClipboardPanel::ClipboardPanel(wxWindow* parent, Controller& ctl, MainFrame* frame)
    : PanelBase(parent, ctl, frame) {
    build_ui();
    reload_targets();
}

void ClipboardPanel::build_ui() {
    auto* root = new wxBoxSizer(wxVERTICAL);

    auto* statebox = new wxStaticBoxSizer(wxVERTICAL, this, W("剪贴板同步状态"));
    state_ = new wxStaticText(statebox->GetStaticBox(), wxID_ANY, "-");
    statebox->Add(state_, 1, wxEXPAND | wxALL, 8);
    root->Add(statebox, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);

    auto* localbox = new wxStaticBoxSizer(wxVERTICAL, this, W("本机剪贴板"));
    auto* read_row = new wxBoxSizer(wxHORIZONTAL);
    auto* read_btn = new wxButton(localbox->GetStaticBox(), wxID_ANY, W("读取本地剪贴板"));
    read_row->Add(read_btn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 10);
    local_preview_ = new wxTextCtrl(localbox->GetStaticBox(), wxID_ANY, wxEmptyString,
                                    wxDefaultPosition, wxSize(-1, 110),
                                    wxTE_MULTILINE | wxTE_READONLY | wxBORDER_SIMPLE);
    local_preview_->SetHint(W("点击左侧按钮读取当前剪贴板内容（文本会显示在这里）"));
    read_row->Add(local_preview_, 1, wxEXPAND);
    localbox->Add(read_row, 0, wxEXPAND | wxALL, 8);
    root->Add(localbox, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);

    auto* pushbox = new wxStaticBoxSizer(wxVERTICAL, this, W("推送到对方"));
    auto* target_row = new wxBoxSizer(wxHORIZONTAL);
    target_row->Add(new wxStaticText(pushbox->GetStaticBox(), wxID_ANY, W("目标设备")),
                    0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
    target_ = new wxChoice(pushbox->GetStaticBox(), wxID_ANY);
    target_row->Add(target_, 1, wxALIGN_CENTER_VERTICAL);
    pushbox->Add(target_row, 0, wxEXPAND | wxALL, 8);

    push_text_ = new wxTextCtrl(pushbox->GetStaticBox(), wxID_ANY, wxEmptyString,
                                wxDefaultPosition, wxSize(-1, 110),
                                wxTE_MULTILINE | wxBORDER_SIMPLE);
    push_text_->SetHint(W("输入要写入对方剪贴板的文本"));
    pushbox->Add(push_text_, 0, wxEXPAND | wxLEFT | wxRIGHT, 8);

    auto* btn_row = new wxBoxSizer(wxHORIZONTAL);
    result_ = new wxStaticText(pushbox->GetStaticBox(), wxID_ANY, W("就绪"));
    auto* push_btn = new wxButton(pushbox->GetStaticBox(), wxID_ANY, W("推送剪贴板"));
    btn_row->Add(result_, 1, wxALIGN_CENTER_VERTICAL);
    btn_row->Add(push_btn, 0, wxALIGN_CENTER_VERTICAL);
    pushbox->Add(btn_row, 0, wxEXPAND | wxALL, 8);
    root->Add(pushbox, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);

    auto* notebox = new wxStaticBoxSizer(wxVERTICAL, this, W("说明"));
    notebox->Add(new wxStaticText(
                     notebox->GetStaticBox(), wxID_ANY,
                     W("对方设备可在「设置」中把 clipboard.mode 改为 auto 自动接收；"
                       "confirm 模式下每次接收都会弹出确认框。\n"
                       "单条内容受 clipboard.max_bytes 限制（默认 16 MiB）。")),
                 1, wxEXPAND | wxALL, 8);
    root->Add(notebox, 0, wxEXPAND | wxALL, 10);
    root->AddStretchSpacer(1);
    SetSizer(root);

    read_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_read(); });
    push_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_push(); });
}

void ClipboardPanel::reload_targets() {
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

void ClipboardPanel::refresh(const UiSnapshot& snap) {
    state_->SetLabel(W("剪贴板同步 " + std::string(snap.clipboard_enabled ? "已启用" : "已停用") +
                       "，模式：" + snap.clipboard_mode +
                       "\n说明：confirm 模式接收前弹确认框；auto 模式自动写入本地剪贴板。"));
    if (target_ids_.size() != snap.trusted + 1) reload_targets();
}

void ClipboardPanel::on_read() {
    local_preview_->SetValue(W("正在读取…"));
    ctl_.read_clipboard([this](bool ok, const std::string& mime, const std::string& text,
                               std::uint64_t size) {
        if (!ok) {
            local_preview_->SetValue(W("读取失败（可能剪贴板为空或格式不受支持）"));
            return;
        }
        if (mime == "text/plain") {
            local_preview_->SetValue(W(text));
        } else {
            local_preview_->SetValue(
                W("非文本内容：" + mime + "，" + format_size(size) + "（当前仅支持文本预览）"));
        }
    });
}

void ClipboardPanel::on_push() {
    const int sel = target_->GetSelection();
    if (sel < 0 || sel >= static_cast<int>(target_ids_.size())) return;
    const std::string text = U8(push_text_->GetValue());
    if (text.empty()) {
        wxMessageBox(W("请输入要推送的文本。"), W("推送剪贴板"), wxOK | wxICON_INFORMATION, this);
        return;
    }
    if (!frame_->service_running()) {
        wxMessageBox(W("服务未启动，请先在「概览」页启动服务。"), W("推送剪贴板"),
                     wxOK | wxICON_INFORMATION, this);
        return;
    }
    const std::string id = target_ids_[sel];
    result_->SetLabel(W("正在推送…"));
    ctl_.push_clipboard(id, text, [this](bool ok, const std::string& msg) {
        result_->SetLabel((ok ? W("推送成功：") : W("推送失败：")) + W(msg));
        frame_->append_log((ok ? W("剪贴板已推送：") : W("剪贴板推送失败：")) + W(msg));
    });
}

void ClipboardPanel::on_event(const UiEvent& ev) {
    if (ev.type == "clipboard_out")
        result_->SetLabel(W("推送成功：") + W(ev.title));
    else if (ev.type == "clipboard_fail")
        result_->SetLabel(W("推送失败：") + W(ev.detail));
    else if (ev.type == "clipboard_in")
        frame_->append_log(W("收到来自 ") + W(ev.title) + W(" 的剪贴板：") + W(ev.detail));
}

// ---------------- 屏幕共享 ----------------

ScreenPanel::ScreenPanel(wxWindow* parent, Controller& ctl, MainFrame* frame)
    : PanelBase(parent, ctl, frame), frame_timer_(this) {
    build_ui();
    reload_targets();
}

void ScreenPanel::build_ui() {
    auto* root = new wxBoxSizer(wxVERTICAL);

    auto* ctlbox = new wxStaticBoxSizer(wxHORIZONTAL, this, W("查看对方屏幕（只读）"));
    ctlbox->Add(new wxStaticText(ctlbox->GetStaticBox(), wxID_ANY, W("目标设备")), 0,
                wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
    target_ = new wxChoice(ctlbox->GetStaticBox(), wxID_ANY);
    ctlbox->Add(target_, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 10);
    ctlbox->Add(new wxStaticText(ctlbox->GetStaticBox(), wxID_ANY, W("时长（秒）")), 0,
                wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
    seconds_ = new wxSpinCtrl(ctlbox->GetStaticBox(), wxID_ANY, "10", wxDefaultPosition,
                              wxSize(80, -1), wxSP_ARROW_KEYS, 3, 120, 10);
    ctlbox->Add(seconds_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 10);
    start_btn_ = new wxButton(ctlbox->GetStaticBox(), wxID_ANY, W("开始查看"));
    stop_btn_ = new wxButton(ctlbox->GetStaticBox(), wxID_ANY, W("停止预览"));
    ctlbox->Add(start_btn_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    ctlbox->Add(stop_btn_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 10);
    control_ = new wxCheckBox(ctlbox->GetStaticBox(), wxID_ANY, W("远程控制"));
    control_->SetToolTip(W("勾选后以控制模式会话；对方需启用输入注入（Android 无障碍服务）并接受请求"));
    ctlbox->Add(control_, 0, wxALIGN_CENTER_VERTICAL);
    root->Add(ctlbox, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);

    auto* viewbox = new wxStaticBoxSizer(wxVERTICAL, this, W("实时画面"));
    preview_ = new wxStaticBitmap(viewbox->GetStaticBox(), wxID_ANY, wxNullBitmap);
    preview_->SetMinSize(wxSize(640, 360));
    viewbox->Add(preview_, 1, wxALIGN_CENTER | wxALL, 6);

    auto* keyrow = new wxBoxSizer(wxHORIZONTAL);
    keyrow->Add(new wxStaticText(viewbox->GetStaticBox(), wxID_ANY, W("系统按键：")), 0,
                wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    back_btn_ = new wxButton(viewbox->GetStaticBox(), wxID_ANY, W("返回"));
    home_btn_ = new wxButton(viewbox->GetStaticBox(), wxID_ANY, W("主页"));
    recents_btn_ = new wxButton(viewbox->GetStaticBox(), wxID_ANY, W("最近任务"));
    keyrow->Add(back_btn_, 0, wxRIGHT, 6);
    keyrow->Add(home_btn_, 0, wxRIGHT, 6);
    keyrow->Add(recents_btn_, 0);
    viewbox->Add(keyrow, 0, wxALIGN_CENTER | wxLEFT | wxRIGHT | wxBOTTOM, 6);

    info_ = new wxStaticText(viewbox->GetStaticBox(), wxID_ANY,
                             W("尚未开始。选择目标设备与时长后点击「开始查看」；"
                               "对方会弹出确认框。勾选「远程控制」后，"
                               "点击画面发送单击，按住拖动发送滑动。"));
    viewbox->Add(info_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
    root->Add(viewbox, 1, wxEXPAND | wxALL, 10);

    auto* notebox = new wxStaticBoxSizer(wxVERTICAL, this, W("说明"));
    notebox->Add(new wxStaticText(notebox->GetStaticBox(), wxID_ANY,
                                  W("默认只读查看；勾选「远程控制」并得到对方确认后，"
                                    "点击/拖动画面与系统按键会注入到对方设备（需对方支持输入注入，"
                                    "如 Android 已启用无障碍服务）。\n"
                                    "帧率受对方 screen.max_fps 限制；最新帧同时保存在数据目录的 "
                                    "screen/latest.bmp。对方必须在「设置」中启用屏幕共享。")),
                 1, wxEXPAND | wxALL, 8);
    root->Add(notebox, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
    SetSizer(root);

    start_btn_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_start(); });
    stop_btn_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_stop(); });
    back_btn_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { send_key(ScreenKey::Back); });
    home_btn_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { send_key(ScreenKey::Home); });
    recents_btn_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { send_key(ScreenKey::Recents); });
    preview_->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& ev) { on_preview_down(ev); });
    preview_->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent& ev) { on_preview_up(ev); });
    preview_->Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent&) { drag_x1_ = -1; });
    frame_timer_.Bind(wxEVT_TIMER, [this](wxTimerEvent&) { tick_frame(); });
    stop_btn_->Enable(false);
    set_control_enabled(false);
    out_dir_ = ctl_.data_dir() + "/screen";
}

void ScreenPanel::reload_targets() {
    const wxString keep = target_->GetStringSelection();
    target_ids_.clear();
    target_->Clear();
    for (const auto& r : ctl_.trust()) {
        target_->Append(W(r.name + "  ·  " + r.device_id.substr(0, 8)));
        target_ids_.push_back(r.device_id);
    }
    if (target_->IsEmpty()) {
        target_->Append(W("（暂无受信任设备）"));
        target_ids_.push_back(std::string());
    } else if (!keep.empty() && target_->FindString(keep) != wxNOT_FOUND) {
        target_->SetStringSelection(keep);
    } else {
        target_->SetSelection(0);
    }
    start_btn_->Enable(!target_ids_.empty() && !target_ids_[0].empty() && !viewing_);
}

void ScreenPanel::refresh(const UiSnapshot& snap) {
    if (target_ids_.size() != snap.trusted ||
        (snap.trusted > 0 && target_ids_.size() == 1 && target_ids_[0].empty()))
        reload_targets();
    if (!snap.screen_enabled && !viewing_)
        info_->SetLabel(W("本机已停用屏幕共享功能（设置 → 屏幕共享开关）；仍可查看他人屏幕。"));
}

void ScreenPanel::set_target(const std::string& device_id) {
    for (std::size_t i = 0; i < target_ids_.size(); ++i) {
        if (target_ids_[i] == device_id) {
            target_->SetSelection(static_cast<int>(i));
            break;
        }
    }
}

void ScreenPanel::on_start() {
    const int sel = target_->GetSelection();
    if (sel < 0 || sel >= static_cast<int>(target_ids_.size()) || target_ids_[sel].empty()) {
        wxMessageBox(W("请先选择目标设备；若列表为空，请先完成配对。"), W("屏幕共享"),
                     wxOK | wxICON_INFORMATION, this);
        return;
    }
    if (!frame_->service_running()) {
        wxMessageBox(W("服务未启动，请先在「概览」页启动服务。"), W("屏幕共享"),
                     wxOK | wxICON_INFORMATION, this);
        return;
    }
    out_dir_ = ctl_.data_dir() + "/screen";
    input_ = control_->GetValue() ? std::make_shared<ScreenInputQueue>() : nullptr;
    frame_w_ = 0; // 新会话首帧到达前不做坐标映射，避免沿用上一次分辨率
    frame_h_ = 0;
    viewing_ = true;
    start_btn_->Enable(false);
    stop_btn_->Enable(true);
    set_control_enabled(input_ != nullptr);
    info_->SetLabel(input_ ? W("正在等待对方确认（控制模式）…") : W("正在等待对方确认…"));
    frame_timer_.Start(500);
    const std::string id = target_ids_[sel];
    ctl_.view_screen(id, seconds_->GetValue(), input_, [this](bool ok, const std::string& msg) {
        viewing_ = false;
        input_.reset();
        frame_timer_.Stop();
        start_btn_->Enable(true);
        stop_btn_->Enable(false);
        set_control_enabled(false);
        info_->SetLabel((ok ? W("会话结束：") : W("查看失败：")) + W(msg));
        frame_->append_log((ok ? W("屏幕查看结束：") : W("屏幕查看失败：")) + W(msg));
        tick_frame();
    });
    frame_->append_log(W("开始查看屏幕，时长 ") +
                       wxString::Format("%d", seconds_->GetValue()) + W(" 秒") +
                       (input_ ? W("（远程控制）") : W("（只读）")));
}

void ScreenPanel::on_stop() {
    viewing_ = false;
    input_.reset();
    frame_timer_.Stop();
    start_btn_->Enable(true);
    stop_btn_->Enable(false);
    set_control_enabled(false);
    info_->SetLabel(W("已停止本地预览（会话将在设定时长后自然结束）"));
    frame_->append_log(W("[屏幕] 已停止预览"));
}

void ScreenPanel::set_control_enabled(bool enabled) {
    back_btn_->Enable(enabled);
    home_btn_->Enable(enabled);
    recents_btn_->Enable(enabled);
}

bool ScreenPanel::map_to_frame(int view_x, int view_y, int& out_x, int& out_y) const {
    if (!input_ || frame_w_ <= 0 || frame_h_ <= 0) return false;
    const wxSize size = preview_->GetSize();
    if (size.x <= 0 || size.y <= 0) return false;
    if (view_x < 0 || view_y < 0 || view_x >= size.x || view_y >= size.y) return false;
    out_x = view_x * frame_w_ / size.x;
    out_y = view_y * frame_h_ / size.y;
    return true;
}

void ScreenPanel::on_preview_down(wxMouseEvent& ev) {
    if (!input_) return;
    drag_x1_ = ev.GetX();
    drag_y1_ = ev.GetY();
    drag_x2_ = ev.GetX();
    drag_y2_ = ev.GetY();
    if (!preview_->HasCapture()) preview_->CaptureMouse();
}

void ScreenPanel::on_preview_up(wxMouseEvent& ev) {
    if (!input_) return;
    drag_x2_ = ev.GetX();
    drag_y2_ = ev.GetY();
    if (preview_->HasCapture()) preview_->ReleaseMouse();
    const int x1 = drag_x1_;
    const int y1 = drag_y1_;
    drag_x1_ = -1;
    drag_y1_ = -1;
    if (x1 < 0 || y1 < 0) return;

    const int dx = drag_x2_ - x1;
    const int dy = drag_y2_ - y1;
    if (std::abs(dx) <= 6 && std::abs(dy) <= 6) {
        int fx = 0;
        int fy = 0;
        if (map_to_frame(x1, y1, fx, fy)) send_tap(fx, fy);
        return;
    }
    int fx1 = 0;
    int fy1 = 0;
    int fx2 = 0;
    int fy2 = 0;
    if (map_to_frame(x1, y1, fx1, fy1) && map_to_frame(drag_x2_, drag_y2_, fx2, fy2))
        send_swipe(fx1, fy1, fx2, fy2);
}

void ScreenPanel::send_tap(int x, int y) {
    if (!input_) return;
    ScreenInput in;
    in.type = ScreenInputType::Tap;
    in.x1 = x;
    in.y1 = y;
    input_->push(in);
    frame_->append_log(wxString::Format(W("控制：单击 (%d, %d)"), x, y));
}

void ScreenPanel::send_swipe(int x1, int y1, int x2, int y2) {
    if (!input_) return;
    ScreenInput in;
    in.type = ScreenInputType::Swipe;
    in.x1 = x1;
    in.y1 = y1;
    in.x2 = x2;
    in.y2 = y2;
    in.duration_ms = 200;
    input_->push(in);
    frame_->append_log(wxString::Format(W("控制：滑动 (%d, %d) → (%d, %d)"), x1, y1, x2, y2));
}

void ScreenPanel::send_key(ScreenKey key) {
    if (!input_) {
        wxMessageBox(W("当前不是控制会话：请勾选「远程控制」后重新开始查看。"), W("屏幕控制"),
                     wxOK | wxICON_INFORMATION, this);
        return;
    }
    ScreenInput in;
    in.type = ScreenInputType::Key;
    in.key = key;
    input_->push(in);
    const char* name = key == ScreenKey::Back ? "返回"
                       : key == ScreenKey::Home ? "主页"
                                               : "最近任务";
    frame_->append_log(wxString::Format(W("控制：按键 %s"), W(name)));
}

void ScreenPanel::tick_frame() {
    if (!viewing_) return;
    const std::string path = out_dir_ + "/latest.bmp";
    if (!pal::file_exists(path)) return;
    wxImage img;
    {
        wxLogNull no_log; // 帧文件可能正在原子替换，读取失败时静默跳过
        if (!img.LoadFile(W(path), wxBITMAP_TYPE_BMP)) return;
    }
    if (!img.IsOk() || img.GetWidth() <= 0 || img.GetHeight() <= 0) return;
    const int ow = img.GetWidth();
    const int oh = img.GetHeight();
    frame_w_ = ow;
    frame_h_ = oh;
    const double scale = std::min(640.0 / ow, 360.0 / oh);
    const int w = std::max(1, static_cast<int>(std::lround(ow * scale)));
    const int h = std::max(1, static_cast<int>(std::lround(oh * scale)));
    if (w != ow || h != oh) img = img.Scale(w, h, wxIMAGE_QUALITY_HIGH);
    preview_->SetBitmap(wxBitmap(img));
    // 控件尺寸与位图尺寸保持一致（控件本身由 sizer 居中），使预览坐标可直接按比例反算。
    if (preview_->GetMinSize() != wxSize(w, h)) {
        preview_->SetMinSize(wxSize(w, h));
        Layout();
    }
    const wxString suffix = input_ ? W("（控制中）") : wxString();
    info_->SetLabel(wxString::Format(W("画面 %dx%d，缩放显示 %dx%d%s；等待下一帧…"), ow, oh, w, h,
                                     suffix));
}

void ScreenPanel::on_event(const UiEvent& ev) {
    if (ev.type == "screen_started") {
        info_->SetLabel(W("会话已建立：") + W(ev.title) + W("，等待首帧…"));
    } else if (ev.type == "screen_frame") {
        if (viewing_)
            info_->SetLabel(W("已接收 " + std::to_string(ev.b + 1) + " 帧，尺寸 " + ev.title + "，") +
                            W(format_size(ev.a)));
    } else if (ev.type == "screen_ended") {
        if (viewing_) {
            viewing_ = false;
            frame_timer_.Stop();
            start_btn_->Enable(true);
            stop_btn_->Enable(false);
            info_->SetLabel(W("对方结束会话：" + ev.detail + "（共 " + std::to_string(ev.a) + " 帧）"));
        }
    } else if (ev.type == "screen_failed") {
        if (viewing_) {
            viewing_ = false;
            frame_timer_.Stop();
            start_btn_->Enable(true);
            stop_btn_->Enable(false);
            info_->SetLabel(W("屏幕共享失败：") + W(ev.detail));
        }
    }
}

} // namespace lb::gui
