// A connection to another device, in its own window. Shows the remote monitors together or
// one at a time; any monitor can be popped out into a window of its own.
#pragma once

#include <windows.h>

#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "devices.h"
#include "hyperlink/client.h"
#include "videoview.h"

class SessionWindow : public hl::ClientListener {
public:
    // Opens a new window and starts connecting. The window owns itself and deletes itself on close.
    static void open(const SavedDevice& d);
    static void registerClasses(HINSTANCE inst);
    static int openCount();

    // hl::ClientListener (network threads): forwarded to the window thread.
    void onMonitors(const std::vector<hl::MonitorInfo>& m) override;
    void onStreamStarted(const hl::StreamStarted& s) override;
    void onStreamError(uint8_t id, const std::string& msg) override;
    void onCursorShape(const hl::CursorShape& c) override;
    void onCursorPos(uint32_t mon, uint16_t x, uint16_t y, bool visible) override;
    void onDisconnected(const std::string& reason) override;

private:
    explicit SessionWindow(const SavedDevice& d);
    ~SessionWindow() override;

    struct Screen {
        hl::MonitorInfo info;
        uint8_t streamId = 0;
        std::shared_ptr<VideoView> view;
        HWND popout = nullptr;   // its own window, when popped out
        bool running = false;
    };

    static LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp);
    static LRESULT CALLBACK popoutProc(HWND h, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);
    void connect();
    void setMonitors(const std::vector<hl::MonitorInfo>& m);
    void layout();
    void syncStreams();
    void stopAll();
    void buildMenu();
    void popOut(uint32_t monitorId);
    void popIn(HWND popout);
    void toggleFullscreen(HWND w);
    bool keyEvent(UINT msg, WPARAM wp, LPARAM lp);
    void updateTitle();
    void setStatus(const std::wstring& s);
    uint8_t chooseCodec() const;
    Screen* screenFor(uint32_t monitorId);

    // The connect thread may outlive the window; whoever finishes last deletes this object.
    std::mutex life_;
    bool connecting_ = false;
    bool closed_ = false;

    HWND hwnd_ = nullptr;
    HWND status_ = nullptr;   // text shown while connecting / on errors
    SavedDevice device_;
    hl::Client client_;
    bool connected_ = false;
    std::string hostName_;
    uint32_t hostCodecs_ = 0;
    std::vector<Screen> screens_;
    int focused_ = -1;         // index into screens_, -1 = all
    std::set<uint32_t> hidden_;
    HCURSOR cursor_ = nullptr;
    uint32_t cursorMonitor_ = 0;
    bool cursorVisible_ = true;
    bool showStats_ = true;
    std::map<HWND, WINDOWPLACEMENT> fullscreen_;
};
