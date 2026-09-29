#include "hyperlink/hostcore.h"

#include <algorithm>
#include <cstring>
#include <random>

#include "hyperlink/common.h"

namespace hl {

struct HostCore::Session {
    explicit Session(net::Socket s) : conn(std::move(s)) {}
    net::MessageConn conn;
    net::Addr peer;
    uint32_t id = 0;
    std::string name;
    std::mutex vm;
    net::Addr videoAddr;
    bool haveVideo = false;
    bool streaming = false;  // guarded by HostCore::m_
    StartStream req;
    Packetizer packetizer;
};

HostCore::HostCore(Config cfg, Callbacks cb) : cfg_(std::move(cfg)), cb_(std::move(cb)) {}

HostCore::~HostCore() { stop(); }

void HostCore::setPin(const std::string& pin) {
    std::lock_guard<std::mutex> l(cfgMutex_);
    cfg_.pin = pin;
}

void HostCore::setName(const std::string& name) {
    std::lock_guard<std::mutex> l(cfgMutex_);
    cfg_.name = name;
}

bool HostCore::start() {
    net::init();
    listen_ = net::Socket::listenTcp(cfg_.controlPort);
    video_ = net::Socket::udp(cfg_.videoPort);
    discovery_ = net::Socket::udp(cfg_.discoveryPort);
    if (!listen_.valid() || !video_.valid()) {
        log("hostcore: cannot open ports %u/%u", cfg_.controlPort, cfg_.videoPort);
        return false;
    }
    video_.setBuffers(4 * 1024 * 1024);
    nextId_ = std::random_device()() | 1;
    running_ = true;
    threads_.emplace_back([this] { acceptLoop(); });
    threads_.emplace_back([this] { videoLoop(); });
    if (discovery_.valid()) threads_.emplace_back([this] { discoveryLoop(); });
    log("hostcore: sharing on TCP %u", cfg_.controlPort);
    return true;
}

void HostCore::stop() {
    if (!running_.exchange(false)) return;
    listen_.shutdown();
    listen_.close();
    video_.close();
    discovery_.close();
    std::vector<std::shared_ptr<Session>> ss;
    {
        std::lock_guard<std::mutex> l(m_);
        ss = sessions_;
    }
    for (auto& s : ss) s->conn.shutdown();
    for (auto& t : threads_)
        if (t.joinable()) t.join();
    threads_.clear();
    // Session threads are detached; wait until they have all left.
    for (int i = 0; i < 100; i++) {
        {
            std::lock_guard<std::mutex> l(m_);
            if (sessions_.empty()) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

int HostCore::clientCount() {
    std::lock_guard<std::mutex> l(m_);
    int n = 0;
    for (auto& s : sessions_) n += !s->name.empty();
    return n;
}

int HostCore::streamCount() {
    std::lock_guard<std::mutex> l(m_);
    int n = 0;
    for (auto& s : sessions_) n += s->streaming;
    return n;
}

void HostCore::acceptLoop() {
    while (running_) {
        net::Addr peer;
        net::Socket c = listen_.accept(&peer);
        if (!c.valid()) {
            if (!running_) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        auto s = std::make_shared<Session>(std::move(c));
        s->peer = peer;
        {
            std::lock_guard<std::mutex> l(m_);
            s->id = nextId_++;
            sessions_.push_back(s);
        }
        std::thread([this, s] { runSession(s); }).detach();
    }
}

void HostCore::runSession(std::shared_ptr<Session> s) {
    std::vector<uint8_t> body;
    bool ok = false;
    if (s->conn.read(body) && !body.empty() && body[0] == MSG_HELLO) {
        Reader r(body.data() + 1, body.size() - 1);
        Hello hello;
        Config cfg;
        {
            std::lock_guard<std::mutex> l(cfgMutex_);
            cfg = cfg_;
        }
        if (hello.decode(r)) {
            if (!cfg.pin.empty() && hello.pin != cfg.pin) {
                Writer w(MSG_AUTH_FAILED);
                w.str(hello.pin.empty() ? "This device needs a PIN" : "Wrong PIN");
                s->conn.send(w.done());
            } else {
                Welcome w;
                w.hostName = cfg.name;
                w.hostId = cfg.hostId;
                w.hostVersion = cfg.version;
                w.sessionId = s->id;
                w.videoPort = cfg.videoPort;
                w.codecMask = cfg.codecMask;
                if (cb_.monitors) w.monitors = cb_.monitors();
                w.remoteAddresses = net::tailscaleAddresses();
                {
                    std::lock_guard<std::mutex> l(m_);
                    s->name = hello.clientName.empty() ? s->peer.ip() : hello.clientName;
                }
                ok = s->conn.send(w.encode());
                log("hostcore: %s connected", s->name.c_str());
            }
        }
    }
    if (ok) {
        if (cb_.statusChanged) cb_.statusChanged();
        while (running_ && s->conn.read(body))
            if (!body.empty()) handle(s, body);
        log("hostcore: %s disconnected", s->name.c_str());
    }
    {
        std::lock_guard<std::mutex> l(m_);
        sessions_.erase(std::remove(sessions_.begin(), sessions_.end(), s), sessions_.end());
    }
    updateEncoder();
    if (cb_.statusChanged) cb_.statusChanged();
}

void HostCore::handle(const std::shared_ptr<Session>& s, const std::vector<uint8_t>& body) {
    Reader r(body.data() + 1, body.size() - 1);
    switch (body[0]) {
        case MSG_START_STREAM: {
            StartStream req;
            if (!req.decode(r)) return;
            bool alreadyRunning;
            {
                std::lock_guard<std::mutex> l(m_);
                s->req = req;
                s->streaming = true;
                s->packetizer.setFecPercent(req.fecPercent);
                s->packetizer.setStreamId(req.streamId);
                alreadyRunning = encoderRunning_ && currentValid_;
                if (alreadyRunning) {
                    // Join the running encoder: new epoch for this client, then a keyframe.
                    StreamStarted st = current_;
                    st.streamId = req.streamId;
                    st.epoch = ++epoch_;
                    s->packetizer.setEpoch(st.epoch);
                    s->conn.send(st.encode());
                }
            }
            if (alreadyRunning) {
                if (cb_.requestKeyframe) cb_.requestKeyframe();
            } else {
                // First viewer (or a new size): (re)start the encoder with this request.
                {
                    std::lock_guard<std::mutex> l(m_);
                    encoderRunning_ = true;
                    currentValid_ = false;
                }
                if (cb_.startEncoder) cb_.startEncoder(req);
            }
            if (cb_.statusChanged) cb_.statusChanged();
            break;
        }
        case MSG_STOP_STREAM:
            {
                std::lock_guard<std::mutex> l(m_);
                s->streaming = false;
            }
            updateEncoder();
            if (cb_.statusChanged) cb_.statusChanged();
            break;
        case MSG_REQUEST_IDR:
            if (cb_.requestKeyframe) cb_.requestKeyframe();
            break;
        case MSG_PING: {
            uint64_t t = r.u64();
            Writer w(MSG_PONG);
            w.u64(t);
            s->conn.send(w.done());
            break;
        }
        case MSG_MOUSE_ABS: case MSG_MOUSE_REL: case MSG_MOUSE_BUTTON: case MSG_MOUSE_SCROLL:
        case MSG_KEY: case MSG_TEXT:
            if (cb_.input) cb_.input(body);
            break;
        default:
            break;
    }
}

void HostCore::updateEncoder() {
    bool anyone = false, wasRunning;
    {
        std::lock_guard<std::mutex> l(m_);
        for (auto& s : sessions_) anyone |= s->streaming;
        wasRunning = encoderRunning_;
        if (!anyone) {
            encoderRunning_ = false;
            currentValid_ = false;
        }
    }
    if (!anyone && wasRunning && cb_.stopEncoder) cb_.stopEncoder();
}

void HostCore::streamReady(uint16_t width, uint16_t height, uint16_t fps, uint8_t codec,
                           const std::string& encoderName) {
    std::lock_guard<std::mutex> l(m_);
    current_ = StreamStarted();
    current_.monitorId = 1;
    current_.width = width;
    current_.height = height;
    current_.fps = fps;
    current_.codec = codec;
    current_.encoderName = encoderName;
    currentValid_ = true;
    frameIndex_ = 0;
    for (auto& s : sessions_) {
        if (!s->streaming) continue;
        StreamStarted st = current_;
        st.streamId = s->req.streamId;
        st.monitorId = s->req.monitorId;
        st.epoch = ++epoch_;
        s->packetizer.setEpoch(st.epoch);
        s->conn.send(st.encode());
    }
}

void HostCore::sendFrame(const uint8_t* data, size_t size, bool keyframe, uint64_t captureUs) {
    std::lock_guard<std::mutex> l(m_);
    if (!currentValid_) return;
    EncodedFrame f;
    f.data = data;
    f.size = size;
    f.keyframe = keyframe;
    f.frameIndex = frameIndex_++;
    f.captureUs = (uint32_t)captureUs;
    f.hostUs = (uint32_t)(nowUs() - captureUs);
    for (auto& s : sessions_) {
        if (!s->streaming) continue;
        net::Addr to;
        {
            std::lock_guard<std::mutex> vl(s->vm);
            if (!s->haveVideo) continue;
            to = s->videoAddr;
        }
        s->packetizer.packetize(f, [&](const uint8_t* p, size_t n) { video_.sendTo(to, p, n); });
    }
}

void HostCore::monitorsChanged() {
    if (!cb_.monitors) return;
    Monitors m;
    m.monitors = cb_.monitors();
    auto msg = m.encode();
    std::lock_guard<std::mutex> l(m_);
    for (auto& s : sessions_)
        if (!s->name.empty()) s->conn.send(msg);
}

void HostCore::videoLoop() {
    uint8_t buf[256];
    while (running_) {
        net::Addr from;
        int n = video_.recvFrom(buf, sizeof buf, &from, 300);
        if (n < 8 || buf[0] != kPacketMagic || buf[2] != PKT_PUNCH) continue;
        uint32_t id = get32(buf + 4);
        std::lock_guard<std::mutex> l(m_);
        for (auto& s : sessions_)
            if (s->id == id) {
                std::lock_guard<std::mutex> vl(s->vm);
                s->videoAddr = from;
                s->haveVideo = true;
            }
    }
}

void HostCore::discoveryLoop() {
    uint8_t buf[512];
    while (running_) {
        net::Addr from;
        int n = discovery_.recvFrom(buf, sizeof buf, &from, 300);
        if (n < (int)kDiscoverTagLen || std::memcmp(buf, kDiscoverQuery, kDiscoverTagLen) != 0) continue;
        DiscoveryReply d;
        {
            std::lock_guard<std::mutex> l(cfgMutex_);
            d.hostId = cfg_.hostId;
            d.hostName = cfg_.name;
            d.version = cfg_.version;
            d.controlPort = cfg_.controlPort;
            d.pinRequired = cfg_.pin.empty() ? 0 : 1;
        }
        d.clients = (uint16_t)clientCount();
        d.streams = (uint16_t)streamCount();
        d.remoteAddresses = net::tailscaleAddresses();
        auto reply = d.encode();
        discovery_.sendTo(from, reply.data(), reply.size());
    }
}

}  // namespace hl
