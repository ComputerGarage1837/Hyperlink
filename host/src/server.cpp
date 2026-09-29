#include "server.h"

#include <windows.h>

#include <algorithm>
#include <random>

#include "capture.h"
#include "encoder.h"
#include "hyperlink/common.h"
#include "hyperlink/video.h"
#include "input.h"
#include "stream.h"

#ifndef HYPERLINK_VERSION
#define HYPERLINK_VERSION "0.0.0"
#endif

std::string hostVersion() { return HYPERLINK_VERSION; }

// ---------------------------------------------------------------- Session

Session::Session(Server& server, hl::net::Socket sock, const hl::net::Addr& peer, uint32_t id)
    : server_(server), conn_(std::move(sock)), peer_(peer), id_(id) {}

Session::~Session() { close(); }

void Session::close() {
    conn_.shutdown();
    std::map<uint8_t, std::unique_ptr<Stream>> streams;
    {
        std::lock_guard<std::mutex> lock(streamsMutex_);
        streams.swap(streams_);
    }
    streams.clear();  // joins the stream threads
}

std::string Session::clientName() const {
    std::lock_guard<std::mutex> lock(nameMutex_);
    return clientName_;
}

int Session::streamCount() {
    std::lock_guard<std::mutex> lock(streamsMutex_);
    return (int)streams_.size();
}

void Session::setVideoAddr(const hl::net::Addr& a) {
    std::lock_guard<std::mutex> lock(videoMutex_);
    if (!haveVideo_ || !videoAddr_.sameAs(a)) hl::log("session %u: video goes to %s", id_, a.str().c_str());
    videoAddr_ = a;
    haveVideo_ = true;
}

void Session::sendVideo(const uint8_t* p, size_t n) {
    hl::net::Addr a;
    {
        std::lock_guard<std::mutex> lock(videoMutex_);
        a = videoAddr_;
    }
    server_.sendVideo(a, p, n);
}

void Session::sendCursorShape(uint64_t seq, const std::vector<uint8_t>& msg) {
    if (cursorShapeSeq_.exchange(seq) != seq) conn_.send(msg);
}

void Session::stopStream(uint8_t id) {
    std::unique_ptr<Stream> s;
    {
        std::lock_guard<std::mutex> lock(streamsMutex_);
        auto it = streams_.find(id);
        if (it == streams_.end()) return;
        s = std::move(it->second);
        streams_.erase(it);
    }
    s.reset();
    server_.statusChanged();
}

void Session::run() {
    std::vector<uint8_t> body;
    if (!conn_.read(body) || body.empty() || body[0] != hl::MSG_HELLO) return;
    hl::Reader r(body.data() + 1, body.size() - 1);
    hl::Hello hello;
    if (!hello.decode(r)) return;
    HostSettings st = server_.settings();
    if (!st.pin.empty() && hello.pin != st.pin) {
        hl::log("session %u: wrong PIN from %s", id_, peer_.str().c_str());
        hl::Writer w(hl::MSG_AUTH_FAILED);
        w.str(hello.pin.empty() ? "This PC needs a PIN" : "Wrong PIN");
        conn_.send(w.done());
        return;
    }
    authed_ = true;
    {
        std::lock_guard<std::mutex> lock(nameMutex_);
        clientName_ = hello.clientName.empty() ? peer_.ip() : hello.clientName;
    }
    hl::log("session %u: %s connected from %s (%ux%u@%u)", id_, clientName().c_str(), peer_.str().c_str(),
            hello.displayWidth, hello.displayHeight, hello.displayHz);

    hl::Welcome w;
    w.hostName = st.name;
    w.hostId = st.hostId;
    w.hostVersion = hostVersion();
    w.sessionId = id_;
    w.videoPort = hl::kVideoPort;
    w.codecMask = server_.codecMask();
    for (auto& m : server_.monitors()) w.monitors.push_back(m.info);
    conn_.send(w.encode());
    server_.statusChanged();

    while (conn_.read(body)) {
        if (!body.empty()) handle(body);
    }
    hl::log("session %u: %s disconnected", id_, clientName().c_str());
    close();
    input::releaseAll();
}

