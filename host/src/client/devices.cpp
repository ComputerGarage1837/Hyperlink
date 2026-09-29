#include "devices.h"

#include <windows.h>

#include <chrono>
#include <cstring>
#include <random>

#include "../monitors.h"
#include "../settings.h"
#include "hyperlink/net.h"

uint64_t nowMs() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

namespace devices {
namespace {

std::wstring path() { return dataDir() + L"\\devices.ini"; }

std::string get(const std::wstring& sec, const wchar_t* key) {
    wchar_t buf[1024];
    GetPrivateProfileStringW(sec.c_str(), key, L"", buf, 1024, path().c_str());
    return toUtf8(buf);
}

void put(const std::wstring& sec, const wchar_t* key, const std::string& v) {
    WritePrivateProfileStringW(sec.c_str(), key, fromUtf8(v).c_str(), path().c_str());
}

}  // namespace

std::vector<SavedDevice> all() {
    std::vector<SavedDevice> out;
    std::vector<wchar_t> names(64 * 1024);
    DWORD n = GetPrivateProfileSectionNamesW(names.data(), (DWORD)names.size(), path().c_str());
    for (const wchar_t* p = names.data(); p < names.data() + n && *p; p += wcslen(p) + 1) {
        std::wstring sec = p;
        if (sec.rfind(L"device.", 0) != 0) continue;
        SavedDevice d;
        d.id = toUtf8(sec.substr(7));
        d.name = get(sec, L"name");
        d.hostId = get(sec, L"hostId");
        d.address = get(sec, L"address");
        d.pin = get(sec, L"pin");
        int port = atoi(get(sec, L"port").c_str());
        d.port = port > 0 ? (uint16_t)port : hl::kControlPort;
        out.push_back(d);
    }
    return out;
}

void save(const SavedDevice& in) {
    SavedDevice d = in;
    if (d.id.empty()) {
        std::random_device rd;
        char b[17];
        snprintf(b, sizeof b, "%08x%08x", rd(), rd());
        d.id = b;
    }
    std::wstring sec = L"device." + fromUtf8(d.id);
    put(sec, L"name", d.name);
    put(sec, L"hostId", d.hostId);
    put(sec, L"address", d.address);
    put(sec, L"pin", d.pin);
    put(sec, L"port", std::to_string(d.port));
}

void remove(const std::string& id) {
    WritePrivateProfileStringW((L"device." + fromUtf8(id)).c_str(), nullptr, nullptr, path().c_str());
}

std::vector<uint32_t> hiddenScreens(const std::string& id) {
    std::vector<uint32_t> out;
    std::string v = get(L"device." + fromUtf8(id), L"hidden");
    size_t i = 0;
    while (i < v.size()) {
        size_t j = v.find(',', i);
        if (j == std::string::npos) j = v.size();
        if (j > i) out.push_back((uint32_t)strtoul(v.substr(i, j - i).c_str(), nullptr, 10));
        i = j + 1;
    }
    return out;
}

void setHiddenScreens(const std::string& id, const std::vector<uint32_t>& ids) {
    if (id.empty()) return;
    std::string v;
    for (auto x : ids) v += (v.empty() ? "" : ",") + std::to_string(x);
    put(L"device." + fromUtf8(id), L"hidden", v);
}

}  // namespace devices

static void sendQueries(hl::net::Socket& s, const std::vector<std::string>& targets) {
    const size_t n = hl::kDiscoverTagLen;
    s.sendTo(hl::net::Addr::broadcast(hl::kDiscoveryPort), hl::kDiscoverQuery, n);
    for (auto& t : targets) {
        hl::net::Addr a;
        if (!t.empty() && hl::net::Addr::resolve(t, hl::kDiscoveryPort, a)) s.sendTo(a, hl::kDiscoverQuery, n);
    }
}

void PresenceScanner::start(std::function<void()> onChange) {
    if (running_.exchange(true)) return;
    onChange_ = std::move(onChange);
    thread_ = std::thread([this] { loop(); });
}

void PresenceScanner::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

void PresenceScanner::setTargets(std::vector<std::string> t) {
    std::lock_guard<std::mutex> lock(m_);
    targets_ = std::move(t);
}

std::map<std::string, Presence> PresenceScanner::snapshot() {
    std::lock_guard<std::mutex> lock(m_);
    return seen_;
}

void PresenceScanner::loop() {
    hl::net::init();
    hl::net::Socket s = hl::net::Socket::udp(0);
    s.enableBroadcast();
    uint8_t buf[1024];
    while (running_) {
        std::vector<std::string> targets;
        {
            std::lock_guard<std::mutex> lock(m_);
            targets = targets_;
        }
        sendQueries(s, targets);
        uint64_t start = nowMs();
        while (running_ && nowMs() - start < 2500) {
            hl::net::Addr from;
            int n = s.recvFrom(buf, sizeof buf, &from, 200);
            if (n <= 0) continue;
            hl::DiscoveryReply r;
            if (!r.decode(buf, (size_t)n) || r.hostId.empty()) continue;
            std::lock_guard<std::mutex> lock(m_);
            Presence& p = seen_[r.hostId];
            p.address = from.ip();
            p.info = r;
            p.seenMs = nowMs();
        }
        {
            std::lock_guard<std::mutex> lock(m_);
            for (auto it = seen_.begin(); it != seen_.end();)
                it = nowMs() - it->second.seenMs > 8000 ? seen_.erase(it) : std::next(it);
        }
        if (onChange_) onChange_();
    }
}

bool PresenceScanner::locate(const std::string& hostId, std::string& address, int timeoutMs) {
    if (hostId.empty()) return false;
    hl::net::init();
    hl::net::Socket s = hl::net::Socket::udp(0);
    s.enableBroadcast();
    uint8_t buf[1024];
    uint64_t start = nowMs();
    uint64_t lastSend = 0;
    while (nowMs() - start < (uint64_t)timeoutMs) {
        if (nowMs() - lastSend > 300) {
            sendQueries(s, {});
            lastSend = nowMs();
        }
        hl::net::Addr from;
        int n = s.recvFrom(buf, sizeof buf, &from, 100);
        if (n <= 0) continue;
        hl::DiscoveryReply r;
        if (r.decode(buf, (size_t)n) && r.hostId == hostId) {
            address = from.ip();
            return true;
        }
    }
    return false;
}
