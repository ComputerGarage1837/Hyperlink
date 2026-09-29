// Hyperlink Host: sits in the tray, streams this PC's monitors to Hyperlink clients.
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <timeapi.h>

#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "hyperlink/common.h"
#include "encoder.h"
#include "hyperlink/net.h"
#include "monitors.h"
#include "resource.h"
#include "server.h"
#include "settings.h"
#include "updater.h"
#include "client/roster.h"
#include "client/videoview.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace {

enum : UINT {
    WM_APP_TRAY = WM_APP + 1,
    WM_APP_STATUS,
    WM_APP_UPDATE_FOUND,    // lParam: ReleaseInfo*, wParam: 1 if the user asked
    WM_APP_UPDATE_NONE,     // wParam: 1 if the user asked; lParam: std::string* error or null
    WM_APP_UPDATE_PROGRESS, // wParam: percent
    WM_APP_UPDATE_READY,    // lParam: std::wstring* installer path
    WM_APP_UPDATE_FAILED,   // lParam: std::string* error
    WM_APP_OPEN,            // another launch asked us to show the device list
};

enum : UINT {
    ID_TRAY = 1,
    IDM_OPEN = 99,
    IDM_SETTINGS = 100,
    IDM_UPDATES,
    IDM_LOG,
    IDM_EXIT,
    IDF_NAME = 200,
    IDF_PIN,
    IDF_AUTOUPDATE,
    IDF_SAVE,
    IDF_CHECK,
    IDF_STATUS,
    IDF_INFO,
};

HINSTANCE gInst;
HWND gMsgWnd, gSettingsWnd;
std::unique_ptr<Server> gServer;
NOTIFYICONDATAW gNid{};
FILE* gLog = nullptr;
std::mutex gLogMutex;
std::atomic<bool> gUpdateBusy{false};
HFONT gFont;

std::wstring W(const std::string& s) { return fromUtf8(s); }

void openLog() {
    std::wstring dir = dataDir();
    std::wstring path = dir + L"\\host.log";
    MoveFileExW(path.c_str(), (dir + L"\\host.previous.log").c_str(), MOVEFILE_REPLACE_EXISTING);
    gLog = _wfopen(path.c_str(), L"w");
    hl::setLogSink([](const std::string& line) {
        SYSTEMTIME t;
        GetLocalTime(&t);
        std::lock_guard<std::mutex> lock(gLogMutex);
        if (gLog) {
            fprintf(gLog, "%02d:%02d:%02d.%03d %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
                    line.c_str());
            fflush(gLog);
        }
    });
}

std::string addressesText() {
    std::string s;
    for (auto& a : hl::net::localAddresses()) s += (s.empty() ? "" : ", ") + a;
    return s.empty() ? "no network" : s;
}

void setTip(const std::wstring& tip) {
    wcsncpy(gNid.szTip, tip.c_str(), ARRAYSIZE(gNid.szTip) - 1);
    gNid.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &gNid);
}

void balloon(const std::wstring& title, const std::wstring& text) {
    gNid.uFlags = NIF_INFO;
    wcsncpy(gNid.szInfoTitle, title.c_str(), ARRAYSIZE(gNid.szInfoTitle) - 1);
    wcsncpy(gNid.szInfo, text.c_str(), ARRAYSIZE(gNid.szInfo) - 1);
    gNid.dwInfoFlags = NIIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY, &gNid);
}

void refreshStatus() {
    auto st = gServer->status();
    std::wstring tip = L"Hyperlink Host";
    if (st.clients.empty()) tip += L" - waiting for connections";
    else tip += L" - " + std::to_wstring(st.clients.size()) + L" connected";
    setTip(tip);
    // Keep the PC and its screens awake while someone is watching.
    SetThreadExecutionState(st.streams ? (ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED)
                                       : ES_CONTINUOUS);
    if (gSettingsWnd) {
        auto s = gServer->settings();
        std::string info = "Addresses: " + addressesText() + "\r\nConnected: ";
        if (st.clients.empty()) info += "nobody";
        for (size_t i = 0; i < st.clients.size(); i++) info += (i ? ", " : "") + st.clients[i];
        SetDlgItemTextW(gSettingsWnd, IDF_INFO, W(info).c_str());
    }
}

void setUpdateStatus(const std::wstring& s) {
    if (gSettingsWnd) SetDlgItemTextW(gSettingsWnd, IDF_STATUS, s.c_str());
}

