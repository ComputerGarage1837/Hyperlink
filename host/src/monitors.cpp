#include "monitors.h"

#include <windows.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <map>

using Microsoft::WRL::ComPtr;

std::string toUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring fromUtf8(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

// Friendly monitor names ("DELL U2720Q") keyed by GDI device name.
static std::map<std::wstring, std::wstring> friendlyNames() {
    std::map<std::wstring, std::wstring> out;
    UINT32 pathCount = 0, modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS)
        return out;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount,
                           modes.data(), nullptr) != ERROR_SUCCESS)
        return out;
    for (UINT32 i = 0; i < pathCount; i++) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME src{};
        src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        src.header.size = sizeof src;
        src.header.adapterId = paths[i].sourceInfo.adapterId;
        src.header.id = paths[i].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS) continue;
        DISPLAYCONFIG_TARGET_DEVICE_NAME tgt{};
        tgt.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        tgt.header.size = sizeof tgt;
        tgt.header.adapterId = paths[i].targetInfo.adapterId;
        tgt.header.id = paths[i].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&tgt.header) != ERROR_SUCCESS) continue;
        if (tgt.monitorFriendlyDeviceName[0]) out[src.viewGdiDeviceName] = tgt.monitorFriendlyDeviceName;
    }
    return out;
}

std::vector<HostMonitor> enumerateMonitors() {
    std::vector<HostMonitor> out;
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return out;
    auto names = friendlyNames();

    ComPtr<IDXGIAdapter1> adapter;
    for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; a++) {
        ComPtr<IDXGIOutput> output;
        for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; o++) {
            DXGI_OUTPUT_DESC d{};
            output->GetDesc(&d);
            output.Reset();
            if (!d.AttachedToDesktop) continue;

            HostMonitor m;
            m.adapterIndex = (int)a;
            m.outputIndex = (int)o;
            m.gdiName = d.DeviceName;
            m.info.id = ((a + 1) << 8) | o;
            m.info.x = d.DesktopCoordinates.left;
            m.info.y = d.DesktopCoordinates.top;
            m.info.width = d.DesktopCoordinates.right - d.DesktopCoordinates.left;
            m.info.height = d.DesktopCoordinates.bottom - d.DesktopCoordinates.top;

            DEVMODEW dm{};
            dm.dmSize = sizeof dm;
            if (EnumDisplaySettingsW(d.DeviceName, ENUM_CURRENT_SETTINGS, &dm))
                m.info.refreshHz = (uint16_t)dm.dmDisplayFrequency;

            MONITORINFO mi{};
            mi.cbSize = sizeof mi;
            if (GetMonitorInfoW(d.Monitor, &mi)) m.info.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) ? 1 : 0;

            auto it = names.find(d.DeviceName);
            std::wstring name = it != names.end() ? it->second : std::wstring(d.DeviceName);
            m.info.name = toUtf8(name);
            out.push_back(m);
        }
        adapter.Reset();
    }
    return out;
}
