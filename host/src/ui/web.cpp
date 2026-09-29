#include "web.h"

#include <dwmapi.h>
#include <shlobj.h>
#include <wrl.h>

#include <WebView2.h>

#include "../monitors.h"
#include "../settings.h"
#include "hyperlink/common.h"

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace web {

struct Panel::Impl {
    HWND parent;
    std::wstring html;
    std::function<void(const std::string&)> onMessage;
    std::function<void(bool)> onReady;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> view;
    RECT bounds{};
    bool visible = true;
    std::vector<std::string> pending;  // posts made before the page was ready
    Panel* owner;
};

Panel::Panel(HWND parent, std::wstring html, std::function<void(const std::string&)> onMessage,
             std::function<void(bool)> onReady)
    : impl_(new Impl{parent, std::move(html), std::move(onMessage), std::move(onReady)}) {
    impl_->owner = this;
    GetClientRect(parent, &impl_->bounds);
    std::wstring data = dataDir() + L"\\WebView2";
    Impl* im = impl_;
    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, data.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [im](HRESULT r, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(r) || !env) {
                    hl::log("ui: WebView2 environment failed 0x%08lx", r);
                    if (im->onReady) im->onReady(false);
                    return S_OK;
                }
                return env->CreateCoreWebView2Controller(
                    im->parent,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [im](HRESULT r2, ICoreWebView2Controller* ctl) -> HRESULT {
                            if (FAILED(r2) || !ctl) {
                                if (im->onReady) im->onReady(false);
                                return S_OK;
                            }
                            im->controller = ctl;
                            ctl->get_CoreWebView2(&im->view);
                            ComPtr<ICoreWebView2Controller2> c2;
                            if (SUCCEEDED(ctl->QueryInterface(IID_PPV_ARGS(&c2)))) {
                                COREWEBVIEW2_COLOR bg{255, 11, 11, 11};
                                c2->put_DefaultBackgroundColor(bg);
                            }
                            ComPtr<ICoreWebView2Settings> s;
                            im->view->get_Settings(&s);
                            s->put_AreDefaultContextMenusEnabled(FALSE);
                            s->put_IsStatusBarEnabled(FALSE);
                            s->put_IsZoomControlEnabled(FALSE);
#ifdef NDEBUG
                            s->put_AreDevToolsEnabled(FALSE);
#endif
                            EventRegistrationToken tok;
                            im->view->add_WebMessageReceived(
                                Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                    [im](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* a) -> HRESULT {
                                        LPWSTR msg = nullptr;
                                        if (SUCCEEDED(a->TryGetWebMessageAsString(&msg)) && msg) {
                                            std::string m = toUtf8(msg);
                                            CoTaskMemFree(msg);
                                            if (m == "{\"cmd\":\"ready\"}") {
                                                im->owner->ready_ = true;
                                                for (auto& p : im->pending)
                                                    im->view->PostWebMessageAsString(fromUtf8(p).c_str());
                                                im->pending.clear();
                                            }
                                            if (im->onMessage) im->onMessage(m);
                                        }
                                        return S_OK;
                                    })
                                    .Get(),
                                &tok);
                            ctl->put_Bounds(im->bounds);
                            ctl->put_IsVisible(im->visible);
                            im->view->NavigateToString(im->html.c_str());
                            if (im->onReady) im->onReady(true);
                            return S_OK;
                        })
                        .Get());
            })
            .Get());
    if (FAILED(hr)) {
        hl::log("ui: WebView2 not available (0x%08lx)", hr);
        if (impl_->onReady) impl_->onReady(false);
    }
}

Panel::~Panel() {
    if (impl_->controller) impl_->controller->Close();
    delete impl_;
}

void Panel::post(const std::string& json) {
    if (!impl_->view || !ready_) {
        if (impl_->pending.size() > 50) impl_->pending.erase(impl_->pending.begin());
        impl_->pending.push_back(json);
        return;
    }
    impl_->view->PostWebMessageAsString(fromUtf8(json).c_str());
}

void Panel::resize(const RECT& r) {
    impl_->bounds = r;
    if (impl_->controller) impl_->controller->put_Bounds(r);
}

void Panel::show(bool visible) {
    impl_->visible = visible;
    if (impl_->controller) impl_->controller->put_IsVisible(visible);
}

std::wstring loadHtml(int id) {
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!r) return L"<html><body>missing UI</body></html>";
    HGLOBAL g = LoadResource(nullptr, r);
    auto* p = static_cast<const char*>(LockResource(g));
    DWORD n = SizeofResource(nullptr, r);
    return fromUtf8(std::string(p, n));
}

std::string esc(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) {
                    char b[8];
                    snprintf(b, sizeof b, "\\u%04x", c);
                    o += b;
                } else {
                    o += (char)c;
                }
        }
    }
    return o;
}

void styleWindow(HWND h) {
    BOOL dark = TRUE;
    DwmSetWindowAttribute(h, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof dark);
    int backdrop = 2;  // DWMSBT_MAINWINDOW (Mica), Windows 11 only; ignored elsewhere
    DwmSetWindowAttribute(h, 38 /* DWMWA_SYSTEMBACKDROP_TYPE */, &backdrop, sizeof backdrop);
    COLORREF caption = RGB(11, 11, 11);
    DwmSetWindowAttribute(h, 35 /* DWMWA_CAPTION_COLOR */, &caption, sizeof caption);
    COLORREF border = RGB(90, 70, 25);
    DwmSetWindowAttribute(h, 34 /* DWMWA_BORDER_COLOR */, &border, sizeof border);
}

}  // namespace web