void checkForUpdates(bool userAsked) {
    if (gUpdateBusy.exchange(true)) return;
    setUpdateStatus(L"Checking for updates...");
    std::thread([userAsked] {
        auto r = std::make_unique<ReleaseInfo>();
        std::string err;
        if (updater::latest(*r, err) && updater::isNewer(r->version, hostVersion())) {
            PostMessageW(gMsgWnd, WM_APP_UPDATE_FOUND, userAsked, (LPARAM)r.release());
        } else {
            PostMessageW(gMsgWnd, WM_APP_UPDATE_NONE, userAsked,
                         err.empty() ? 0 : (LPARAM) new std::string(err));
        }
    }).detach();
}

void startDownload(const ReleaseInfo& r) {
    std::thread([r] {
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        auto path = std::make_unique<std::wstring>(std::wstring(tmp) + W(r.assetName));
        std::string err;
        int lastPct = -1;
        bool ok = updater::download(r, *path, [&](uint64_t done, uint64_t total) {
            int pct = total ? (int)(done * 100 / total) : 0;
            if (pct != lastPct) {
                lastPct = pct;
                PostMessageW(gMsgWnd, WM_APP_UPDATE_PROGRESS, pct, 0);
            }
        }, err);
        if (ok) PostMessageW(gMsgWnd, WM_APP_UPDATE_READY, 0, (LPARAM)path.release());
        else PostMessageW(gMsgWnd, WM_APP_UPDATE_FAILED, 0, (LPARAM) new std::string(err));
    }).detach();
}

void onUpdateFound(ReleaseInfo* r, bool userAsked) {
    std::unique_ptr<ReleaseInfo> rel(r);
    auto s = gServer->settings();
    if (!userAsked && s.skippedVersion == r->version) {
        gUpdateBusy = false;
        return;
    }
    std::wstring notes = W(r->notes);
    if (notes.size() > 1500) notes = notes.substr(0, 1500) + L"...";
    std::wstring main = L"Hyperlink " + W(r->version) + L" is available";
    std::wstring content = L"You have " + W(hostVersion()) + L".\n\n" + notes;

    TASKDIALOG_BUTTON buttons[] = {{100, L"Install now"}, {101, L"Skip this version"}, {IDCANCEL, L"Later"}};
    TASKDIALOGCONFIG tc{};
    tc.cbSize = sizeof tc;
    tc.hwndParent = gSettingsWnd;
    tc.hInstance = gInst;
    tc.pszWindowTitle = L"Hyperlink update";
    tc.pszMainInstruction = main.c_str();
    tc.pszContent = content.c_str();
    tc.pButtons = buttons;
    tc.cButtons = 3;
    tc.nDefaultButton = 100;
    tc.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT;
    tc.pszMainIcon = MAKEINTRESOURCEW(IDI_APP);
    tc.dwFlags |= TDF_USE_HICON_MAIN;
    tc.hMainIcon = LoadIconW(gInst, MAKEINTRESOURCEW(IDI_APP));
    int pressed = IDCANCEL;
    TaskDialogIndirect(&tc, &pressed, nullptr, nullptr);
    if (pressed == 100) {
        setUpdateStatus(L"Downloading update...");
        startDownload(*r);
        return;  // still busy
    }
    if (pressed == 101) {
        s.skippedVersion = r->version;
        gServer->updateSettings(s);
    }
    setUpdateStatus(L"Update " + W(r->version) + L" available");
    gUpdateBusy = false;
}

// ---------------------------------------------------------------- settings window

HWND addCtl(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h,
            int id, float scale) {
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, (int)(x * scale), (int)(y * scale),
                             (int)(w * scale), (int)(h * scale), parent, (HMENU)(INT_PTR)id, gInst, nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)gFont, TRUE);
    return c;
}

