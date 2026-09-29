// Portable host side: accepts clients (PIN sign-in), answers discovery, receives input, and
// sends video that the platform encodes. Used by the Android app to share the phone.
// (The Windows host has its own capture/encode pipeline in host/.)
//
// Simplification that suits a phone: one screen and one encoder. Every client that starts a
// stream receives the same encoded frames.
#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "hyperlink/net.h"
#include "hyperlink/protocol.h"
#include "hyperlink/video.h"

namespace hl {

class HostCore {
public:
    struct Config {
        std::string name, pin, hostId, version;
        uint16_t controlPort = kControlPort, videoPort = kVideoPort, discoveryPort = kDiscoveryPort;
        uint32_t codecMask = 1u << CODEC_H264;
    };
    struct Callbacks {
        std::function<std::vector<MonitorInfo>()> monitors;
        // The platform starts (or reconfigures) its encoder, then calls streamReady().
        std::function<void(const StartStream& req)> startEncoder;
        std::function<void()> stopEncoder;       // no client wants video any more
        std::function<void()> requestKeyframe;
        // Input messages, as received (type byte + fields). See protocol.h for layouts.
        std::function<void(const std::vector<uint8_t>& body)> input;
        std::function<void()> statusChanged;
    };

    HostCore(Config cfg, Callbacks cb);
    ~HostCore();
    bool start();
    void stop();
    void setPin(const std::string& pin);
    void setName(const std::string& name);

    // Platform -> clients.
    void streamReady(uint16_t width, uint16_t height, uint16_t fps, uint8_t codec, const std::string& encoderName);
    void sendFrame(const uint8_t* data, size_t size, bool keyframe, uint64_t captureUs);
    void monitorsChanged();

    int clientCount();
    int streamCount();

private:
    struct Session;
    void acceptLoop();
    void videoLoop();
    void discoveryLoop();
    void runSession(std::shared_ptr<Session> s);
    void handle(const std::shared_ptr<Session>& s, const std::vector<uint8_t>& body);
    void updateEncoder();

    Config cfg_;
    Callbacks cb_;
    std::mutex cfgMutex_;
    std::atomic<bool> running_{false};
    net::Socket listen_, video_, discovery_;
    std::vector<std::thread> threads_;

    std::mutex m_;
    std::vector<std::shared_ptr<Session>> sessions_;
    uint32_t nextId_ = 1;
    uint16_t epoch_ = 0;
    bool encoderRunning_ = false;
    StreamStarted current_;  // what the encoder produces now
    bool currentValid_ = false;
    uint32_t frameIndex_ = 0;
};

}  // namespace hl
