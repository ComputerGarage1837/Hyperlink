// One video stream: one monitor, for one client. Waits for new desktop frames, paces them to
// the requested frame rate, encodes, and sends them.
#pragma once

#include <atomic>
#include <thread>

#include "hyperlink/protocol.h"
#include "monitors.h"

class Session;
class Server;

class Stream {
public:
    Stream(Server& server, Session& session, const hl::StartStream& req, const HostMonitor& mon);
    ~Stream();
    void requestKeyframe() { idr_ = true; }
    uint32_t monitorId() const { return mon_.info.id; }

private:
    void run();

    Server& server_;
    Session& session_;
    hl::StartStream req_;
    HostMonitor mon_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> idr_{true};
    std::thread thread_;
};
