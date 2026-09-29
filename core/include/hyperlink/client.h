// Client side of a connection to one host: control channel, video reception, loss recovery.
// Platform code supplies a FrameSink (decoder) per stream and a listener for events.
// Several Clients can run at once (one per host window).
#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "hyperlink/net.h"
#include "hyperlink/protocol.h"
#include "hyperlink/video.h"

namespace hl {

class ClientListener {
public:
    virtual ~ClientListener() = default;
    virtual void onMonitors(const std::vector<MonitorInfo>&) {}
    virtual void onStreamStarted(const StreamStarted&) {}
    virtual void onStreamError(uint8_t /*streamId*/, const std::string&) {}
    virtual void onCursorShape(const CursorShape&) {}
    virtual void onCursorPos(uint32_t /*monitorId*/, uint16_t /*x*/, uint16_t /*y*/, bool /*visible*/) {}
    virtual void onHostStats(const HostStats&) {}
    virtual void onDisconnected(const std::string& /*reason*/) {}
};

class FrameSink {
public:
    virtual ~FrameSink() = default;
    // A complete frame, in order. Called on the network thread; keep it quick.
    // Return false if the decoder broke and needs a fresh keyframe.
    virtual bool onFrame(const CompleteFrame& f, const StreamStarted& info) = 0;
};

struct StreamStats {
    bool valid = false;
    uint16_t width = 0, height = 0;
    uint8_t codec = 0;
    std::string encoderName;
    float fps = 0;             // frames completed per second
    float mbps = 0;            // video bitrate received
    float lossPercent = 0;     // shards that never arrived (before FEC)
    uint32_t framesLost = 0;   // frames FEC could not save (total)
    uint32_t recovered = 0;    // shards rebuilt by FEC in the last period
    float hostMs = 0;          // capture -> sent, on the host
    float assemblyMs = 0;      // first packet -> frame complete, here
    HostStats host;
};

class Client {
public:
    explicit Client(ClientListener* listener);
    ~Client();

    // Connects and says hello. Blocks. Returns an empty string on success, else the reason.
    std::string connect(const std::string& host, uint16_t port, const Hello& hello, Welcome& welcome,
                        int timeoutMs = 5000);
    void disconnect();
    bool connected() const { return connected_; }

    void startStream(const StartStream& req, std::shared_ptr<FrameSink> sink);
    void stopStream(uint8_t streamId);
    // Swaps the decoder of a running stream (e.g. its surface was recreated) and asks for a keyframe.
    void setSink(uint8_t streamId, std::shared_ptr<FrameSink> sink);
    void requestKeyframe(uint8_t streamId);

    // Input and anything else: pre-encoded control messages.
    void send(const std::vector<uint8_t>& msg);
    void mouseAbs(uint32_t monitorId, uint16_t x, uint16_t y);
    void mouseRel(int dx, int dy);
    void mouseButton(uint8_t button, bool down);
    void scroll(int dy, int dx);
    void key(uint16_t vk, bool down);
    void text(const std::string& utf8);

    float rttMs() const { return rttUs_ / 1000.0f; }
    // Stats since the previous call for this stream.
    StreamStats takeStats(uint8_t streamId);

private:
    struct Stream {
        StartStream req;
        StreamStarted info;
        bool started = false;
        std::shared_ptr<FrameSink> sink;
        Reassembler ra;
        bool waitingKey = true;
        uint64_t lastIdrRequest = 0;
        // stats
        uint64_t statStart = 0, bytes = 0, frames = 0, hostUsSum = 0, assemblyUsSum = 0;
        uint64_t shardsAtStart = 0, recoveredAtStart = 0, lostShardsEstimate = 0;
        HostStats host;
    };

    void controlLoop();
    void videoLoop();
    void timerLoop();
    void punch();
    void handle(const std::vector<uint8_t>& body);
    void onFrame(uint8_t id, Stream& s, CompleteFrame&& f);
    void needKeyframe(uint8_t id, Stream& s, bool force);
    void closeAll(const std::string& reason);

    ClientListener* listener_;
    std::unique_ptr<net::MessageConn> conn_;
    net::Socket udp_;
    net::Addr hostVideo_;
    uint32_t sessionId_ = 0;
    std::atomic<bool> connected_{false};
    std::atomic<bool> stopping_{false};
    std::thread control_, video_, timer_;
    std::atomic<uint32_t> rttUs_{0};

    std::mutex streamsMutex_;
    std::map<uint8_t, std::unique_ptr<Stream>> streams_;
};

}  // namespace hl
