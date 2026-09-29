#include "home.h"

#include <memory>

#include "../client/devices.h"
#include "../client/session.h"
#include "../monitors.h"
#include "../server.h"
#include "hyperlink/json.h"
#include "hyperlink/net.h"
#include "resource.h"
#include "web.h"

namespace home {
namespace {

enum : UINT { WM_H_REFRESH = WM_APP + 70 };

HINSTANCE gInst;
Hooks gHooks;
HWND gWnd = nullptr;
std::unique_ptr<web::Panel> gPanel;
PresenceScanner gScanner;
bool gPendingPcTab = false;

std::string deviceJson(const SavedDevice* d, const Presence* p, bool saved) {
    std::string name = d ? d->name : p->info.hostName;
    std::string detail;
    if (p) {
        detail = p->info.hostName + "  ·  v" + p->info.version;
    } else if (d && !d->remoteAddress.empty()) {
        detail = "Reachable from anywhere via Tailscale when it's on";
    } else if (d && !d->address.empty()) {
        detail = "Fallback address " + d->address;
    } else {
        detail = "Found automatically when it's on your network";
    }
    std::string j = "{";
    j += "\"id\":\"" + web::esc(d ? d->id : "") + "\",";
    j += "\"hostId\":\"" + web::esc(d ? d->hostId : p->info.hostId) + "\",";
    j += "\"name\":\"" + web::esc(name) + "\",";
    j += "\"hostName\":\"" + web::esc(p ? p->info.hostName : "") + "\",";
    j += "\"pin\":\"" + web::esc(d ? d->pin : "") + "\",";
    j += "\"address\":\"" + web::esc(d ? d->address : "") + "\",";
    j += std::string("\"online\":") + (p ? "true" : "false") + ",";
    j += "\"clients\":" + std::to_string(p ? p->info.clients : 0) + ",";
    j += "\"streams\":" + std::to_string(p ? p->info.streams : 0) + ",";
    j += "\"detail\":\"" + web::esc(detail) + "\"";
    (void)saved;
    return j + "}";
}

void pushState() {
    if (!gPanel) return;
    HostSettings me = gHooks.settings();
    auto seen = gScanner.snapshot();
    std::vector<std::string> targets;
    std::string saved, found;
    for (auto& d0 : devices::all()) {
        SavedDevice d = d0;
        const Presence* p = nullptr;
        auto it = d.hostId.empty() ? seen.end() : seen.find(d.hostId);
        if (it != seen.end()) {
            p = &it->second;
            // Pick up a device's Tailscale address as soon as we see it.
            if (!p->info.remoteAddresses.empty() && p->info.remoteAddresses[0] != d.remoteAddress) {
                d.remoteAddress = p->info.remoteAddresses[0];
                devices::save(d);
            }
        }
        if (!d.remoteAddress.empty()) targets.push_back(d.remoteAddress);
        if (!d.address.empty()) targets.push_back(d.address);
        saved += (saved.empty() ? "" : ",") + deviceJson(&d, p, true);
    }
    auto all = devices::all();
    for (auto& [id, p] : seen) {
        if (id == me.hostId) continue;
        bool known = false;
        for (auto& d : all) known |= d.hostId == id;
        if (!known) found += (found.empty() ? "" : ",") + deviceJson(nullptr, &p, false);
    }
    gScanner.setTargets(targets);

    std::string lan;
    for (auto& a : hl::net::localAddresses())
        if (a.rfind("100.", 0) != 0) lan += (lan.empty() ? "" : ", ") + a;
    auto ts = hl::net::tailscaleAddresses();
    std::string clients;
    for (auto& c : gHooks.clients()) clients += (clients.empty() ? "\"" : ",\"") + web::esc(c) + "\"";
    std::string j = "{\"type\":\"state\",\"devices\":[" + saved + "],\"found\":[" + found + "],\"pc\":{";
    j += "\"name\":\"" + web::esc(me.name) + "\",";
    j += "\"pin\":\"" + web::esc(me.pin) + "\",";
    j += "\"version\":\"" + web::esc(hostVersion()) + "\",";
    j += "\"lan\":\"" + web::esc(lan.empty() ? "not connected" : lan) + "\",";
    j += "\"tailscale\":\"" + web::esc(ts.empty() ? "" : ts[0]) + "\",";
    j += std::string("\"autoUpdate\":") + (me.checkUpdatesOnStart ? "true" : "false") + ",";
    j += "\"updateStatus\":\"" + web::esc(gHooks.updateStatus ? gHooks.updateStatus() : "") + "\",";
    j += "\"clients\":[" + clients + "]}}";
    gPanel->post(j);
}

void onMessage(const std::string& m) {
    hl::json::Value v;
    if (!hl::json::parse(m, v)) return;
    std::string cmd = v["cmd"].asString();
    if (cmd == "ready") {
        pushState();
        if (gPendingPcTab) gPanel->post("{\"type\":\"showPc\"}");
        gPendingPcTab = false;
    } else if (cmd == "connect") {
        std::string id = v["id"].asString();
        for (auto& d : devices::all())
            if (d.id == id) {
                if (v["newWindow"].asBool() || !SessionWindow::bringToFront(d.id)) SessionWindow::open(d);
            }
    } else if (cmd == "save") {
        SavedDevice d;
        std::string id = v["id"].asString();
        for (auto& x : devices::all())
            if (x.id == id) d = x;
        d.name = v["name"].asString();
        d.pin = v["pin"].asString();
        d.address = v["address"].asString();
        if (d.hostId.empty()) d.hostId = v["hostId"].asString();
        devices::save(d);
        pushState();
        gPanel->post("{\"type\":\"toast\",\"text\":\"Saved " + web::esc(d.name) + "\"}");
    } else if (cmd == "remove") {
        devices::remove(v["id"].asString());
        pushState();
    } else if (cmd == "thisPc") {
        HostSettings s = gHooks.settings();
        if (v["name"].type == hl::json::Value::String && !v["name"].str.empty()) s.name = v["name"].str;
        if (v["pin"].type == hl::json::Value::String) s.pin = v["pin"].str;
        if (v["autoUpdate"].type == hl::json::Value::Bool) s.checkUpdatesOnStart = v["autoUpdate"].b;
        gHooks.saveSettings(s);
        pushState();
    } else if (cmd == "checkUpdates") {
        if (gHooks.checkForUpdates) gHooks.checkForUpdates();
    } else if (cmd == "openLog") {
        if (gHooks.openLog) gHooks.openLog();
    }
}

LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            gWnd = h;
            web::styleWindow(h);
            gPanel = std::make_unique<web::Panel>(h, web::loadHtml(IDR_APP_HTML), onMessage, [h](bool ok) {
                if (!ok) {
                    // No WebView2 runtime: use the classic window instead.
                    PostMessageW(h, WM_CLOSE, 0, 0);
                    if (gHooks.fallback) gHooks.fallback();
                }
            });
            gScanner.start([] { if (gWnd) PostMessageW(gWnd, WM_H_REFRESH, 0, 0); });
            SetTimer(h, 1, 3000, nullptr);
            return 0;
        }
        case WM_SIZE: {
            RECT r;
            GetClientRect(h, &r);
            if (gPanel) gPanel->resize(r);
            return 0;
        }
        case WM_H_REFRESH:
        case WM_TIMER:
            pushState();
            return 0;
        case WM_GETMINMAXINFO: {
            auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
            mm->ptMinTrackSize = {MulDiv(560, GetDpiForWindow(h), 96), MulDiv(420, GetDpiForWindow(h), 96)};
            return 0;
        }
        case WM_DESTROY:
            KillTimer(h, 1);
            gScanner.stop();
            gPanel.reset();
            gWnd = nullptr;
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

void init(HINSTANCE inst, Hooks hooks) {
    gInst = inst;
    gHooks = std::move(hooks);
    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(RGB(11, 11, 11));
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
    wc.lpszClassName = L"HyperlinkHome";
    RegisterClassExW(&wc);
}

void show(bool thisPcTab) {
    if (gWnd) {
        ShowWindow(gWnd, SW_RESTORE);
        SetForegroundWindow(gWnd);
        if (thisPcTab && gPanel) gPanel->post("{\"type\":\"showPc\"}");
        return;
    }
    gPendingPcTab = thisPcTab;
    UINT dpi = GetDpiForSystem();
    HWND h = CreateWindowExW(0, L"HyperlinkHome", L"Hyperlink", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                             MulDiv(980, dpi, 96), MulDiv(660, dpi, 96), nullptr, nullptr, gInst, nullptr);
    ShowWindow(h, SW_SHOW);
    SetForegroundWindow(h);
}

void refresh() {
    if (gWnd) PostMessageW(gWnd, WM_H_REFRESH, 0, 0);
}

}  // namespace home
