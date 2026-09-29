// Accepts clients, runs their sessions and streams, answers discovery.
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
#include "monitors.h"
#include "settings.h"

class Server;
class Stream;

class Session : public std::enable_shared_from_this<Session> {
public:
    Session(Server& server, hl::net::Socket sock, const hl::net::Addr& peer, uint32_t id);
    ~Session();
    void run();
    void close();

    // Called by streams.
    void sendControl(const std::vector<uint8_t>& msg) { conn_.send(msg); }
    void sendVideo(const uint8_t* p, size_t n);
    bool hasVideoAddr() const { return haveVideo_; }
    void setVideoAddr(const hl::net::Addr& a);
    // Sends the cursor shape unless this session already has that version.
    void sendCursorShape(uint64_t seq, const std::vector<uint8_t>& msg);

    uint32_t id() const { return id_; }
    std::string clientName() const;
    int streamCount();

private:
    void handle(const std::vector<uint8_t>& body);
    void stopStream(uint8_t id);

    Server& server_;
    hl::net::MessageConn conn_;
    hl::net::Addr peer_;
    uint32_t id_;
    mutable std::mutex nameMutex_;
    std::string clientName_;
    bool authed_ = false;

    std::mutex videoMutex_;
    hl::net::Addr videoAddr_;
    std::atomic<bool> haveVideo_{false};
    std::atomic<uint64_t> cursorShapeSeq_{0};

    std::mutex streamsMutex_;
    std::map<uint8_t, std::unique_ptr<Stream>> streams_;
};

class Server {
public:
    explicit Server(HostSettings settings);
    ~Server();
    bool start();
    void stop();

    HostSettings settings();
    void updateSettings(const HostSettings& s);
    std::vector<HostMonitor> monitors();
    bool findMonitor(uint32_t id, HostMonitor& out);
    uint32_t codecMask() const { return codecMask_; }
    uint16_t nextEpoch() { return (uint16_t)++epoch_; }
    void sendVideo(const hl::net::Addr& to, const uint8_t* p, size_t n) { video_.sendTo(to, p, n); }

    struct Status {
        std::vector<std::string> clients;  // "name (2 screens)"
        int streams = 0;
    };
    Status status();
    // Fired (from any thread) when clients connect, disconnect or start/stop streams.
    std::function<void()> onStatusChanged;
    void statusChanged() { if (onStatusChanged) onStatusChanged(); }

private:
    friend class Session;
    void acceptLoop();
    void videoLoop();
    void discoveryLoop();
    void monitorLoop();
    void removeSession(Session* s);

    std::mutex settingsMutex_;
    HostSettings settings_;
    std::atomic<bool> running_{false};
    hl::net::Socket listen_, video_, discovery_;
    std::vector<std::thread> threads_;

    std::mutex sessionsMutex_;
    std::vector<std::shared_ptr<Session>> sessions_;
    uint32_t nextSessionId_ = 1;

    std::mutex monitorsMutex_;
    std::vector<HostMonitor> monitors_;
    std::atomic<uint32_t> codecMask_{1};
    std::atomic<uint32_t> epoch_{0};
};

std::string hostVersion();
