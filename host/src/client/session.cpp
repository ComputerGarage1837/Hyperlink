#include "session.h"

#include <windowsx.h>

#include <algorithm>
#include <thread>

#include "../monitors.h"
#include "../settings.h"
#include "hyperlink/common.h"
#include "ui.h"
#include "../resource.h"
#include "hyperlink/json.h"

namespace {

enum : UINT {
    WM_S_CONNECTED = WM_APP + 10,  // lParam: std::string* error (empty = ok)
    WM_S_MONITORS,                 // lParam: std::vector<hl::MonitorInfo>*
    WM_S_STREAM_ERROR,             // lParam: std::string*
    WM_S_CURSOR_SHAPE,             // lParam: hl::CursorShape*
    WM_S_CURSOR_POS,               // wParam: monitor, lParam: visible
    WM_S_DISCONNECTED,             // lParam: std::string*
};

enum : UINT {
    IDM_ALL = 100,
    IDM_SCREEN = 101,     // + index
    IDM_HIDE = 200,       // + index
    IDM_POPOUT = 300,     // + index
    IDM_FULLSCREEN = 400,
    IDM_STATS,
    IDM_TASKMGR,
    IDM_WINKEY,
    IDM_ALTTAB,
    IDM_DISCONNECT,
    IDM_PIN,
};

const wchar_t* kMain = L"HyperlinkSession";
const wchar_t* kPopout = L"HyperlinkPopout";
std::set<HWND> gWindows;  // session and pop-out windows, for the keyboard hook
std::map<std::string, HWND> gByDevice;  // open main session window per saved device
HHOOK gHook = nullptr;
int gOpen = 0;

SessionWindow* ownerOf(HWND h) {
    return reinterpret_cast<SessionWindow*>(GetWindowLongPtrW(h, GWLP_USERDATA));
}

int localMaxHz() {
    int best = 60;
    DISPLAY_DEVICEW dd{sizeof dd};
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dd, 0); i++) {
        DEVMODEW dm{};
        dm.dmSize = sizeof dm;
        if (EnumDisplaySettingsW(dd.DeviceName, ENUM_CURRENT_SETTINGS, &dm))
            best = std::max<int>(best, (int)dm.dmDisplayFrequency);
    }
    return std::min(best, 240);
}

// Keys Windows would otherwise keep for itself (Win, Alt+Tab, Ctrl+Esc) go to the remote PC
// while one of our windows has focus.
LRESULT CALLBACK keyboardHook(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION) {
        HWND fg = GetForegroundWindow();
        if (gWindows.count(fg)) {
            auto* k = reinterpret_cast<KBDLLHOOKSTRUCT*>(lp);
            bool alt = (k->flags & LLKHF_ALTDOWN) != 0;
            bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
            bool grab = k->vkCode == VK_LWIN || k->vkCode == VK_RWIN || (alt && k->vkCode == VK_TAB) ||
                        (alt && k->vkCode == VK_ESCAPE) || (ctrl && k->vkCode == VK_ESCAPE);
            if (grab && !(k->flags & LLKHF_INJECTED)) {
                bool down = wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN;
                PostMessageW(fg, WM_APP + 30, k->vkCode, down);
                return 1;
            }
        }
    }
    return CallNextHookEx(gHook, code, wp, lp);
}

}  // namespace

int SessionWindow::openCount() { return gOpen; }

bool SessionWindow::bringToFront(const std::string& deviceId) {
    auto it = gByDevice.find(deviceId);
    if (it == gByDevice.end() || !IsWindow(it->second)) return false;
    if (IsIconic(it->second)) ShowWindow(it->second, SW_RESTORE);
    SetForegroundWindow(it->second);
    return true;
}

void SessionWindow::registerClasses(HINSTANCE inst) {
    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(RGB(5, 6, 15));
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.lpszClassName = kMain;
    RegisterClassExW(&wc);
    wc.lpfnWndProc = popoutProc;
    wc.lpszClassName = kPopout;
    RegisterClassExW(&wc);
    VideoView::registerClass(inst);
}

void SessionWindow::open(const SavedDevice& d) { new SessionWindow(d); }

