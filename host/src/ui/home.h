// The main Hyperlink window (devices and This PC), drawn with WebView2. Falls back to the
// classic device list when WebView2 isn't installed.
#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

#include "../settings.h"

namespace home {

struct Hooks {
    std::function<HostSettings()> settings;
    std::function<void(const HostSettings&)> saveSettings;
    std::function<std::vector<std::string>()> clients;   // "Name (2 screens)"
    std::function<void()> checkForUpdates;
    std::function<std::string()> updateStatus;
    std::function<void()> openLog;
    std::function<void()> fallback;                      // show the classic window instead
};

void init(HINSTANCE inst, Hooks hooks);
void show(bool thisPcTab = false);
void refresh();  // e.g. after the update status changed

}  // namespace home
