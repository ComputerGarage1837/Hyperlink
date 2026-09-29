#pragma once

#include <string>
#include <vector>

#include "hyperlink/protocol.h"

struct HostMonitor {
    hl::MonitorInfo info;
    int adapterIndex = 0;
    int outputIndex = 0;
    std::wstring gdiName;  // \\.\DISPLAY1
};

// Every monitor attached to the desktop, in DXGI order. Coordinates are physical pixels
// (the process is per-monitor DPI aware).
std::vector<HostMonitor> enumerateMonitors();

std::string toUtf8(const std::wstring& w);
std::wstring fromUtf8(const std::string& s);
