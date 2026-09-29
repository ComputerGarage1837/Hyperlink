#pragma once

#include <string>

// Host settings, stored in %LOCALAPPDATA%\Hyperlink\host.ini.
struct HostSettings {
    std::string name;      // shown to clients; defaults to the computer name
    std::string pin;       // clients must send this to connect; empty = no PIN
    std::string hostId;    // random, generated once; lets clients recognise the host on any IP
    bool checkUpdatesOnStart = true;
    std::string skippedVersion;

    static HostSettings load();
    void save() const;
};

std::wstring dataDir();  // %LOCALAPPDATA%\Hyperlink, created on demand