SessionWindow::SessionWindow(const SavedDevice& d) : device_(d), client_(this) {
    gOpen++;
    hwnd_ = CreateWindowExW(0, kMain, fromUtf8(d.name).c_str(), WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT,
                            CW_USEDEFAULT, 1280, 760, nullptr, nullptr, GetModuleHandleW(nullptr), this);
    status_ = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_CENTER, 0, 0, 10, 10, hwnd_, nullptr,
                              GetModuleHandleW(nullptr), nullptr);
    SendMessageW(status_, WM_SETFONT, (WPARAM)ui::font(hwnd_), TRUE);
    gWindows.insert(hwnd_);
    if (!d.id.empty()) gByDevice[d.id] = hwnd_;
    web::styleWindow(hwnd_);
    toolbar_ = std::make_unique<web::Panel>(
        hwnd_, web::loadHtml(IDR_TOOLBAR_HTML), [this](const std::string& m) { onToolbarMessage(m); },
        [this](bool ok) {
            if (!ok) {
                useMenu_ = true;
                toolbar_.reset();
                buildMenu();
            }
        });
    toolbar_->show(false);
    if (!gHook) gHook = SetWindowsHookExW(WH_KEYBOARD_LL, keyboardHook, GetModuleHandleW(nullptr), 0);
    buildMenu();
    ShowWindow(hwnd_, SW_SHOW);
    SetTimer(hwnd_, 1, 1000, nullptr);
    connect();
}

SessionWindow::~SessionWindow() {
    gOpen--;
}

void SessionWindow::setStatus(const std::wstring& s) {
    SetWindowTextW(status_, s.c_str());
    ShowWindow(status_, s.empty() ? SW_HIDE : SW_SHOW);
    layout();
}

void SessionWindow::connect() {
    setStatus(L"Connecting to " + fromUtf8(device_.name) + L"...");
    SavedDevice d = device_;
    HWND h = hwnd_;
    hl::Client* client = &client_;
    std::string* hostName = &hostName_;
    uint32_t* codecs = &hostCodecs_;
    {
        std::lock_guard<std::mutex> lock(life_);
        if (connecting_) return;
        connecting_ = true;
    }
    std::thread([this, d, h, client, hostName, codecs] {
        // Home network first (found by id), then the host's Tailscale address, then what the user typed.
        std::vector<std::string> tries;
        std::string lan;
        if (PresenceScanner::locate(d.hostId, lan)) tries.push_back(lan);
        if (!d.remoteAddress.empty()) tries.push_back(d.remoteAddress);
        if (!d.address.empty()) tries.push_back(d.address);
        auto* err = new std::string;
        if (tries.empty()) {
            *err = "Can't find " + d.name + " on this network. Is it on and running Hyperlink? To reach it "
                   "from anywhere, install Tailscale on both devices and connect once at home.";
        } else {
            HostSettings me = HostSettings::load();
            hl::Hello hello;
            hello.clientName = me.name;
            hello.clientId = me.hostId;
            hello.pin = d.pin;
            hello.displayWidth = (uint16_t)GetSystemMetrics(SM_CXSCREEN);
            hello.displayHeight = (uint16_t)GetSystemMetrics(SM_CYSCREEN);
            hello.displayHz = (uint16_t)localMaxHz();
            hello.codecMask = VideoView::decodableCodecs();
            hl::Welcome w;
            for (auto& addr : tries) {
                *err = client->connect(addr, d.port, hello, w, 4000);
                if (err->empty() || err->find("PIN") != std::string::npos) break;
            }
            if (err->empty()) {
                *hostName = w.hostName;
                *codecs = w.codecMask;
                // Remember the host's permanent id so it's found by id from now on.
                // Remember the host's permanent id and Tailscale address for next time.
                std::string remote = w.remoteAddresses.empty() ? d.remoteAddress : w.remoteAddresses[0];
                if (d.hostId != w.hostId || d.remoteAddress != remote) {
                    SavedDevice nd = d;
                    nd.hostId = w.hostId;
                    nd.remoteAddress = remote;
                    if (!nd.id.empty()) devices::save(nd);
                }
                PostMessageW(h, WM_S_MONITORS, 0, (LPARAM) new std::vector<hl::MonitorInfo>(w.monitors));
            }
        }
        std::unique_lock<std::mutex> lock(life_);
        connecting_ = false;
        if (closed_) {  // the window was closed while we were connecting
            lock.unlock();
            delete err;
            delete this;
            return;
        }
        PostMessageW(h, WM_S_CONNECTED, 0, (LPARAM)err);
    }).detach();
}

// ---- listener: hop to the window thread

