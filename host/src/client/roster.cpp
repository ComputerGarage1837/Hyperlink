#include "roster.h"

#include <commctrl.h>

#include <memory>
#include <string>
#include <vector>

#include "../monitors.h"
#include "devices.h"
#include "session.h"
#include "ui.h"

namespace roster {
namespace {

enum : UINT { WM_R_PRESENCE = WM_APP + 50 };
enum : UINT { IDR_LIST = 10, IDR_CONNECT, IDR_ADD, IDR_EDIT, IDR_REMOVE, IDR_THISPC, IDR_UPDATE, IDR_SETTINGS, IDR_HEADER };

Hooks gHooks;
HWND gWnd = nullptr;
HWND gList = nullptr, gHeader = nullptr;
std::vector<HWND> gButtons;
PresenceScanner gScanner;

// One row: a saved device, or a device found on the network that isn't saved yet.
struct Row {
    bool saved = false;
    SavedDevice dev;
    bool online = false;
    Presence p;
};
std::vector<Row> gRows;

std::wstring W(const std::string& s) { return fromUtf8(s); }

void refresh() {
    if (!gList) return;
    auto seen = gScanner.snapshot();
    std::string me = gHooks.thisHostId ? gHooks.thisHostId() : "";
    std::vector<std::string> targets;
    gRows.clear();
    for (auto& d : devices::all()) {
        Row r;
        r.saved = true;
        r.dev = d;
        auto it = d.hostId.empty() ? seen.end() : seen.find(d.hostId);
        if (it != seen.end()) {
            r.online = true;
            r.p = it->second;
            seen.erase(it);
        }
        if (!d.address.empty()) targets.push_back(d.address);
        gRows.push_back(r);
    }
    for (auto& [id, p] : seen) {
        if (id == me) continue;
        Row r;
        r.dev.name = p.info.hostName;
        r.dev.hostId = id;
        r.online = true;
        r.p = p;
        gRows.push_back(r);
    }
    gScanner.setTargets(targets);

    int sel = ListView_GetNextItem(gList, -1, LVNI_SELECTED);
    SendMessageW(gList, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(gList);
    for (size_t i = 0; i < gRows.size(); i++) {
        const Row& r = gRows[i];
        std::wstring name = W(r.dev.name) + (r.saved ? L"" : L"   (new: click Add)");
        std::wstring status, detail;
        if (!r.online) {
            status = L"○ Offline";
            detail = r.dev.address.empty() ? L"Found automatically when it's on your network" : L"Fallback: " + W(r.dev.address);
        } else if (r.p.info.clients > 0) {
            status = L"● In use";
            detail = std::to_wstring(r.p.info.clients) + L" connected, " + std::to_wstring(r.p.info.streams) +
                     L" screen(s) streaming  ·  " + W(r.p.info.hostName) + L"  v" + W(r.p.info.version);
        } else {
            status = L"● Online";
            detail = L"Ready  ·  " + W(r.p.info.hostName) + L"  v" + W(r.p.info.version);
        }
        LVITEMW it{};
        it.mask = LVIF_TEXT;
        it.iItem = (int)i;
        it.pszText = name.data();
        ListView_InsertItem(gList, &it);
        ListView_SetItemText(gList, (int)i, 1, status.data());
        ListView_SetItemText(gList, (int)i, 2, detail.data());
    }
    if (sel >= 0 && sel < (int)gRows.size()) ListView_SetItemState(gList, sel, LVIS_SELECTED, LVIS_SELECTED);
    SendMessageW(gList, WM_SETREDRAW, TRUE, 0);
    if (gHeader && gHooks.thisPcSummary) SetWindowTextW(gHeader, (L"This PC:  " + W(gHooks.thisPcSummary())).c_str());
}

Row* selected() {
    int i = ListView_GetNextItem(gList, -1, LVNI_SELECTED);
    return i >= 0 && i < (int)gRows.size() ? &gRows[i] : nullptr;
}

void connectSelected() {
    Row* r = selected();
    if (!r) return;
    if (!r->saved) {
        SavedDevice d = r->dev;
        if (!ui::editDevice(gWnd, d, L"Add " + W(d.name))) return;
        devices::save(d);
        refresh();
        for (auto& x : devices::all())
            if (x.hostId == d.hostId) d = x;
        SessionWindow::open(d);
        return;
    }
    SessionWindow::open(r->dev);
}

void layout() {
    RECT rc;
    GetClientRect(gWnd, &rc);
    int pad = ui::scale(gWnd, 12), bh = ui::scale(gWnd, 32), hh = ui::scale(gWnd, 28);
    MoveWindow(gHeader, pad, pad, rc.right - pad * 2 - ui::scale(gWnd, 330), hh, TRUE);
    int x = rc.right - pad;
    for (int i = (int)gButtons.size() - 1; i >= 0; i--) {
        int id = GetDlgCtrlID(gButtons[i]);
        if (id == IDR_THISPC || id == IDR_UPDATE || id == IDR_SETTINGS) {
            int w = ui::scale(gWnd, id == IDR_UPDATE ? 140 : 90);
            x -= w;
            MoveWindow(gButtons[i], x, pad - ui::scale(gWnd, 2), w, bh, TRUE);
            x -= ui::scale(gWnd, 8);
        }
    }
    int top = pad + hh + pad;
    int bottomBar = bh + pad * 2;
    MoveWindow(gList, pad, top, rc.right - pad * 2, rc.bottom - top - bottomBar, TRUE);
    int bx = pad;
    for (HWND b : gButtons) {
        int id = GetDlgCtrlID(b);
        if (id == IDR_THISPC || id == IDR_UPDATE || id == IDR_SETTINGS) continue;
        int w = ui::scale(gWnd, id == IDR_CONNECT ? 120 : 90);
        MoveWindow(b, bx, rc.bottom - pad - bh, w, bh, TRUE);
        bx += w + ui::scale(gWnd, 8);
    }
    ListView_SetColumnWidth(gList, 0, ui::scale(gWnd, 240));
    ListView_SetColumnWidth(gList, 1, ui::scale(gWnd, 110));
    ListView_SetColumnWidth(gList, 2, LVSCW_AUTOSIZE_USEHEADER);
}

LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            gWnd = h;
            auto mk = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
                HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, h,
                                         (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
                SendMessageW(c, WM_SETFONT, (WPARAM)ui::font(h), TRUE);
                return c;
            };
            gHeader = mk(L"STATIC", L"", SS_LEFTNOWORDWRAP | SS_CENTERIMAGE, IDR_HEADER);
            gList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                                    0, 0, 10, 10, h, (HMENU)IDR_LIST, GetModuleHandleW(nullptr), nullptr);
            SendMessageW(gList, WM_SETFONT, (WPARAM)ui::font(h), TRUE);
            ListView_SetExtendedListViewStyle(gList, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
            const wchar_t* cols[] = {L"Device", L"Status", L"Details"};
            for (int i = 0; i < 3; i++) {
                LVCOLUMNW c{};
                c.mask = LVCF_TEXT | LVCF_WIDTH;
                c.pszText = const_cast<wchar_t*>(cols[i]);
                c.cx = 150;
                ListView_InsertColumn(gList, i, &c);
            }
            gButtons = {mk(L"BUTTON", L"Connect", BS_DEFPUSHBUTTON | WS_TABSTOP, IDR_CONNECT),
                        mk(L"BUTTON", L"Add...", WS_TABSTOP, IDR_ADD),
                        mk(L"BUTTON", L"Edit...", WS_TABSTOP, IDR_EDIT),
                        mk(L"BUTTON", L"Remove", WS_TABSTOP, IDR_REMOVE),
                        mk(L"BUTTON", L"This PC...", WS_TABSTOP, IDR_THISPC),
                        mk(L"BUTTON", L"Check for updates", WS_TABSTOP, IDR_UPDATE)};
            gScanner.start([] { if (gWnd) PostMessageW(gWnd, WM_R_PRESENCE, 0, 0); });
            refresh();
            layout();
            return 0;
        }
        case WM_SIZE:
            layout();
            return 0;
        case WM_R_PRESENCE:
            refresh();
            return 0;
        case WM_NOTIFY: {
            auto* n = reinterpret_cast<NMHDR*>(lp);
            if (n->idFrom == IDR_LIST && n->code == NM_DBLCLK) connectSelected();
            return 0;
        }
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case IDR_CONNECT: connectSelected(); break;
                case IDR_ADD: {
                    Row* r = selected();
                    SavedDevice d;
                    if (r && !r->saved) d = r->dev;
                    if (ui::editDevice(h, d, L"Add a device")) {
                        if (d.name.empty()) d.name = d.address.empty() ? "My device" : d.address;
                        devices::save(d);
                        refresh();
                    }
                    break;
                }
                case IDR_EDIT:
                    if (Row* r = selected(); r && r->saved) {
                        SavedDevice d = r->dev;
                        if (ui::editDevice(h, d, L"Edit " + W(d.name))) {
                            devices::save(d);
                            refresh();
                        }
                    }
                    break;
                case IDR_REMOVE:
                    if (Row* r = selected(); r && r->saved &&
                        MessageBoxW(h, (L"Remove " + W(r->dev.name) + L"?").c_str(), L"Hyperlink",
                                    MB_YESNO | MB_ICONQUESTION) == IDYES) {
                        devices::remove(r->dev.id);
                        refresh();
                    }
                    break;
                case IDR_THISPC:
                    if (gHooks.openHostSettings) gHooks.openHostSettings();
                    break;
                case IDR_UPDATE:
                    if (gHooks.checkForUpdates) gHooks.checkForUpdates();
                    break;
            }
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(wp) != WA_INACTIVE) refresh();
            return 0;
        case WM_CLOSE:
            DestroyWindow(h);  // closing the list doesn't stop hosting or open sessions
            return 0;
        case WM_DESTROY:
            gScanner.stop();
            gWnd = gList = gHeader = nullptr;
            gButtons.clear();
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

void registerClasses(HINSTANCE inst, Hooks hooks) {
    gHooks = std::move(hooks);
    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.lpszClassName = L"HyperlinkRoster";
    RegisterClassExW(&wc);
    ui::registerClasses(inst);
    SessionWindow::registerClasses(inst);
}

void show() {
    if (gWnd) {
        ShowWindow(gWnd, SW_RESTORE);
        SetForegroundWindow(gWnd);
        return;
    }
    UINT dpi = GetDpiForSystem();
    HWND h = CreateWindowExW(0, L"HyperlinkRoster", L"Hyperlink", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                             MulDiv(860, dpi, 96), MulDiv(520, dpi, 96), nullptr, nullptr, GetModuleHandleW(nullptr),
                             nullptr);
    ShowWindow(h, SW_SHOW);
    SetForegroundWindow(h);
}

}  // namespace roster
