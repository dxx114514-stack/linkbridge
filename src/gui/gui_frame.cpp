#include "gui/gui_frame.h"

#include "gui/gui_panels.h"
#include "gui/gui_util.h"

#include <wx/dcscreen.h>
#include <wx/menu.h>

#include <chrono>

namespace lb::gui {
namespace {
enum {
    ID_START_SERVICE = wxID_HIGHEST + 1,
    ID_STOP_SERVICE,
    ID_RESTART_SERVICE,
    ID_OPEN_GUIDE,
    ID_ABOUT,
};
} // namespace

MainFrame::MainFrame(Controller& ctl)
    : wxFrame(nullptr, wxID_ANY, W("LinkBridge 连桥 — 图形界面"), wxDefaultPosition,
              wxSize(1060, 740)),
      ctl_(ctl) {
    SetMinSize(wxSize(860, 600));
    build_ui();
    build_menu();

    CreateStatusBar(3);
    const int widths[] = {-3, -4, -3};
    SetStatusWidths(3, widths);
    SetStatusText(W("正在初始化…"), 0);

    // 核心事件 → UI 线程（Controller 内部已用 CallAfter 投递）
    ctl_.set_event_sink([this](const UiEvent& ev) { dispatch_event(ev); });
    ctl_.set_ask_handler([this](const std::string& title, const std::string& message) {
        wxMessageDialog dlg(this, W(message), W(title), wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION);
        dlg.SetYesNoLabels(W("接受"), W("拒绝"));
        return dlg.ShowModal() == wxID_YES;
    });

    refresh_timer_.SetOwner(this);
    Bind(wxEVT_TIMER, [this](wxTimerEvent&) { refresh_ui(); }, refresh_timer_.GetId());
    refresh_timer_.Start(1000);
    refresh_ui();

    Bind(wxEVT_CLOSE_WINDOW, &MainFrame::on_close, this);
}

MainFrame::~MainFrame() {
    refresh_timer_.Stop();
    ctl_.set_event_sink(nullptr);
    ctl_.set_ask_handler(nullptr);
}

void MainFrame::build_ui() {
    book_ = new wxListbook(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLB_LEFT);

    status_ = new StatusPanel(book_, ctl_, this);
    devices_ = new DevicesPanel(book_, ctl_, this);
    pair_ = new PairPanel(book_, ctl_, this);
    transfer_ = new TransferPanel(book_, ctl_, this);
    clipboard_ = new ClipboardPanel(book_, ctl_, this);
    screen_ = new ScreenPanel(book_, ctl_, this);
    sync_ = new SyncPanel(book_, ctl_, this);
    settings_ = new SettingsPanel(book_, ctl_, this);
    help_ = new HelpPanel(book_, ctl_, this);

    book_->AddPage(status_, W("概览"));
    book_->AddPage(devices_, W("设备"));
    book_->AddPage(pair_, W("配对"));
    book_->AddPage(transfer_, W("文件传输"));
    book_->AddPage(clipboard_, W("剪贴板"));
    book_->AddPage(screen_, W("屏幕共享与控制"));
    book_->AddPage(sync_, W("文件夹同步"));
    book_->AddPage(settings_, W("设置"));
    book_->AddPage(help_, W("帮助"));

    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(book_, 1, wxEXPAND);
    SetSizer(sizer);
    book_->SetSelection(0);
    Layout();
}

void MainFrame::build_menu() {
    auto* file = new wxMenu();
    file->Append(wxID_EXIT, W("退出\tAlt+F4"));

    auto* service = new wxMenu();
    service->Append(ID_START_SERVICE, W("启动服务\tF5"));
    service->Append(ID_STOP_SERVICE, W("停止服务\tShift+F5"));
    service->Append(ID_RESTART_SERVICE, W("重启服务"));

    auto* help = new wxMenu();
    help->Append(ID_OPEN_GUIDE, W("操作指引\tF1"));
    help->Append(ID_ABOUT, W("关于 LinkBridge"));

    auto* bar = new wxMenuBar();
    bar->Append(file, W("文件"));
    bar->Append(service, W("服务"));
    bar->Append(help, W("帮助"));
    SetMenuBar(bar);

    Bind(wxEVT_MENU, [this](wxCommandEvent&) { Close(true); }, wxID_EXIT);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { start_service(); }, ID_START_SERVICE);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { stop_service(); }, ID_STOP_SERVICE);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { restart_service(); }, ID_RESTART_SERVICE);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { select_page(8); }, ID_OPEN_GUIDE);
    Bind(wxEVT_MENU,
         [this](wxCommandEvent&) {
             wxMessageBox(
                 W("LinkBridge 连桥 v0.1\n\n局域网内的跨设备协作：配对、文件互传、剪贴板同步、"
                   "文件夹同步与只读屏幕共享。\n\n核心库与命令行共用同一套协议与安全实现。"),
                 W("关于 LinkBridge"), wxOK | wxICON_INFORMATION, this);
         },
         ID_ABOUT);

    // 快捷键
    wxAcceleratorEntry entries[3];
    entries[0].Set(wxACCEL_NORMAL, WXK_F1, ID_OPEN_GUIDE);
    entries[1].Set(wxACCEL_NORMAL, WXK_F5, ID_START_SERVICE);
    entries[2].Set(wxACCEL_SHIFT, WXK_F5, ID_STOP_SERVICE);
    wxAcceleratorTable accel(3, entries);
    SetAcceleratorTable(accel);
}

void MainFrame::start_service() {
    std::string error;
    if (!ctl_.start(&error)) {
        append_log(W("[服务] 启动失败：") + W(error));
        flash_status(W("启动失败：") + W(error));
        return;
    }
    append_log(W("[服务] 已启动，六个通道开始监听"));
    flash_status(W("服务已启动"));
    refresh_ui();
}