void SessionWindow::onMonitors(const std::vector<hl::MonitorInfo>& m) {
    PostMessageW(hwnd_, WM_S_MONITORS, 0, (LPARAM) new std::vector<hl::MonitorInfo>(m));
}
void SessionWindow::onStreamStarted(const hl::StreamStarted&) {}
void SessionWindow::onStreamError(uint8_t, const std::string& msg) {
    PostMessageW(hwnd_, WM_S_STREAM_ERROR, 0, (LPARAM) new std::string(msg));
}
void SessionWindow::onCursorShape(const hl::CursorShape& c) {
    PostMessageW(hwnd_, WM_S_CURSOR_SHAPE, 0, (LPARAM) new hl::CursorShape(c));
}
void SessionWindow::onCursorPos(uint32_t mon, uint16_t, uint16_t, bool visible) {
    PostMessageW(hwnd_, WM_S_CURSOR_POS, mon, visible);
}
void SessionWindow::onDisconnected(const std::string& reason) {
    PostMessageW(hwnd_, WM_S_DISCONNECTED, 0, (LPARAM) new std::string(reason));
}

// ---- screens and layout

SessionWindow::Screen* SessionWindow::screenFor(uint32_t id) {
    for (auto& s : screens_)
        if (s.info.id == id) return &s;
    return nullptr;
}

void SessionWindow::setMonitors(const std::vector<hl::MonitorInfo>& m) {
    stopAll();
    for (auto& s : screens_)
        if (s.popout) DestroyWindow(s.popout);
    screens_.clear();
    auto hiddenList = devices::hiddenScreens(device_.id);
    hidden_ = std::set<uint32_t>(hiddenList.begin(), hiddenList.end());
    for (size_t i = 0; i < m.size(); i++) {
        Screen s;
        s.info = m[i];
        s.streamId = (uint8_t)i;
        VideoView::Input in;
        in.move = [this](uint32_t mon, uint16_t x, uint16_t y) { client_.mouseAbs(mon, x, y); };
        in.button = [this](uint8_t b, bool down) { client_.mouseButton(b, down); };
        in.scroll = [this](int dy, int dx) { client_.scroll(dy, dx); };
        HWND self = hwnd_;
        in.focusParent = [self] {
            HWND top = GetAncestor(GetFocus() ? GetFocus() : self, GA_ROOT);
            if (!top) top = self;
            SetFocus(top);
        };
        uint32_t monId = m[i].id;
        in.hover = [this, monId](int x, int y) {
            // Pointer at the very top of the window: slide the toolbar in.
            Screen* sc = screenFor(monId);
            if (!sc || sc->popout || toolbarShown_) return;
            POINT p{x, y};
            MapWindowPoints(sc->view->hwnd(), hwnd_, &p, 1);
            if (p.y <= ui::scale(hwnd_, 6)) showToolbar(true);
        };
        s.view = std::make_shared<VideoView>(hwnd_, m[i], in);
        screens_.push_back(std::move(s));
    }
    if (focused_ >= (int)screens_.size()) focused_ = -1;
    buildMenu();
    layout();
    syncStreams();
}

void SessionWindow::layout() {
    RECT rc;
    GetClientRect(hwnd_, &rc);
    int W = rc.right, H = rc.bottom;
    bool showingStatus = IsWindowVisible(status_);
    MoveWindow(status_, 0, H / 2 - ui::scale(hwnd_, 40), W, ui::scale(hwnd_, 80), TRUE);

    std::vector<Screen*> shown;
    for (int i = 0; i < (int)screens_.size(); i++) {
        Screen& s = screens_[i];
        if (s.popout) continue;
        bool visible = focused_ >= 0 ? i == focused_ : !hidden_.count(s.info.id);
        ShowWindow(s.view->hwnd(), visible && !showingStatus ? SW_SHOW : SW_HIDE);
        if (visible) shown.push_back(&s);
    }
    if (shown.empty() || showingStatus) return;
    int gap = shown.size() > 1 ? ui::scale(hwnd_, 4) : 0;
    long minX = LONG_MAX, minY = LONG_MAX, maxX = LONG_MIN, maxY = LONG_MIN;
    for (auto* s : shown) {
        minX = std::min<long>(minX, s->info.x);
        minY = std::min<long>(minY, s->info.y);
        maxX = std::max<long>(maxX, s->info.x + (long)s->info.width);
        maxY = std::max<long>(maxY, s->info.y + (long)s->info.height);
    }
    if (shown.size() == 1) { minX = shown[0]->info.x; minY = shown[0]->info.y; }
    double sc = std::min((double)W / std::max(1L, maxX - minX), (double)H / std::max(1L, maxY - minY));
    int ox = (int)((W - (maxX - minX) * sc) / 2), oy = (int)((H - (maxY - minY) * sc) / 2);
    for (auto* s : shown) {
        int x = ox + (int)((s->info.x - minX) * sc), y = oy + (int)((s->info.y - minY) * sc);
        int w = (int)(s->info.width * sc), h = (int)(s->info.height * sc);
        MoveWindow(s->view->hwnd(), x + gap / 2, y + gap / 2, w - gap, h - gap, TRUE);
    }
}

