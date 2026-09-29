#include "hyperlink/protocol.h"

#include <algorithm>

#include "hyperlink/common.h"

namespace hl {

Writer& Writer::str(const std::string& s) {
    u16((uint16_t)std::min<size_t>(s.size(), 65535));
    buf.insert(buf.end(), s.begin(), s.begin() + std::min<size_t>(s.size(), 65535));
    return *this;
}

Writer& Writer::blob(const std::vector<uint8_t>& b) {
    u32((uint32_t)b.size());
    buf.insert(buf.end(), b.begin(), b.end());
    return *this;
}

const std::vector<uint8_t>& Writer::done() {
    put32(buf.data(), (uint32_t)(buf.size() - 4));
    return buf;
}

bool Reader::need(size_t k) {
    if (!ok || n < k) { ok = false; return false; }
    return true;
}
uint8_t Reader::u8() {
    if (!need(1)) return 0;
    uint8_t v = p[0]; p += 1; n -= 1; return v;
}
uint16_t Reader::u16() {
    if (!need(2)) return 0;
    uint16_t v = get16(p); p += 2; n -= 2; return v;
}
uint32_t Reader::u32() {
    if (!need(4)) return 0;
    uint32_t v = get32(p); p += 4; n -= 4; return v;
}
uint64_t Reader::u64() {
    if (!need(8)) return 0;
    uint64_t v = get64(p); p += 8; n -= 8; return v;
}
std::string Reader::str() {
    uint16_t len = u16();
    if (!need(len)) return {};
    std::string s((const char*)p, len); p += len; n -= len; return s;
}
std::vector<uint8_t> Reader::blob() {
    uint32_t len = u32();
    if (!need(len)) return {};
    std::vector<uint8_t> b(p, p + len); p += len; n -= len; return b;
}

void MonitorInfo::write(Writer& w) const {
    w.u32(id).str(name).i32(x).i32(y).u32(width).u32(height).u16(refreshHz).u8(primary);
}
void MonitorInfo::read(Reader& r) {
    id = r.u32(); name = r.str(); x = r.i32(); y = r.i32();
    width = r.u32(); height = r.u32(); refreshHz = r.u16(); primary = r.u8();
}

std::vector<uint8_t> DiscoveryReply::encode() const {
    Writer w(0);
    w.str(hostId).str(hostName).str(version).u16(controlPort).u8(pinRequired).u16(clients).u16(streams);
    std::vector<uint8_t> out(kDiscoverReply, kDiscoverReply + kDiscoverTagLen);
    out.insert(out.end(), w.buf.begin() + 5, w.buf.end());
    return out;
}
bool DiscoveryReply::decode(const uint8_t* p, size_t n) {
    if (n < kDiscoverTagLen || std::memcmp(p, kDiscoverReply, kDiscoverTagLen) != 0) return false;
    Reader r(p + kDiscoverTagLen, n - kDiscoverTagLen);
    hostId = r.str(); hostName = r.str(); version = r.str(); controlPort = r.u16();
    pinRequired = r.u8(); clients = r.u16(); streams = r.u16();
    return r.ok;
}

std::vector<uint8_t> Hello::encode() const {
    Writer w(MSG_HELLO);
    w.u32(version).str(clientName).str(clientId).str(pin);
    w.u16(displayWidth).u16(displayHeight).u16(displayHz).u32(codecMask);
    return w.done();
}
bool Hello::decode(Reader& r) {
    version = r.u32(); clientName = r.str(); clientId = r.str(); pin = r.str();
    displayWidth = r.u16(); displayHeight = r.u16();
    displayHz = r.u16(); codecMask = r.u32();
    return r.ok;
}

std::vector<uint8_t> Welcome::encode() const {
    Writer w(MSG_WELCOME);
    w.u32(version).str(hostName).str(hostId).str(hostVersion).u32(sessionId).u16(videoPort).u32(codecMask);
    w.u16((uint16_t)monitors.size());
    for (auto& m : monitors) m.write(w);
    return w.done();
}
bool Welcome::decode(Reader& r) {
    version = r.u32(); hostName = r.str(); hostId = r.str(); hostVersion = r.str(); sessionId = r.u32(); videoPort = r.u16();
    codecMask = r.u32();
    uint16_t count = r.u16();
    monitors.clear();
    for (int i = 0; i < count && r.ok; i++) {
        MonitorInfo m;
        m.read(r);
        monitors.push_back(m);
    }
    return r.ok;
}

std::vector<uint8_t> Monitors::encode() const {
    Writer w(MSG_MONITORS);
    w.u16((uint16_t)monitors.size());
    for (auto& m : monitors) m.write(w);
    return w.done();
}
bool Monitors::decode(Reader& r) {
    uint16_t count = r.u16();
    monitors.clear();
    for (int i = 0; i < count && r.ok; i++) {
        MonitorInfo m;
        m.read(r);
        monitors.push_back(m);
    }
    return r.ok;
}

std::vector<uint8_t> StartStream::encode() const {
    Writer w(MSG_START_STREAM);
    w.u8(streamId).u32(monitorId).u16(maxWidth).u16(maxHeight).u16(fps).u32(bitrateKbps).u8(codec).u8(fecPercent);
    return w.done();
}
bool StartStream::decode(Reader& r) {
    streamId = r.u8(); monitorId = r.u32(); maxWidth = r.u16(); maxHeight = r.u16(); fps = r.u16();
    bitrateKbps = r.u32(); codec = r.u8(); fecPercent = r.u8();
    return r.ok;
}

std::vector<uint8_t> StreamStarted::encode() const {
    Writer w(MSG_STREAM_STARTED);
    w.u8(streamId).u32(monitorId).u16(width).u16(height).u16(fps).u8(codec).u16(epoch).str(encoderName);
    return w.done();
}
bool StreamStarted::decode(Reader& r) {
    streamId = r.u8(); monitorId = r.u32(); width = r.u16(); height = r.u16(); fps = r.u16(); codec = r.u8();
    epoch = r.u16(); encoderName = r.str();
    return r.ok;
}

std::vector<uint8_t> CursorShape::encode() const {
    Writer w(MSG_CURSOR_SHAPE);
    w.u16(width).u16(height).u16(hotX).u16(hotY).blob(rgba);
    return w.done();
}
bool CursorShape::decode(Reader& r) {
    width = r.u16(); height = r.u16(); hotX = r.u16(); hotY = r.u16(); rgba = r.blob();
    return r.ok && rgba.size() == (size_t)width * height * 4;
}

std::vector<uint8_t> HostStats::encode() const {
    Writer w(MSG_HOST_STATS);
    w.u8(streamId).u16(captureFps).u16(encodeUsAvg).u32(bitrateKbps);
    return w.done();
}
bool HostStats::decode(Reader& r) {
    streamId = r.u8(); captureFps = r.u16(); encodeUsAvg = r.u16(); bitrateKbps = r.u32();
    return r.ok;
}

}  // namespace hl
