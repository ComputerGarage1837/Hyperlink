// The device list: saved devices with live status, devices found on the network, this PC's
// own name and PIN, and the buttons to connect, add, edit and remove.
#pragma once

#include <windows.h>

#include <functional>
#include <string>

namespace roster {

struct Hooks {
    std::function<void()> openHostSettings;
    std::function<void()> checkForUpdates;
    std::function<std::string()> thisPcSummary;  // "Name · PIN 123456"
    std::function<std::string()> thisHostId;
};

void registerClasses(HINSTANCE inst, Hooks hooks);
void show();  // opens the window, or brings it forward

}  // namespace roster