uint8_t SessionWindow::chooseCodec() const {
    uint32_t both = hostCodecs_ & VideoView::decodableCodecs();
    if (both & (1u << hl::CODEC_HEVC)) return hl::CODEC_HEVC;
    if (both & (1u << hl::CODEC_H264)) return hl::CODEC_H264;
    return hl::CODEC_H264;
}

void SessionWindow::syncStreams() {
    if (!connected_) return;
    uint64_t totalArea = 0;
    auto wanted = [&](int i) {
        const Screen& s = screens_[i];
        if (s.popout) return !IsIconic(s.popout);
        if (IsIconic(hwnd_)) return false;
        return focused_ >= 0 ? i == focused_ : !hidden_.count(s.info.id);
    };
    for (int i = 0; i < (int)screens_.size(); i++)
        if (wanted(i)) totalArea += (uint64_t)screens_[i].info.width * screens_[i].info.height;
    const int totalKbps = 80000;
    for (int i = 0; i < (int)screens_.size(); i++) {
        Screen& s = screens_[i];
        bool want = wanted(i);
        if (want && !s.running) {
            hl::StartStream req;
            req.streamId = s.streamId;
            req.monitorId = s.info.id;
            req.maxWidth = 0;  // the PC's full resolution
            req.maxHeight = 0;
            req.fps = (uint16_t)localMaxHz();
            double share = totalArea ? (double)s.info.width * s.info.height / totalArea : 1.0;
            req.bitrateKbps = (uint32_t)std::max(8000.0, totalKbps * share);
            req.codec = chooseCodec();
            req.fecPercent = 15;
            client_.startStream(req, s.view);
            s.running = true;
        } else if (!want && s.running) {
            client_.stopStream(s.streamId);
            s.running = false;
        }
    }
}

void SessionWindow::stopAll() {
    for (auto& s : screens_)
        if (s.running) {
            client_.stopStream(s.streamId);
            s.running = false;
        }
}

void SessionWindow::buildMenu() {
    if (!useMenu_) {
        pushToolbar();
        return;
    }
    HMENU bar = CreateMenu();
    HMENU view = CreatePopupMenu();
    AppendMenuW(view, MF_STRING | (focused_ < 0 ? MF_CHECKED : 0), IDM_ALL, L"All screens");
    for (size_t i = 0; i < screens_.size(); i++) {
        std::wstring n = std::to_wstring(i + 1) + L"  " + fromUtf8(screens_[i].info.name);
        AppendMenuW(view, MF_STRING | ((int)i == focused_ ? MF_CHECKED : 0), IDM_SCREEN + i, n.c_str());
    }
    if (screens_.size() > 1) {
        AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
        HMENU show = CreatePopupMenu();
        HMENU pop = CreatePopupMenu();
        for (size_t i = 0; i < screens_.size(); i++) {
            std::wstring n = std::to_wstring(i + 1) + L"  " + fromUtf8(screens_[i].info.name);
            AppendMenuW(show, MF_STRING | (hidden_.count(screens_[i].info.id) ? 0 : MF_CHECKED), IDM_HIDE + i, n.c_str());
            AppendMenuW(pop, MF_STRING | (screens_[i].popout ? MF_GRAYED : 0), IDM_POPOUT + i, n.c_str());
        }
        AppendMenuW(view, MF_POPUP, (UINT_PTR)show, L"Show in All screens");
        AppendMenuW(view, MF_POPUP, (UINT_PTR)pop, L"Open screen in its own window");
    }
    AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(view, MF_STRING, IDM_FULLSCREEN, L"Full screen\tCtrl+Alt+Enter");
    AppendMenuW(view, MF_STRING | (showStats_ ? MF_CHECKED : 0), IDM_STATS, L"Show stats in title");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)view, L"View");
    HMENU keys = CreatePopupMenu();
    AppendMenuW(keys, MF_STRING, IDM_TASKMGR, L"Task Manager (Ctrl+Shift+Esc)");
    AppendMenuW(keys, MF_STRING, IDM_WINKEY, L"Start menu (Win)");
    AppendMenuW(keys, MF_STRING, IDM_ALTTAB, L"Switch app (Alt+Tab)");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)keys, L"Keys");
    HMENU conn = CreatePopupMenu();
    AppendMenuW(conn, MF_STRING, IDM_PIN, L"Change PIN for this device...");
    AppendMenuW(conn, MF_STRING, IDM_DISCONNECT, L"Disconnect");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)conn, L"Connection");
    HMENU old = GetMenu(hwnd_);
    SetMenu(hwnd_, bar);
    if (old) DestroyMenu(old);
}

