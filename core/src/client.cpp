#include "hyperlink/client.h"

#include <chrono>

#include "hyperlink/common.h"

namespace hl {

namespace {
constexpr uint64_t kIdrRetryUs = 150000;  // re-ask for a keyframe this often while waiting
}

Client::Client(ClientListener* listener) : listener_(listener) { net::init(); }

Client::~Client() { disconnect(); }

std::string Client::connect(const std::string& host, uint16_t port, const Hello& hello, Welcome& welcome,
                            int timeoutMs) {
    disconnect();
    stopping_ = false;
    net::Addr addr;
    if (!net::Addr::resolve(host, port, addr)) return "Can't find " + host;
    net::Socket s = net::Socket::connectTcp(addr, timeoutMs);
    if (!s.valid()) return "No answer from " + host + " (is Hyperlink Host running?)";
    conn_ = std::make_unique<net::MessageConn>(std::move(s));
    if (!conn_->send(hello.encode())) return "Connection dropped";

    std::vector<uint8_t> body;
    if (!conn_->read(body) || body.empty()) {
        conn_.reset();
        return "Connection dropped during sign-in";
    }
    Reader r(body.data() + 1, body.size() - 1);
    if (body[0] == MSG_AUTH_FAILED) {
        std::string why = r.str();
        conn_.reset();
        return why.empty() ? "Wrong PIN" : why;
    }
    if (body[0] != MSG_WELCOME || !welcome.decode(r)) {
        conn_.reset();
        return "This host speaks a different protocol version; update both apps";
    }
    sessionId_ = welcome.sessionId;
    hostVideo_ = addr;
    hostVideo_.sa.sin_port = htons(welcome.videoPort);

    udp_ = net::Socket::udp(0);
    udp_.setBuffers(8 * 1024 * 1024);
    connected_ = true;
    punch();
    control_ = std::thread([this] { controlLoop(); });
    video_ = std::thread([this] { videoLoop(); });
    timer_ = std::thread([this] { timerLoop(); });
    return "";
}

void Client::disconnect() {
    stopping_ = true;
    if (conn_) conn_->shutdown();
    udp_.shutdown();
    if (control_.joinable() && control_.get_id() != std::this_thread::get_id()) control_.join();
    if (video_.joinable()) video_.join();
    if (timer_.joinable()) timer_.join();
    if (control_.joinable()) control_.detach();
    udp_.close();
    conn_.reset();
    connected_ = false;
    std::lock_guard<std::mutex> lock(streamsMutex_);
    streams_.clear();
}

void Client::punch() {
    uint8_t p[8] = {kPacketMagic, kPacketVersion, PKT_PUNCH, 0};
    put32(p + 4, sessionId_);
    udp_.sendTo(hostVideo_, p, sizeof p);
}

void Client::send(const std::vector<uint8_t>& msg) {
    if (conn_ && connected_) conn_->send(msg);
}

void Client::startStream(const StartStream& req, std::shared_ptr<FrameSink> sink) {
    {
        std::lock_guard<std::mutex> lock(streamsMutex_);
        auto s = std::make_unique<Stream>();
        s->req = req;
        s->sink = std::move(sink);
        s->ra.setEpoch(0xFFFF);  // nothing matches until StreamStarted gives the real epoch
        s->statStart = nowUs();
        streams_[req.streamId] = std::move(s);
    }
    punch();
    send(req.encode());
}

void Client::stopStream(uint8_t streamId) {
    {
        std::lock_guard<std::mutex> lock(streamsMutex_);
        streams_.erase(streamId);
    }
    Writer w(MSG_STOP_STREAM);
    w.u8(streamId);
    send(w.done());
}

void Client::setSink(uint8_t streamId, std::shared_ptr<FrameSink> sink) {
    std::lock_guard<std::mutex> lock(streamsMutex_);
    auto it = streams_.find(streamId);
    if (it == streams_.end()) return;
    it->second->sink = std::move(sink);
    needKeyframe(streamId, *it->second, true);
}

void Client::requestKeyframe(uint8_t streamId) {
    std::lock_guard<std::mutex> lock(streamsMutex_);
    auto it = streams_.find(streamId);
    if (it != streams_.end()) needKeyframe(streamId, *it->second, true);
}

// Caller holds streamsMutex_.
void Client::needKeyframe(uint8_t id, Stream& s, bool force) {
    s.waitingKey = true;
    uint64_t now = nowUs();
    if (!force && now - s.lastIdrRequest < kIdrRetryUs) return;
    s.lastIdrRequest = now;
    Writer w(MSG_REQUEST_IDR);
    w.u8(id);
    send(w.done());
}

void Client::mouseAbs(uint32_t monitorId, uint16_t x, uint16_t y) {
    Writer w(MSG_MOUSE_ABS);
    w.u32(monitorId).u16(x).u16(y);
    send(w.done());
}
void Client::mouseRel(int dx, int dy) {
    Writer w(MSG_MOUSE_REL);
    w.i16((int16_t)dx).i16((int16_t)dy);
    send(w.done());
}
void Client::mouseButton(uint8_t button, bool down) {
    Writer w(MSG_MOUSE_BUTTON);
    w.u8(button).u8(down ? 1 : 0);
    send(w.done());
}
void Client::scroll(int dy, int dx) {
    Writer w(MSG_MOUSE_SCROLL);
    w.i16((int16_t)dy).i16((int16_t)dx);
    send(w.done());
}
void Client::key(uint16_t vk, bool down) {
    Writer w(MSG_KEY);
    w.u16(vk).u8(down ? 1 : 0);
    send(w.done());
}
void Client::text(const std::string& utf8) {
    Writer w(MSG_TEXT);
    w.str(utf8);
    send(w.done());
}

void Client::closeAll(const std::string& reason) {
    bool was = connected_.exchange(false);
    if (was && !stopping_ && listener_) listener_->onDisconnected(reason);
}

void Client::controlLoop() {
    std::vector<uint8_t> body;
    while (!stopping_ && conn_->read(body)) {
        if (!body.empty()) handle(body);
    }
    closeAll("The host closed the connection");
}

void Client::handle(const std::vector<uint8_t>& body) {
    Reader r(body.data() + 1, body.size() - 1);
    switch (body[0]) {
        case MSG_STREAM_STARTED: {
            StreamStarted st;
            if (!st.decode(r)) return;
            {
                std::lock_guard<std::mutex> lock(streamsMutex_);
                auto it = streams_.find(st.streamId);
                if (it == streams_.end()) return;
                Stream& s = *it->second;
                s.info = st;
                s.started = true;
                s.ra.setEpoch(st.epoch);
                uint8_t id = st.streamId;
                s.ra.onFrame = [this, id, &s](CompleteFrame&& f) { onFrame(id, s, std::move(f)); };
                s.ra.onLoss = [this, id, &s](uint32_t, uint32_t) { needKeyframe(id, s, false); };
                s.waitingKey = true;
            }
            if (listener_) listener_->onStreamStarted(st);
            break;
        }
        case MSG_STREAM_ERROR: {
            uint8_t id = r.u8();
            std::string msg = r.str();
            if (listener_) listener_->onStreamError(id, msg);
            break;
        }
        case MSG_MONITORS: {
            Monitors m;
            if (m.decode(r) && listener_) listener_->onMonitors(m.monitors);
            break;
        }
        case MSG_PONG: {
            uint64_t t = r.u64();
            if (r.ok) rttUs_ = (uint32_t)(nowUs() - t);
            break;
        }
        case MSG_CURSOR_SHAPE: {
            CursorShape c;
            if (c.decode(r) && listener_) listener_->onCursorShape(c);
            break;
        }
        case MSG_CURSOR_POS: {
            uint32_t mon = r.u32();
            uint16_t x = r.u16(), y = r.u16();
            uint8_t vis = r.u8();
            if (r.ok && listener_) listener_->onCursorPos(mon, x, y, vis != 0);
            break;
        }
        case MSG_HOST_STATS: {
            HostStats hs;
            if (!hs.decode(r)) return;
            {
                std::lock_guard<std::mutex> lock(streamsMutex_);
                auto it = streams_.find(hs.streamId);
                if (it != streams_.end()) it->second->host = hs;
            }
            if (listener_) listener_->onHostStats(hs);
            break;
        }
        default:
            break;
    }
}

// Called from Reassembler::push with streamsMutex_ held.
void Client::onFrame(uint8_t id, Stream& s, CompleteFrame&& f) {
    s.frames++;
    s.bytes += f.data.size();
    s.hostUsSum += f.hostUs;
    s.assemblyUsSum += f.completeUs - f.firstPacketUs;
    if (s.waitingKey) {
        if (!f.keyframe) {
            needKeyframe(id, s, false);
            return;
        }
        s.waitingKey = false;
    }
    if (!s.sink || !s.sink->onFrame(f, s.info)) needKeyframe(id, s, true);
}

void Client::videoLoop() {
    std::vector<uint8_t> buf(kMaxDatagram + 64);
    while (!stopping_) {
        int n = udp_.recvFrom(buf.data(), buf.size(), nullptr, 200);
        if (n <= 0) {
            if (n < 0 && stopping_) break;
            continue;
        }
        int id = peekStreamId(buf.data(), (size_t)n);
        if (id < 0) continue;
        std::lock_guard<std::mutex> lock(streamsMutex_);
        auto it = streams_.find((uint8_t)id);
        if (it == streams_.end() || !it->second->started) continue;
        it->second->ra.push(buf.data(), (size_t)n, nowUs());
    }
}

void Client::timerLoop() {
    uint64_t last = 0;
    while (!stopping_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        uint64_t now = nowUs();
        if (now - last < 1000000) continue;
        last = now;
        punch();  // keeps NAT/firewall mappings open and tells the host where we are
        Writer w(MSG_PING);
        w.u64(now);
        send(w.done());
        // A stream stuck waiting for a keyframe keeps asking.
        std::lock_guard<std::mutex> lock(streamsMutex_);
        for (auto& [id, s] : streams_)
            if (s->started && s->waitingKey) needKeyframe(id, *s, false);
    }
}

StreamStats Client::takeStats(uint8_t streamId) {
    StreamStats st;
    std::lock_guard<std::mutex> lock(streamsMutex_);
    auto it = streams_.find(streamId);
    if (it == streams_.end()) return st;
    Stream& s = *it->second;
    uint64_t now = nowUs();
    double secs = std::max(0.001, (now - s.statStart) / 1e6);
    st.valid = s.started;
    st.width = s.info.width;
    st.height = s.info.height;
    st.codec = s.info.codec;
    st.encoderName = s.info.encoderName;
    st.fps = (float)(s.frames / secs);
    st.mbps = (float)(s.bytes * 8 / secs / 1e6);
    uint64_t got = s.ra.shardsReceived - s.shardsAtStart;
    uint64_t rec = s.ra.shardsRecovered - s.recoveredAtStart;
    st.recovered = (uint32_t)rec;
    st.lossPercent = got + rec ? (float)(100.0 * rec / (got + rec)) : 0;
    st.framesLost = (uint32_t)s.ra.framesLost;
    st.hostMs = s.frames ? (float)(s.hostUsSum / s.frames / 1000.0) : 0;
    st.assemblyMs = s.frames ? (float)(s.assemblyUsSum / s.frames / 1000.0) : 0;
    st.host = s.host;
    s.statStart = now;
    s.frames = s.bytes = s.hostUsSum = s.assemblyUsSum = 0;
    s.shardsAtStart = s.ra.shardsReceived;
    s.recoveredAtStart = s.ra.shardsRecovered;
    return st;
}

}  // namespace hl
