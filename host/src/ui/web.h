// A WebView2 panel inside one of our windows, used to draw the modern UI in HTML/CSS.
// Messages are JSON strings in both directions.
#pragma once

#include <windows.h>

#include <functional>
#include <string>

namespace web {

class Panel {
public:
    // Starts creating the web view asynchronously; onReady(false) if WebView2 isn't available.
    Panel(HWND parent, std::wstring html, std::function<void(const std::string&)> onMessage,
          std::function<void(bool)> onReady = nullptr);
    ~Panel();
    void post(const std::string& json);  // to the page
    void resize(const RECT& r);
    void show(bool visible);
    bool ready() const { return ready_; }

private:
    struct Impl;
    Impl* impl_;
    bool ready_ = false;
};

// Reads an HTML page embedded as an RCDATA resource.
std::wstring loadHtml(int resourceId);
// JSON string escaping for building messages.
std::string esc(const std::string& s);
// Dark title bar and, on Windows 11, the Mica backdrop.
void styleWindow(HWND h);

}  // namespace web
