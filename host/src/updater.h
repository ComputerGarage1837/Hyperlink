// In-app updates from this project's GitHub releases.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

#ifndef HYPERLINK_REPO
#define HYPERLINK_REPO "ComputerGarage1837/Hyperlink"
#endif

struct ReleaseInfo {
    std::string version;     // "0.2.0"
    std::string notes;       // release body (markdown)
    std::string assetName;
    std::string assetUrl;
    std::string sha256;      // lowercase hex, empty if GitHub didn't give one
    uint64_t size = 0;
};

namespace updater {

// Reads the newest release and picks the Windows setup program from its assets.
bool latest(ReleaseInfo& out, std::string& error);
bool isNewer(const std::string& version, const std::string& current);
// Downloads to `path`, verifying size and SHA-256. progress(done, total) may be called often.
bool download(const ReleaseInfo& r, const std::wstring& path,
              const std::function<void(uint64_t, uint64_t)>& progress, std::string& error);
// Starts the setup program silently; the installer restarts the app when it is done.
bool runInstaller(const std::wstring& path);

}  // namespace updater