LRESULT CALLBACK settingsProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            float sc = GetDpiForWindow(h) / 96.0f;
            NONCLIENTMETRICSW ncm{sizeof ncm};
            SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, GetDpiForWindow(h));
            if (gFont) DeleteObject(gFont);
            gFont = CreateFontIndirectW(&ncm.lfMessageFont);
            auto s = gServer->settings();
            std::wstring title = L"Hyperlink Host " + W(hostVersion());
            addCtl(h, L"STATIC", title.c_str(), 0, 16, 14, 400, 20, 0, sc);
            addCtl(h, L"STATIC", L"This PC's name", 0, 16, 48, 130, 20, 0, sc);
            addCtl(h, L"EDIT", W(s.name).c_str(), WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 150, 45, 250, 24,
                   IDF_NAME, sc);
            addCtl(h, L"STATIC", L"PIN", 0, 16, 84, 130, 20, 0, sc);
            addCtl(h, L"EDIT", W(s.pin).c_str(), WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 150, 81, 120, 24,
                   IDF_PIN, sc);
            addCtl(h, L"STATIC", L"Devices must enter this to connect.\r\nLeave empty to allow anyone.", 0,
                   16, 112, 400, 36, 0, sc);
            addCtl(h, L"STATIC", L"", 0, 16, 154, 400, 40, IDF_INFO, sc);
            HWND chk = addCtl(h, L"BUTTON", L"Check for updates when Hyperlink starts",
                              BS_AUTOCHECKBOX | WS_TABSTOP, 16, 200, 380, 22, IDF_AUTOUPDATE, sc);
            SendMessageW(chk, BM_SETCHECK, s.checkUpdatesOnStart ? BST_CHECKED : BST_UNCHECKED, 0);
            addCtl(h, L"BUTTON", L"Save", BS_DEFPUSHBUTTON | WS_TABSTOP, 16, 236, 90, 30, IDF_SAVE, sc);
            addCtl(h, L"BUTTON", L"Check for updates", WS_TABSTOP, 116, 236, 150, 30, IDF_CHECK, sc);
            addCtl(h, L"STATIC", L"", 0, 16, 276, 400, 20, IDF_STATUS, sc);
            gSettingsWnd = h;
            refreshStatus();
            return 0;
        }
        case WM_COMMAND:
            if (LOWORD(wp) == IDF_SAVE) {
                auto s = gServer->settings();
                wchar_t buf[256];
                GetDlgItemTextW(h, IDF_NAME, buf, 256);
                s.name = toUtf8(buf);
                if (s.name.empty()) s.name = hl::net::hostName();
                GetDlgItemTextW(h, IDF_PIN, buf, 256);
                s.pin = toUtf8(buf);
                s.checkUpdatesOnStart = IsDlgButtonChecked(h, IDF_AUTOUPDATE) == BST_CHECKED;
                gServer->updateSettings(s);
                setUpdateStatus(L"Saved.");
            } else if (LOWORD(wp) == IDF_CHECK) {
                checkForUpdates(true);
            }
            return 0;
        case WM_CTLCOLORSTATIC:
            SetBkMode((HDC)wp, TRANSPARENT);
            return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
        case WM_CLOSE:
            DestroyWindow(h);
            return 0;
        case WM_DESTROY:
            gSettingsWnd = nullptr;
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void showSettings() {
    if (gSettingsWnd) {
        ShowWindow(gSettingsWnd, SW_RESTORE);
        SetForegroundWindow(gSettingsWnd);
        return;
    }
    UINT dpi = GetDpiForSystem();
    RECT rc{0, 0, MulDiv(430, dpi, 96), MulDiv(310, dpi, 96)};
    AdjustWindowRectExForDpi(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE, 0, dpi);
    HWND h = CreateWindowExW(0, L"HyperlinkSettings", L"Hyperlink Host",
                             WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, CW_USEDEFAULT,
                             CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, gInst,
                             nullptr);
    ShowWindow(h, SW_SHOW);
    SetForegroundWindow(h);
}

void showMenu() {
    auto s = gServer->settings();
    auto st = gServer->status();
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | MF_DEFAULT, IDM_OPEN, L"Open Hyperlink (devices)");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, (L"Hyperlink " + W(hostVersion())).c_str());
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, (L"Name: " + W(s.name)).c_str());
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, (L"PIN: " + (s.pin.empty() ? L"none" : W(s.pin))).c_str());
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, (L"Address: " + W(addressesText())).c_str());
    for (auto& c : st.clients) AppendMenuW(m, MF_STRING | MF_GRAYED, 0, (L"Connected: " + W(c)).c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_SETTINGS, L"This PC's name and PIN...");
    AppendMenuW(m, MF_STRING, IDM_UPDATES, L"Check for updates");
    AppendMenuW(m, MF_STRING, IDM_LOG, L"Open log");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_EXIT, L"Exit");
    POINT p;
    GetCursorPos(&p);
    SetForegroundWindow(gMsgWnd);
    TrackPopupMenu(m, TPM_RIGHTBUTTON, p.x, p.y, 0, gMsgWnd, nullptr);
    DestroyMenu(m);
}

