#include "ui.h"

#include "../monitors.h"

namespace ui {

HFONT font(HWND w) {
    static HFONT f = nullptr;
    static UINT fDpi = 0;
    UINT dpi = w ? GetDpiForWindow(w) : GetDpiForSystem();
    if (!f || fDpi != dpi) {
        NONCLIENTMETRICSW ncm{sizeof ncm};
        SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, dpi);
        f = CreateFontIndirectW(&ncm.lfMessageFont);
        fDpi = dpi;
    }
    return f;
}

int scale(HWND w, int px) { return MulDiv(px, w ? GetDpiForWindow(w) : GetDpiForSystem(), 96); }

namespace {

struct EditState {
    SavedDevice* dev;
    bool pinOnly;
    bool ok = false;
    bool done = false;
    HWND name = nullptr, pin = nullptr, addr = nullptr;
};

enum { IDE_OK = 1, IDE_CANCEL = 2 };

HWND ctl(HWND p, const wchar_t* cls, const std::wstring& text, DWORD style, int x, int y, int w, int h, int id) {
    HWND c = CreateWindowExW(0, cls, text.c_str(), WS_CHILD | WS_VISIBLE | style, scale(p, x), scale(p, y),
                             scale(p, w), scale(p, h), p, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)font(p), TRUE);
    return c;
}

LRESULT CALLBACK editProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = reinterpret_cast<EditState*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    switch (msg) {
        case WM_CREATE: {
            st = reinterpret_cast<EditState*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)st);
            int y = 14;
            if (!st->pinOnly) {
                ctl(h, L"STATIC", L"Name", 0, 16, y + 3, 110, 20, 0);
                st->name = ctl(h, L"EDIT", fromUtf8(st->dev->name), WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 130, y, 250, 24, 0);
                y += 36;
            }
            ctl(h, L"STATIC", L"PIN", 0, 16, y + 3, 110, 20, 0);
            st->pin = ctl(h, L"EDIT", fromUtf8(st->dev->pin), WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 130, y, 140, 24, 0);
            y += 36;
            if (!st->pinOnly) {
                ctl(h, L"STATIC", L"Fallback address", 0, 16, y + 3, 110, 20, 0);
                st->addr = ctl(h, L"EDIT", fromUtf8(st->dev->address), WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 130, y, 250, 24, 0);
                y += 30;
                ctl(h, L"STATIC", L"Optional. Devices on your network are found automatically; use this for a "
                                  L"Tailscale or host name to connect from elsewhere.", 0, 16, y, 364, 36, 0);
                y += 44;
            }
            ctl(h, L"BUTTON", L"Save", BS_DEFPUSHBUTTON | WS_TABSTOP, 190, y, 90, 30, IDE_OK);
            ctl(h, L"BUTTON", L"Cancel", WS_TABSTOP, 290, y, 90, 30, IDE_CANCEL);
            SetFocus(st->pinOnly ? st->pin : st->name);
            return 0;
        }
        case WM_COMMAND:
            if (LOWORD(wp) == IDE_OK || LOWORD(wp) == IDE_CANCEL) {
                if (LOWORD(wp) == IDE_OK) {
                    wchar_t b[512];
                    if (st->name) {
                        GetWindowTextW(st->name, b, 512);
                        st->dev->name = toUtf8(b);
                    }
                    GetWindowTextW(st->pin, b, 512);
                    st->dev->pin = toUtf8(b);
                    if (st->addr) {
                        GetWindowTextW(st->addr, b, 512);
                        st->dev->address = toUtf8(b);
                    }
                    st->ok = true;
                }
                DestroyWindow(h);
            }
            return 0;
        case WM_CTLCOLORSTATIC:
            SetBkMode((HDC)wp, TRANSPARENT);
            return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
        case WM_CLOSE:
            DestroyWindow(h);
            return 0;
        case WM_DESTROY:
            if (st) st->done = true;
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

void registerClasses(HINSTANCE inst) {
    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = editProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.lpszClassName = L"HyperlinkEditDevice";
    RegisterClassExW(&wc);
}

bool editDevice(HWND owner, SavedDevice& d, const std::wstring& title, bool askPinOnly) {
    EditState st{&d, askPinOnly};
    int h = askPinOnly ? 110 : 230;
    UINT dpi = owner ? GetDpiForWindow(owner) : GetDpiForSystem();
    RECT rc{0, 0, MulDiv(400, dpi, 96), MulDiv(h, dpi, 96)};
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    AdjustWindowRectExForDpi(&rc, style, FALSE, WS_EX_DLGMODALFRAME, dpi);
    RECT o{};
    if (owner) GetWindowRect(owner, &o);
    int x = owner ? o.left + 60 : CW_USEDEFAULT, y = owner ? o.top + 60 : CW_USEDEFAULT;
    HWND w = CreateWindowExW(WS_EX_DLGMODALFRAME, L"HyperlinkEditDevice", title.c_str(), style, x, y,
                             rc.right - rc.left, rc.bottom - rc.top, owner, nullptr, GetModuleHandleW(nullptr), &st);
    if (!w) return false;
    if (owner) EnableWindow(owner, FALSE);
    ShowWindow(w, SW_SHOW);
    MSG m;
    while (!st.done && GetMessageW(&m, nullptr, 0, 0)) {
        if (IsDialogMessageW(w, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    if (owner) {
        EnableWindow(owner, TRUE);
        SetForegroundWindow(owner);
    }
    return st.ok;
}

}  // namespace ui