void Session::handle(const std::vector<uint8_t>& body) {
    hl::Reader r(body.data() + 1, body.size() - 1);
    switch (body[0]) {
        case hl::MSG_START_STREAM: {
            hl::StartStream req;
            if (!req.decode(r)) return;
            HostMonitor mon;
            if (!server_.findMonitor(req.monitorId, mon)) {
                hl::Writer w(hl::MSG_STREAM_ERROR);
                w.u8(req.streamId).str("That monitor is no longer connected");
                conn_.send(w.done());
                return;
            }
            stopStream(req.streamId);
            hl::log("session %u: stream %u -> %s, max %ux%u @%u, %u kbps, %s", id_, req.streamId,
                    mon.info.name.c_str(), req.maxWidth, req.maxHeight, req.fps, req.bitrateKbps,
                    hl::codecName(req.codec));
            auto s = std::make_unique<Stream>(server_, *this, req, mon);
            {
                std::lock_guard<std::mutex> lock(streamsMutex_);
                streams_[req.streamId] = std::move(s);
            }
            server_.statusChanged();
            break;
        }
        case hl::MSG_STOP_STREAM:
            stopStream(r.u8());
            break;
        case hl::MSG_REQUEST_IDR: {
            uint8_t id = r.u8();
            std::lock_guard<std::mutex> lock(streamsMutex_);
            auto it = streams_.find(id);
            if (it != streams_.end()) it->second->requestKeyframe();
            break;
        }
        case hl::MSG_PING: {
            uint64_t t = r.u64();
            hl::Writer w(hl::MSG_PONG);
            w.u64(t);
            conn_.send(w.done());
            break;
        }
        case hl::MSG_MOUSE_ABS: {
            uint32_t mid = r.u32();
            uint16_t x = r.u16(), y = r.u16();
            HostMonitor mon;
            if (r.ok && server_.findMonitor(mid, mon)) input::mouseAbs(mon.info, x, y);
            break;
        }
        case hl::MSG_MOUSE_REL: {
            int dx = r.i16(), dy = r.i16();
            if (r.ok) input::mouseRel(dx, dy);
            break;
        }
        case hl::MSG_MOUSE_BUTTON: {
            uint8_t b = r.u8(), down = r.u8();
            if (r.ok) input::mouseButton(b, down != 0);
            break;
        }
        case hl::MSG_MOUSE_SCROLL: {
            int dy = r.i16(), dx = r.i16();
            if (r.ok) input::scroll(dy, dx);
            break;
        }
        case hl::MSG_KEY: {
            uint16_t vk = r.u16();
            uint8_t down = r.u8();
            if (r.ok) input::key(vk, down != 0);
            break;
        }
        case hl::MSG_TEXT: {
            std::string t = r.str();
            if (r.ok) input::text(t);
            break;
        }
        default:
            break;
    }
}

// ---------------------------------------------------------------- Server

Server::Server(HostSettings settings) : settings_(std::move(settings)) {}

Server::~Server() { stop(); }

HostSettings Server::settings() {
    std::lock_guard<std::mutex> lock(settingsMutex_);
    return settings_;
}

void Server::updateSettings(const HostSettings& s) {
    std::lock_guard<std::mutex> lock(settingsMutex_);
    settings_ = s;
    settings_.save();
}

std::vector<HostMonitor> Server::monitors() {
    std::lock_guard<std::mutex> lock(monitorsMutex_);
    return monitors_;
}

bool Server::findMonitor(uint32_t id, HostMonitor& out) {
    std::lock_guard<std::mutex> lock(monitorsMutex_);
    for (auto& m : monitors_)
        if (m.info.id == id) {
            out = m;
            return true;
        }
    return false;
}

bool Server::start() {
    hl::net::init();
    {
        std::lock_guard<std::mutex> lock(monitorsMutex_);
        monitors_ = enumerateMonitors();
    }
    listen_ = hl::net::Socket::listenTcp(hl::kControlPort);
    video_ = hl::net::Socket::udp(hl::kVideoPort);
    discovery_ = hl::net::Socket::udp(hl::kDiscoveryPort);
    if (!listen_.valid() || !video_.valid()) {
        hl::log("server: cannot open ports %u/%u (is another host running?)", hl::kControlPort, hl::kVideoPort);
        return false;
    }
    video_.setBuffers(8 * 1024 * 1024);
    if (discovery_.valid()) discovery_.enableBroadcast();
    nextSessionId_ = std::random_device()() | 1;
    running_ = true;
    threads_.emplace_back([this] { acceptLoop(); });
    threads_.emplace_back([this] { videoLoop(); });
    if (discovery_.valid()) threads_.emplace_back([this] { discoveryLoop(); });
    threads_.emplace_back([this] { monitorLoop(); });

    // Work out which codecs the GPU can encode, off the startup path.
    threads_.emplace_back([this] {
        auto mons = monitors();
        int adapter = mons.empty() ? 0 : mons[0].adapterIndex;
        if (auto gpu = GpuDevice::forAdapter(adapter)) {
            codecMask_ = Encoder::probeCodecs(gpu->device.Get(), &gpu->lock);
            hl::log("server: encodable codecs mask 0x%x", codecMask_.load());
        }
    });
    hl::log("server: listening on TCP %u, UDP %u, discovery %u", hl::kControlPort, hl::kVideoPort,
            hl::kDiscoveryPort);
    return true;
}