LRESULT CALLBACK msgProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    static UINT taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    if (msg == taskbarCreated) {
        Shell_NotifyIconW(NIM_ADD, &gNid);
        return 0;
    }
    switch (msg) {
        case WM_APP_TRAY:
            switch (LOWORD(lp)) {
                case WM_LBUTTONDBLCLK: roster::show(); break;
                case WM_RBUTTONUP:
                case WM_CONTEXTMENU: showMenu(); break;
                case NIN_BALLOONUSERCLICK: checkForUpdates(true); break;
            }
            return 0;
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case IDM_OPEN: roster::show(); break;
                case IDM_SETTINGS: showSettings(); break;
                case IDM_UPDATES: showSettings(); checkForUpdates(true); break;
                case IDM_LOG: ShellExecuteW(nullptr, L"open", (dataDir() + L"\\host.log").c_str(), nullptr,
                                            nullptr, SW_SHOWNORMAL); break;
                case IDM_EXIT: DestroyWindow(h); break;
            }
            return 0;
        case WM_APP_STATUS:
            refreshStatus();
            return 0;
        case WM_APP_OPEN:
            roster::show();
            return 0;
        case WM_APP_UPDATE_FOUND:
            if (!wp) {  // found by the startup check: don't pop a dialog, just tell
                std::unique_ptr<ReleaseInfo> r((ReleaseInfo*)lp);
                auto s = gServer->settings();
                if (s.skippedVersion != r->version)
                    balloon(L"Hyperlink update available",
                            L"Version " + W(r->version) + L" is ready. Click here to install.");
                gUpdateBusy = false;
                return 0;
            }
            onUpdateFound((ReleaseInfo*)lp, true);
            return 0;
        case WM_APP_UPDATE_NONE: {
            std::unique_ptr<std::string> err((std::string*)lp);
            gUpdateBusy = false;
            if (err) setUpdateStatus(L"Update check failed: " + W(*err));
            else setUpdateStatus(L"You have the latest version.");
            if (wp && err) MessageBoxW(gSettingsWnd, W(*err).c_str(), L"Hyperlink update", MB_ICONWARNING);
            return 0;
        }
        case WM_APP_UPDATE_PROGRESS:
            setUpdateStatus(L"Downloading update... " + std::to_wstring(wp) + L"%");
            setTip(L"Hyperlink Host - downloading update " + std::to_wstring(wp) + L"%");
            return 0;
        case WM_APP_UPDATE_READY: {
            std::unique_ptr<std::wstring> path((std::wstring*)lp);
            setUpdateStatus(L"Installing...");
            if (updater::runInstaller(*path)) {
                DestroyWindow(h);  // the installer restarts us
            } else {
                gUpdateBusy = false;
                setUpdateStatus(L"The installer did not start.");
            }
            return 0;
        }
        case WM_APP_UPDATE_FAILED: {
            std::unique_ptr<std::string> err((std::string*)lp);
            gUpdateBusy = false;
            setUpdateStatus(L"Update failed: " + W(*err));
            MessageBoxW(gSettingsWnd, W(*err).c_str(), L"Hyperlink update", MB_ICONWARNING);
            refreshStatus();
            return 0;
        }
        case WM_DESTROY:
            Shell_NotifyIconW(NIM_DELETE, &gNid);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// Ask the OS to schedule our GPU work ahead of games (no-op without the privilege).
void raiseGpuPriority() {
    using Fn = LONG(WINAPI*)(HANDLE, int);
    if (HMODULE gdi = GetModuleHandleW(L"gdi32.dll"))
        if (auto fn = (Fn)(void*)GetProcAddress(gdi, "D3DKMTSetProcessSchedulingPriorityClass"))
            fn(GetCurrentProcess(), 4 /* HIGH */);
}

}  // namespace

