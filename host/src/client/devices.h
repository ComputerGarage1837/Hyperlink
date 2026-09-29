// Saved devices (the roster) and live presence for the Windows client.
// Devices are identified by the host's permanent id, not by IP: the current address is
// looked up on the network every time, so router changes and DHCP don't matter.
#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "hyperlink/protocol.h"

struct SavedDevice {
    std::string id;          // local id for this entry
    std::string name;        // the user's name for it
    std::string hostId;      // the host's permanent id (empty until first seen/connected)
    std::string address;     // optional fallback the user typed: host name, Tailscale name or IP
    std::string remoteAddress;  // the host's Tailscale address, learned automatically
    uint16_t port = hl::kControlPort;
    std::string pin;
};

namespace devices {
std::vector<SavedDevice> all();
void save(const SavedDevice& d);
void remove(const std::string& id);
std::vector<uint32_t> hiddenScreens(const std::string& id);
void setHiddenScreens(const std::string& id, const std::vector<uint32_t>& monitorIds);
}  // namespace devices

struct Presence {
    std::string address;     // where it answered from, right now
    hl::DiscoveryReply info;
    uint64_t seenMs = 0;
};

// Broadcasts discovery queries every few seconds (and asks saved fallback addresses directly).
class PresenceScanner {
public:
    ~PresenceScanner() { stop(); }
    void start(std::function<void()> onChange);
    void stop();
    std::map<std::string, Presence> snapshot();  // by hostId
    void setTargets(std::vector<std::string> t);

    // Blocking one-off lookup of a host's current address by id (LAN broadcast).
    static bool locate(const std::string& hostId, std::string& address, int timeoutMs = 900);

private:
    void loop();
    std::atomic<bool> running_{false};
    std::thread thread_;
    std::mutex m_;
    std::map<std::string, Presence> seen_;
    std::vector<std::string> targets_;
    std::function<void()> onChange_;
};

uint64_t nowMs();