void SessionWindow::pushToolbar() {
    if (!toolbar_) return;
    std::string j = "{\"type\":\"state\",\"title\":\"" + web::esc(device_.name) + "\",\"focused\":" +
                    std::to_string(focused_) + ",\"fullscreen\":" + (fullscreen_.count(hwnd_) ? "true" : "false") +
                    ",\"stats\":\"" + web::esc(toUtf8(stats_)) + "\",\"screens\":[";
    for (size_t i = 0; i < screens_.size(); i++) {
        j += (i ? "," : "") + std::string("{\"name\":\"") + web::esc(screens_[i].info.name) + "\",\"hidden\":" +
             (hidden_.count(screens_[i].info.id) ? "true" : "false") + ",\"popped\":" +
             (screens_[i].popout ? "true" : "false") + "}";
    }
    toolbar_->post(j + "]}");
}

void SessionWindow::showToolbar(bool show) {
    if (!toolbar_ || useMenu_) return;
    if (show == toolbarShown_ && !show) return;
    toolbarShown_ = show;
    RECT rc;
    GetClientRect(hwnd_, &rc);
    int h = ui::scale(hwnd_, 44) + (toolbarMenuOpen_ ? toolbarExtra_ : 0);
    rc.bottom = std::min<LONG>(rc.bottom, h);
    toolbar_->resize(rc);
    toolbar_->show(show);
    if (show) {
        // Keep the video windows underneath the toolbar.
        for (auto& s : screens_)
            if (!s.popout) SetWindowPos(s.view->hwnd(), HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        pushToolbar();
    }
}

void SessionWindow::onToolbarMessage(const std::string& m) {
    hl::json::Value v;
    if (!hl::json::parse(m, v)) return;
    std::string cmd = v["cmd"].type == hl::json::Value::String ? v["cmd"].str : "";
    if (cmd == "ready") {
        pushToolbar();
    } else if (cmd == "menuOpen") {
        toolbarMenuOpen_ = true;
        toolbarExtra_ = ui::scale(hwnd_, (int)v["height"].asNumber(200));
        showToolbar(true);
    } else if (cmd == "menuClosed") {
        toolbarMenuOpen_ = false;
        showToolbar(toolbarShown_);
    } else if (cmd == "leave") {
        if (!toolbarMenuOpen_) showToolbar(false);
    } else if (v["cmd"].type == hl::json::Value::Number) {
        PostMessageW(hwnd_, WM_COMMAND, (WPARAM)(int)v["cmd"].num, 0);
    }
}

void SessionWindow::popOut(uint32_t id) {
    Screen* s = screenFor(id);
    if (!s || s->popout) return;
    std::wstring title = fromUtf8(device_.name) + L" - " + fromUtf8(s->info.name);
    s->popout = CreateWindowExW(0, kPopout, title.c_str(), WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT,
                                CW_USEDEFAULT, 1280, 760, nullptr, nullptr, GetModuleHandleW(nullptr), this);
    SetParent(s->view->hwnd(), s->popout);
    gWindows.insert(s->popout);
    ShowWindow(s->view->hwnd(), SW_SHOW);
    ShowWindow(s->popout, SW_SHOW);
    RECT rc;
    GetClientRect(s->popout, &rc);
    MoveWindow(s->view->hwnd(), 0, 0, rc.right, rc.bottom, TRUE);
    if (focused_ >= 0 && &screens_[focused_] == s) focused_ = -1;
    buildMenu();
    layout();
    syncStreams();
}

void SessionWindow::popIn(HWND popout) {
    for (auto& s : screens_)
        if (s.popout == popout) {
            SetParent(s.view->hwnd(), hwnd_);
            s.popout = nullptr;
        }
    gWindows.erase(popout);
    buildMenu();
    layout();
    syncStreams();
}

void SessionWindow::toggleFullscreen(HWND w) {
    auto it = fullscreen_.find(w);
    if (it == fullscreen_.end()) {
        WINDOWPLACEMENT wp{sizeof wp};
        GetWindowPlacement(w, &wp);
        fullscreen_[w] = wp;
        MONITORINFO mi{sizeof mi};
        GetMonitorInfoW(MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongW(w, GWL_STYLE, GetWindowLongW(w, GWL_STYLE) & ~WS_OVERLAPPEDWINDOW);
        if (w == hwnd_ && useMenu_) SetMenu(hwnd_, nullptr);
        SetWindowPos(w, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    } else {
        SetWindowLongW(w, GWL_STYLE, GetWindowLongW(w, GWL_STYLE) | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(w, &it->second);
        fullscreen_.erase(it);
        if (w == hwnd_) buildMenu();
        showToolbar(false);
        SetWindowPos(w, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
}

// Keyboard from a session or pop-out window. Returns true if handled.
bool SessionWindow::keyEvent(UINT msg, WPARAM wp, LPARAM lp) {
    if (!connected_) return false;
    bool down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
    UINT vk = (UINT)wp;
    UINT scan = (lp >> 16) & 0xFF;
    bool ext = (lp >> 24) & 1;
    if (vk == VK_SHIFT) vk = MapVirtualKeyW(scan, MAPVK_VSC_TO_VK_EX);
    else if (vk == VK_CONTROL) vk = ext ? VK_RCONTROL : VK_LCONTROL;
    else if (vk == VK_MENU) vk = ext ? VK_RMENU : VK_LMENU;
    // Ctrl+Alt+Enter stays here: full screen on/off.
    if (vk == VK_RETURN && down && (GetKeyState(VK_CONTROL) & 0x8000) && (GetKeyState(VK_MENU) & 0x8000)) {
        toggleFullscreen(GetAncestor(GetFocus() ? GetFocus() : hwnd_, GA_ROOT));
        return true;
    }
    client_.key((uint16_t)vk, down);
    return true;
}

void SessionWindow::updateTitle() {
    std::wstring t = fromUtf8(hostName_.empty() ? device_.name : device_.name + " (" + hostName_ + ")");
    if (connected_ && showStats_) {
        double fps = 0, mbps = 0, host = 0, decode = 0;
        int n = 0;
        for (auto& s : screens_) {
            if (!s.running) continue;
            auto st = client_.takeStats(s.streamId);
            auto vs = s.view->takeStats();
            fps = std::max(fps, (double)vs.frames);
            mbps += st.mbps;
            host = std::max(host, (double)st.hostMs);
            if (vs.frames) decode = std::max(decode, vs.decodeMsSum / vs.frames);
            n++;
        }
        wchar_t b[200];
        swprintf(b, 200, L"%.0f fps  %.0f Mb/s  PC %.1f ms  net %.1f ms  decode %.1f ms", fps, mbps, host,
                 client_.rttMs() / 2, decode);
        stats_ = b;
        t += L"  -  " + stats_;
        if (toolbarShown_) pushToolbar();
    }
    SetWindowTextW(hwnd_, (L"Hyperlink - " + t).c_str());
}

// ---- window procedures

LRESULT CALLBACK SessionWindow::proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* self = reinterpret_cast<SessionWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)self);
        self->hwnd_ = h;
    }
    auto* self = ownerOf(h);
    if (!self) return DefWindowProcW(h, msg, wp, lp);
    if (msg == WM_NCDESTROY) {
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        std::unique_lock<std::mutex> lock(self->life_);
        self->closed_ = true;
        self->hwnd_ = nullptr;
        if (!self->connecting_) {
            lock.unlock();
            delete self;
        }
        return 0;
    }
    return self->handle(msg, wp, lp);
}

LRESULT CALLBACK SessionWindow::popoutProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE)
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR) reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
    auto* self = ownerOf(h);
    if (!self) return DefWindowProcW(h, msg, wp, lp);
    switch (msg) {
        case WM_SIZE: {
            HWND child = GetWindow(h, GW_CHILD);
            if (child) MoveWindow(child, 0, 0, LOWORD(lp), HIWORD(lp), TRUE);
            self->syncStreams();
            return 0;
        }
        case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP:
            if (self->keyEvent(msg, wp, lp)) return 0;
            break;
        case WM_SYSCHAR:
            return 0;
        case WM_APP + 30:
            if (self->connected_) self->client_.key((uint16_t)wp, lp != 0);
            return 0;
        case WM_CLOSE:
            self->popIn(h);
            DestroyWindow(h);
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

LRESULT SessionWindow::handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_SIZE:
            layout();
            syncStreams();
            if (toolbarShown_) showToolbar(true);
            return 0;
        case WM_TIMER:
            updateTitle();
            if (toolbarShown_ && !toolbarMenuOpen_) {
                // Hide the toolbar once the pointer has left it.
                POINT p;
                GetCursorPos(&p);
                ScreenToClient(hwnd_, &p);
                RECT rc;
                GetClientRect(hwnd_, &rc);
                if (p.y > ui::scale(hwnd_, 60) || p.x < 0 || p.x > rc.right || GetForegroundWindow() != hwnd_) showToolbar(false);
            }
            return 0;
        case WM_MOUSEMOVE:
            if (GET_Y_LPARAM(lp) <= ui::scale(hwnd_, 6)) showToolbar(true);
            break;
        case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP:
            if (keyEvent(msg, wp, lp)) return 0;
            break;
        case WM_SYSCHAR:
            if (connected_) return 0;  // don't let Alt+letter open our menus while controlling
            break;
        case WM_APP + 30:
            if (connected_) client_.key((uint16_t)wp, lp != 0);
            return 0;
        case WM_CTLCOLORSTATIC:
            SetTextColor((HDC)wp, RGB(230, 240, 255));
            SetBkColor((HDC)wp, RGB(5, 6, 15));
            return (LRESULT)GetClassLongPtrW(hwnd_, GCLP_HBRBACKGROUND);
        case WM_S_CONNECTED: {
            std::unique_ptr<std::string> err((std::string*)lp);
            if (err->empty()) {
                connected_ = true;
                setStatus(L"");
                syncStreams();
                updateTitle();
            } else if (err->find("PIN") != std::string::npos) {
                setStatus(fromUtf8(*err));
                if (ui::editDevice(hwnd_, device_, L"PIN for " + fromUtf8(device_.name), true)) {
                    if (!device_.id.empty()) devices::save(device_);
                    connect();
                }
            } else {
                setStatus(L"Couldn't connect to " + fromUtf8(device_.name) + L"\n\n" + fromUtf8(*err) +
                          L"\n\nClose this window and try again from the device list.");
            }
            return 0;
        }
        case WM_S_MONITORS: {
            std::unique_ptr<std::vector<hl::MonitorInfo>> m((std::vector<hl::MonitorInfo>*)lp);
            setMonitors(*m);
            return 0;
        }
        case WM_S_STREAM_ERROR: {
            std::unique_ptr<std::string> e((std::string*)lp);
            MessageBoxW(hwnd_, fromUtf8(*e).c_str(), L"Hyperlink", MB_ICONWARNING);
            return 0;
        }
        case WM_S_CURSOR_SHAPE: {
            std::unique_ptr<hl::CursorShape> c((hl::CursorShape*)lp);
            // Build a real Windows cursor so the local pointer looks like the remote one.
            BITMAPV5HEADER bh{};
            bh.bV5Size = sizeof bh;
            bh.bV5Width = c->width;
            bh.bV5Height = -(LONG)c->height;
            bh.bV5Planes = 1;
            bh.bV5BitCount = 32;
            bh.bV5Compression = BI_BITFIELDS;
            bh.bV5RedMask = 0x00FF0000;
            bh.bV5GreenMask = 0x0000FF00;
            bh.bV5BlueMask = 0x000000FF;
            bh.bV5AlphaMask = 0xFF000000;
            void* bits = nullptr;
            HDC dc = GetDC(nullptr);
            HBITMAP color = CreateDIBSection(dc, (BITMAPINFO*)&bh, DIB_RGB_COLORS, &bits, nullptr, 0);
            ReleaseDC(nullptr, dc);
            if (color && bits) {
                auto* px = (uint8_t*)bits;
                for (size_t i = 0; i < (size_t)c->width * c->height; i++) {
                    px[i * 4 + 0] = c->rgba[i * 4 + 2];
                    px[i * 4 + 1] = c->rgba[i * 4 + 1];
                    px[i * 4 + 2] = c->rgba[i * 4 + 0];
                    px[i * 4 + 3] = c->rgba[i * 4 + 3];
                }
                HBITMAP mask = CreateBitmap(c->width, c->height, 1, 1, nullptr);
                ICONINFO ii{FALSE, c->hotX, c->hotY, mask, color};
                HCURSOR cur = CreateIconIndirect(&ii);
                DeleteObject(mask);
                if (cur) {
                    if (cursor_) DestroyCursor(cursor_);
                    cursor_ = cur;
                    for (auto& s : screens_) s.view->setCursor(cursor_, true);
                }
            }
            if (color) DeleteObject(color);
            return 0;
        }
        case WM_S_CURSOR_POS:
            // A game or app hid the pointer on that screen: hide ours there too.
            if (auto* s = screenFor((uint32_t)wp)) s->view->setCursor(cursor_, lp != 0);
            return 0;
        case WM_S_DISCONNECTED: {
            std::unique_ptr<std::string> r((std::string*)lp);
            connected_ = false;
            for (auto& s : screens_) s.running = false;
            setStatus(L"Disconnected from " + fromUtf8(device_.name) + L"\n\n" + fromUtf8(*r));
            return 0;
        }
        case WM_COMMAND: {
            UINT id = LOWORD(wp);
            if (id == IDM_ALL) {
                focused_ = -1;
            } else if (id >= IDM_SCREEN && id < IDM_SCREEN + 99) {
                int i = id - IDM_SCREEN;
                if (i < (int)screens_.size() && !screens_[i].popout) focused_ = i;
            } else if (id >= IDM_HIDE && id < IDM_HIDE + 99) {
                int i = id - IDM_HIDE;
                if (i < (int)screens_.size()) {
                    uint32_t mid = screens_[i].info.id;
                    if (hidden_.count(mid)) hidden_.erase(mid);
                    else if (hidden_.size() + 1 < screens_.size()) hidden_.insert(mid);
                    devices::setHiddenScreens(device_.id, std::vector<uint32_t>(hidden_.begin(), hidden_.end()));
                }
            } else if (id >= IDM_POPOUT && id < IDM_POPOUT + 99) {
                int i = id - IDM_POPOUT;
                if (i < (int)screens_.size()) popOut(screens_[i].info.id);
                return 0;
            } else if (id == IDM_FULLSCREEN) {
                toggleFullscreen(hwnd_);
                return 0;
            } else if (id == IDM_STATS) {
                showStats_ = !showStats_;
            } else if (id == IDM_TASKMGR) {
                for (uint16_t k : {VK_LCONTROL, VK_LSHIFT, VK_ESCAPE}) client_.key(k, true);
                for (uint16_t k : {VK_ESCAPE, VK_LSHIFT, VK_LCONTROL}) client_.key(k, false);
                return 0;
            } else if (id == IDM_WINKEY) {
                client_.key(VK_LWIN, true);
                client_.key(VK_LWIN, false);
                return 0;
            } else if (id == IDM_ALTTAB) {
                client_.key(VK_LMENU, true);
                client_.key(VK_TAB, true);
                client_.key(VK_TAB, false);
                client_.key(VK_LMENU, false);
                return 0;
            } else if (id == IDM_PIN) {
                if (ui::editDevice(hwnd_, device_, L"PIN for " + fromUtf8(device_.name), true) && !device_.id.empty())
                    devices::save(device_);
                return 0;
            } else if (id == IDM_DISCONNECT) {
                DestroyWindow(hwnd_);
                return 0;
            }
            buildMenu();
            layout();
            syncStreams();
            updateTitle();
            return 0;
        }
        case WM_DESTROY: {
            KillTimer(hwnd_, 1);
            for (auto it = gByDevice.begin(); it != gByDevice.end();)
                it = it->second == hwnd_ ? gByDevice.erase(it) : std::next(it);
            toolbar_.reset();
            gWindows.erase(hwnd_);
            stopAll();
            client_.disconnect();
            for (auto& s : screens_)
                if (s.popout) {
                    gWindows.erase(s.popout);
                    SetWindowLongPtrW(s.popout, GWLP_USERDATA, 0);
                    DestroyWindow(s.popout);
                }
            screens_.clear();
            if (cursor_) DestroyCursor(cursor_);
            if (gWindows.empty() && gHook) {
                UnhookWindowsHookEx(gHook);
                gHook = nullptr;
            }
            return 0;
        }
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}