// "HyperlinkHost.exe --selftest <file>": proves the exe and its DLLs load, lists the encoders
// FFmpeg offers, then exits. Used by CI.
static int selfTest(const wchar_t* path) {
    FILE* f = _wfopen(path, L"w");
    if (!f) return 2;
    fprintf(f, "Hyperlink Host %s\n", hostVersion().c_str());
    for (const char* n : {"hevc_nvenc", "hevc_amf", "hevc_qsv", "h264_nvenc", "h264_amf", "h264_qsv", "libx264",
                          "av1_nvenc", "av1_amf", "av1_qsv"})
        fprintf(f, "%s %s\n", n, Encoder::available(n) ? "yes" : "no");
    fprintf(f, "monitors %d\n", (int)enumerateMonitors().size());
    for (const char* n : {"h264", "hevc", "av1"})
        fprintf(f, "decoder %s %s\n", n, avcodec_find_decoder_by_name(n) ? "yes" : "no");
    fprintf(f, "ok\n");
    fclose(f);
    return 0;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmd, int) {
    gInst = inst;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc >= 3 && wcscmp(argv[1], L"--selftest") == 0) return selfTest(argv[2]);
    (void)cmd;
    bool background = false;
    for (int i = 1; i < argc; i++)
        if (wcscmp(argv[i], L"--background") == 0) background = true;
    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\HyperlinkHost");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        // Already running (probably in the tray): just bring up its device list.
        if (HWND other = FindWindowExW(HWND_MESSAGE, nullptr, L"HyperlinkHostMsg", nullptr)) {
            AllowSetForegroundWindow(ASFW_ANY);
            PostMessageW(other, WM_APP_OPEN, 0, 0);
        }
        return 0;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES};
    InitCommonControlsEx(&icc);
    timeBeginPeriod(1);
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    raiseGpuPriority();
    openLog();
    hl::log("Hyperlink Host %s starting", hostVersion().c_str());

    gServer = std::make_unique<Server>(HostSettings::load());
    if (!gServer->start()) {
        MessageBoxW(nullptr,
                    L"Hyperlink Host could not open its network ports (47800-47802).\n"
                    L"Another copy may be running, or another program is using them.",
                    L"Hyperlink Host", MB_ICONERROR);
        return 1;
    }

    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = msgProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"HyperlinkHostMsg";
    RegisterClassExW(&wc);
    WNDCLASSEXW sc{sizeof sc};
    sc.lpfnWndProc = settingsProc;
    sc.hInstance = inst;
    sc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    sc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    sc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
    sc.lpszClassName = L"HyperlinkSettings";
    RegisterClassExW(&sc);
    roster::Hooks hooks;
    hooks.openHostSettings = [] { showSettings(); };
    hooks.checkForUpdates = [] { checkForUpdates(true); };
    hooks.thisPcSummary = [] {
        auto st = gServer->settings();
        return st.name + "   \u00B7   PIN " + (st.pin.empty() ? std::string("none") : st.pin);
    };
    hooks.thisHostId = [] { return gServer->settings().hostId; };
    roster::registerClasses(inst, hooks);
    gMsgWnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, inst, nullptr);

    gNid.cbSize = sizeof gNid;
    gNid.hWnd = gMsgWnd;
    gNid.uID = ID_TRAY;
    gNid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    gNid.uCallbackMessage = WM_APP_TRAY;
    gNid.hIcon = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                   GetSystemMetrics(SM_CYSMICON), 0);
    wcscpy(gNid.szTip, L"Hyperlink Host");
    Shell_NotifyIconW(NIM_ADD, &gNid);
    gNid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &gNid);

    gServer->onStatusChanged = [] { PostMessageW(gMsgWnd, WM_APP_STATUS, 0, 0); };
    refreshStatus();

    auto settings = gServer->settings();
    bool firstRun = GetFileAttributesW((dataDir() + L"\\shown-welcome").c_str()) == INVALID_FILE_ATTRIBUTES;
    if (!background) roster::show();
    if (firstRun) {
        CloseHandle(CreateFileW((dataDir() + L"\\shown-welcome").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                0, nullptr));
        showSettings();
    } else {
        balloon(L"Hyperlink Host is running", L"PIN " + (settings.pin.empty() ? L"not set" : W(settings.pin)) +
                                                 L" - " + W(addressesText()));
    }
    if (settings.checkUpdatesOnStart) checkForUpdates(false);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0)) {
        if (gSettingsWnd && IsDialogMessageW(gSettingsWnd, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    hl::log("exiting");
    gServer->stop();
    gServer.reset();
    timeEndPeriod(1);
    CloseHandle(single);
    return 0;
}