void MainFrame::stop_service() {
    ctl_.stop();
    append_log(W("[服务] 已停止"));
    flash_status(W("服务已停止"));
    refresh_ui();
}

void MainFrame::restart_service() {
    std::string error;
    if (!ctl_.restart(&error)) {
        append_log(W("[服务] 重启失败：") + W(error));
        flash_status(W("重启失败"));
        return;
    }
    append_log(W("[服务] 已按新配置重启"));
    flash_status(W("服务已重启"));
    refresh_ui();
}

void MainFrame::append_log(const wxString& text) {
    if (status_) status_->append_log(text);
}

void MainFrame::flash_status(const wxString& text) {
    flash_ = text;
    flash_until_ = now_secs() + 4;
    SetStatusText(text, 2);
}

void MainFrame::select_page(int index) {
    if (book_ && index >= 0 && index < static_cast<int>(book_->GetPageCount()))
        book_->SetSelection(index);
}

void MainFrame::open_screen_for(const std::string& device_id) {
    select_page(5);
    if (screen_) screen_->set_target(device_id);
}

void MainFrame::dispatch_event(const UiEvent& ev) {
    if (status_) status_->on_event(ev);
    if (devices_) devices_->on_event(ev);
    if (pair_) pair_->on_event(ev);
    if (transfer_) transfer_->on_event(ev);
    if (clipboard_) clipboard_->on_event(ev);
    if (screen_) screen_->on_event(ev);
    if (sync_) sync_->on_event(ev);
    refresh_ui();
}

void MainFrame::refresh_ui() {
    const UiSnapshot snap = ctl_.snapshot();
    SetStatusText(snap.running ? W("服务：运行中") : W("服务：已停止"), 0);
    SetStatusText(W("端口 " + std::to_string(snap.pair_port) + " / " +
                    std::to_string(snap.heartbeat_port) + " / " +
                    std::to_string(snap.transfer_port) + " / " +
                    std::to_string(snap.clipboard_port) + " / " +
                    std::to_string(snap.screen_port) + " / " +
                    std::to_string(snap.sync_port)),
                  1);
    if (!flash_.empty() && now_secs() < flash_until_) {
        SetStatusText(flash_, 2);
    } else {
        flash_.clear();
        SetStatusText(W("受信任 " + std::to_string(snap.trusted) + " 台 · 在线 " +
                        std::to_string(snap.online) + " 台"),
                      2);
    }

    if (status_) status_->refresh(snap);
    if (devices_) devices_->refresh(snap);
    if (pair_) pair_->refresh(snap);
    if (transfer_) transfer_->refresh(snap);
    if (clipboard_) clipboard_->refresh(snap);
    if (screen_) screen_->refresh(snap);
    if (sync_) sync_->refresh(snap);
    if (settings_) settings_->refresh(snap);
    if (help_) help_->refresh(snap);
}

bool MainFrame::capture_to(const std::string& path) {
    Update();
#ifdef __WXMSW__
    // 用 PrintWindow 从窗口自身绘制，避免被其他窗口遮挡导致截图串内容
    HWND hwnd = static_cast<HWND>(GetHandle());
    RECT rc{};
    if (!::GetWindowRect(hwnd, &rc)) return false;
    const int width = rc.right - rc.left;
    const int height = rc.bottom - rc.top;
    if (width <= 0 || height <= 0) return false;

    HDC screen_dc = ::GetDC(nullptr);
    HDC mem_dc = ::CreateCompatibleDC(screen_dc);
    HBITMAP hbmp = ::CreateCompatibleBitmap(screen_dc, width, height);
    if (!mem_dc || !hbmp) {
        if (mem_dc) ::DeleteDC(mem_dc);
        if (hbmp) ::DeleteObject(hbmp);
        ::ReleaseDC(nullptr, screen_dc);
        return false;
    }
    HGDIOBJ old = ::SelectObject(mem_dc, hbmp);
    // PW_RENDERFULLCONTENT 仅 Win8.1+ 支持，失败时退回经典 PrintWindow
    BOOL painted = ::PrintWindow(hwnd, mem_dc, 0x00000002);
    if (!painted) painted = ::PrintWindow(hwnd, mem_dc, 0);
    ::SelectObject(mem_dc, old);
    ::DeleteDC(mem_dc);
    ::ReleaseDC(nullptr, screen_dc);
    if (!painted) {
        ::DeleteObject(hbmp);
        return false;
    }

    BITMAP native{};
    ::GetObject(hbmp, sizeof(native), &native);
    wxBitmap bitmap;
    // InitFromHBITMAP 接管 HBITMAP 所有权
    if (!bitmap.InitFromHBITMAP(static_cast<WXHBITMAP>(hbmp), width, height,
                                native.bmBitsPixel == 32 ? 32 : 24)) {
        ::DeleteObject(hbmp);
        return false;
    }
    return bitmap.ConvertToImage().SaveFile(W(path), wxBITMAP_TYPE_PNG);
#else
    wxScreenDC screen;
    if (!screen.IsOk()) return false;
    const wxRect rect = GetScreenRect();
    wxBitmap bitmap(rect.GetWidth(), rect.GetHeight(), 24);
    {
        wxMemoryDC mem(bitmap);
        mem.Blit(0, 0, rect.GetWidth(), rect.GetHeight(), &screen, rect.GetX(), rect.GetY());
    }
    return bitmap.ConvertToImage().SaveFile(W(path), wxBITMAP_TYPE_PNG);
#endif
}

void MainFrame::on_close(wxCloseEvent& ev) {
    refresh_timer_.Stop();
    ctl_.set_closing(true);
    ctl_.stop();
    ev.Skip();
}

} // namespace lb::gui