void Server::stop() {
    if (!running_.exchange(false)) return;
    listen_.shutdown();
    listen_.close();
    video_.close();
    discovery_.close();
    std::vector<std::shared_ptr<Session>> sessions;
    {
        std::lock_guard<std::mutex> lock(sessionsMutex_);
        sessions = sessions_;
    }
    for (auto& s : sessions) s->close();
    for (auto& t : threads_)
        if (t.joinable()) t.join();
    threads_.clear();
}

void Server::acceptLoop() {
    while (running_) {
        hl::net::Addr peer;
        hl::net::Socket c = listen_.accept(&peer);
        if (!c.valid()) {
            if (!running_) break;
            Sleep(50);
            continue;
        }
        std::shared_ptr<Session> s;
        {
            std::lock_guard<std::mutex> lock(sessionsMutex_);
            s = std::make_shared<Session>(*this, std::move(c), peer, nextSessionId_++);
            sessions_.push_back(s);
        }
        std::thread([this, s] {
            s->run();
            removeSession(s.get());
        }).detach();
    }
}

void Server::removeSession(Session* s) {
    {
        std::lock_guard<std::mutex> lock(sessionsMutex_);
        sessions_.erase(std::remove_if(sessions_.begin(), sessions_.end(),
                                       [s](auto& p) { return p.get() == s; }),
                        sessions_.end());
    }
    statusChanged();
}

void Server::videoLoop() {
    uint8_t buf[2048];
    while (running_) {
        hl::net::Addr from;
        int n = video_.recvFrom(buf, sizeof buf, &from, 500);
        if (n < 8 || buf[0] != hl::kPacketMagic || buf[2] != hl::PKT_PUNCH) continue;
        uint32_t id = hl::get32(buf + 4);
        std::lock_guard<std::mutex> lock(sessionsMutex_);
        for (auto& s : sessions_)
            if (s->id() == id) s->setVideoAddr(from);
    }
}

void Server::discoveryLoop() {
    uint8_t buf[512];
    while (running_) {
        hl::net::Addr from;
        int n = discovery_.recvFrom(buf, sizeof buf, &from, 500);
        if (n < (int)hl::kDiscoverTagLen || memcmp(buf, hl::kDiscoverQuery, hl::kDiscoverTagLen) != 0)
            continue;
        HostSettings st = settings();
        hl::DiscoveryReply d;
        d.hostId = st.hostId;
        d.hostName = st.name;
        d.version = hostVersion();
        d.pinRequired = st.pin.empty() ? 0 : 1;
        Status s = status();
        d.clients = (uint16_t)s.clients.size();
        d.streams = (uint16_t)s.streams;
        auto reply = d.encode();
        discovery_.sendTo(from, reply.data(), reply.size());
    }
}

static bool sameLayout(const std::vector<HostMonitor>& a, const std::vector<HostMonitor>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) {
        auto &x = a[i].info, &y = b[i].info;
        if (x.id != y.id || x.x != y.x || x.y != y.y || x.width != y.width || x.height != y.height ||
            x.refreshHz != y.refreshHz || x.primary != y.primary)
            return false;
    }
    return true;
}

void Server::monitorLoop() {
    while (running_) {
        for (int i = 0; i < 20 && running_; i++) Sleep(100);
        auto now = enumerateMonitors();
        bool changed;
        {
            std::lock_guard<std::mutex> lock(monitorsMutex_);
            changed = !now.empty() && !sameLayout(now, monitors_);
            if (changed) monitors_ = now;
        }
        if (!changed) continue;
        hl::log("server: monitor layout changed (%d monitors)", (int)now.size());
        hl::Monitors m;
        for (auto& x : now) m.monitors.push_back(x.info);
        auto msg = m.encode();
        std::lock_guard<std::mutex> lock(sessionsMutex_);
        for (auto& s : sessions_) s->sendControl(msg);
    }
}

Server::Status Server::status() {
    Status st;
    std::lock_guard<std::mutex> lock(sessionsMutex_);
    for (auto& s : sessions_) {
        if (s->clientName().empty()) continue;
        int n = s->streamCount();
        st.streams += n;
        st.clients.push_back(s->clientName() + (n == 1 ? " (1 screen)" : " (" + std::to_string(n) + " screens)"));
    }
    return st;
}
