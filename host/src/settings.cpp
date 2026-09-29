#include "settings.h"

#include <windows.h>
#include <shlobj.h>

#include <random>

#include "hyperlink/net.h"
#include "monitors.h"

std::wstring dataDir() {
    PWSTR base = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base))) {
        dir = std::wstring(base) + L"\\Hyperlink";
        CoTaskMemFree(base);
    } else {
        dir = L".";
    }
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

static std::wstring iniPath() { return dataDir() + L"\\host.ini"; }

static std::string readStr(const wchar_t* key, const std::string& def) {
    wchar_t buf[512];
    GetPrivateProfileStringW(L"host", key, fromUtf8(def).c_str(), buf, 512, iniPath().c_str());
    return toUtf8(buf);
}

static void writeStr(const wchar_t* key, const std::string& v) {
    WritePrivateProfileStringW(L"host", key, fromUtf8(v).c_str(), iniPath().c_str());
}

static std::string randomDigits(int n, bool hex) {
    std::random_device rd;
    std::string s;
    const char* digits = hex ? "0123456789abcdef" : "0123456789";
    for (int i = 0; i < n; i++) s += digits[rd() % (hex ? 16 : 10)];
    return s;
}

HostSettings HostSettings::load() {
    HostSettings s;
    bool fresh = GetFileAttributesW(iniPath().c_str()) == INVALID_FILE_ATTRIBUTES;
    s.name = readStr(L"name", hl::net::hostName());
    s.pin = readStr(L"pin", fresh ? randomDigits(6, false) : "");
    s.hostId = readStr(L"hostId", "");
    s.checkUpdatesOnStart = readStr(L"checkUpdates", "1") != "0";
    s.skippedVersion = readStr(L"skippedVersion", "");
    if (s.hostId.empty() || fresh) {
        if (s.hostId.empty()) s.hostId = randomDigits(16, true);
        s.save();
    }
    return s;
}

void HostSettings::save() const {
    writeStr(L"name", name);
    writeStr(L"pin", pin);
    writeStr(L"hostId", hostId);
    writeStr(L"checkUpdates", checkUpdatesOnStart ? "1" : "0");
    writeStr(L"skippedVersion", skippedVersion);
}
