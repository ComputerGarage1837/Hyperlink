// Control channel (TCP): session setup, stream control, input, cursor.
// Framing: u32 payload length, then u8 message type, then the fields, all little-endian.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace hl {

constexpr uint32_t kProtocolVersion = 1;
constexpr uint16_t kControlPort = 47800;
constexpr uint16_t kVideoPort = 47801;
constexpr uint16_t kDiscoveryPort = 47802;
constexpr uint32_t kMaxMessage = 8 * 1024 * 1024;

// Discovery and presence: a client sends kDiscoverQuery to kDiscoveryPort, as a LAN broadcast
// to find new hosts or straight to a saved host's address to see whether it is online. Each
// host answers with kDiscoverReply followed by an encoded DiscoveryReply.
constexpr char kDiscoverQuery[] = "HYPERLINK?1";
constexpr char kDiscoverReply[] = "HYPERLINK!1";
constexpr size_t kDiscoverTagLen = sizeof(kDiscoverQuery) - 1;

enum Codec : uint8_t { CODEC_H264 = 0, CODEC_HEVC = 1, CODEC_AV1 = 2 };
inline const char* codecName(uint8_t c) {
    return c == CODEC_H264 ? "H.264" : c == CODEC_HEVC ? "HEVC" : c == CODEC_AV1 ? "AV1" : "?";
}

enum MsgType : uint8_t {
    // client -> host
    MSG_HELLO = 1,
    MSG_START_STREAM = 2,
    MSG_STOP_STREAM = 3,
    MSG_REQUEST_IDR = 4,
    MSG_PING = 5,
    MSG_MOUSE_ABS = 10,
    MSG_MOUSE_REL = 11,
    MSG_MOUSE_BUTTON = 12,
    MSG_MOUSE_SCROLL = 13,
    MSG_KEY = 14,
    MSG_TEXT = 15,
    MSG_CLIENT_STATS = 16,
    // host -> client
    MSG_WELCOME = 64,
    MSG_STREAM_STARTED = 65,
    MSG_STREAM_ERROR = 66,
    MSG_PONG = 67,
    MSG_CURSOR_SHAPE = 68,
    MSG_CURSOR_POS = 69,
    MSG_HOST_STATS = 70,
    MSG_AUTH_FAILED = 71,
    MSG_MONITORS = 72,  // monitor layout changed
};

enum MouseButton : uint8_t { MOUSE_LEFT = 1, MOUSE_RIGHT = 2, MOUSE_MIDDLE = 3, MOUSE_X1 = 4, MOUSE_X2 = 5 };

struct Writer {
    std::vector<uint8_t> buf;
    explicit Writer(uint8_t type) { buf = {0, 0, 0, 0, type}; }
    Writer& u8(uint8_t v) { buf.push_back(v); return *this; }
    Writer& u16(uint16_t v) { return le(v, 2); }
    Writer& u32(uint32_t v) { return le(v, 4); }
    Writer& u64(uint64_t v) { return le(v, 8); }
    Writer& i16(int16_t v) { return u16((uint16_t)v); }
    Writer& i32(int32_t v) { return u32((uint32_t)v); }
    Writer& str(const std::string& s);
    Writer& blob(const std::vector<uint8_t>& b);
    // Fills in the length prefix and returns the finished message.
    const std::vector<uint8_t>& done();

private:
    Writer& le(uint64_t v, int bytes) {
        for (int i = 0; i < bytes; i++) buf.push_back((uint8_t)(v >> (8 * i)));
        return *this;
    }
};

struct Reader {
    const uint8_t* p;
    size_t n;
    bool ok = true;
    Reader(const uint8_t* data, size_t size) : p(data), n(size) {}
    uint8_t u8();
    uint16_t u16();
    uint32_t u32();
    uint64_t u64();
    int16_t i16() { return (int16_t)u16(); }
    int32_t i32() { return (int32_t)u32(); }
    std::string str();
    std::vector<uint8_t> blob();

private:
    bool need(size_t k);
};

struct MonitorInfo {
    uint32_t id = 0;
    std::string name;
    int32_t x = 0, y = 0;       // position on the host's virtual desktop
    uint32_t width = 0, height = 0;
    uint16_t refreshHz = 60;
    uint8_t primary = 0;

    void write(Writer& w) const;
    void read(Reader& r);
};

struct DiscoveryReply {
    std::string hostId;        // stable per host install
    std::string hostName;
    std::string version;
    uint16_t controlPort = kControlPort;
    uint8_t pinRequired = 0;
    uint16_t clients = 0;      // clients connected right now
    uint16_t streams = 0;      // monitors being streamed right now
    // Addresses that reach this host from outside the local network and don't change
    // (its Tailscale addresses). Optional on the wire: older hosts don't send them.
    std::vector<std::string> remoteAddresses;

    std::vector<uint8_t> encode() const;  // includes kDiscoverReply
    bool decode(const uint8_t* p, size_t n);
};

struct Hello {
    uint32_t version = kProtocolVersion;
    std::string clientName;
    std::string clientId;
    std::string pin;
    uint16_t displayWidth = 0, displayHeight = 0, displayHz = 60;
    uint32_t codecMask = 0;  // bit per Codec the client can decode

    std::vector<uint8_t> encode() const;
    bool decode(Reader& r);
};

struct Welcome {
    uint32_t version = kProtocolVersion;
    std::string hostName;
    std::string hostId;
    std::string hostVersion;
    uint32_t sessionId = 0;
    uint16_t videoPort = kVideoPort;
    uint32_t codecMask = 0;  // what the host can encode
    std::vector<MonitorInfo> monitors;
    std::vector<std::string> remoteAddresses;  // see DiscoveryReply

    std::vector<uint8_t> encode() const;
    bool decode(Reader& r);
};

struct Monitors {
    std::vector<MonitorInfo> monitors;
    std::vector<uint8_t> encode() const;
    bool decode(Reader& r);
};

// A session can run several streams at once (one per monitor); the client picks the id.
struct StartStream {
    uint8_t streamId = 0;
    uint32_t monitorId = 0;
    uint16_t maxWidth = 0, maxHeight = 0;  // host keeps the monitor's aspect ratio inside this box
    uint16_t fps = 120;
    uint32_t bitrateKbps = 50000;
    uint8_t codec = CODEC_HEVC;
    uint8_t fecPercent = 20;

    std::vector<uint8_t> encode() const;
    bool decode(Reader& r);
};

struct StreamStarted {
    uint8_t streamId = 0;
    uint32_t monitorId = 0;
    uint16_t width = 0, height = 0, fps = 0;
    uint8_t codec = CODEC_HEVC;
    uint16_t epoch = 0;
    std::string encoderName;

    std::vector<uint8_t> encode() const;
    bool decode(Reader& r);
};

// Simple messages, laid out as:
//   STOP_STREAM, REQUEST_IDR: u8 streamId
//   STREAM_ERROR: u8 streamId, str message        AUTH_FAILED: str message
//   PING / PONG: u64 client time
//   MOUSE_ABS: u32 monitorId, u16 x, u16 y  (0..65535 across that monitor)
//   MOUSE_REL: i16 dx, i16 dy     MOUSE_BUTTON: u8 button, u8 down
//   MOUSE_SCROLL: i16 dy, i16 dx  (120 = one notch)
//   KEY: u16 windows virtual key, u8 down      TEXT: str utf8
//   CURSOR_POS: u32 monitorId, u16 x, u16 y, u8 visible

struct CursorShape {
    uint16_t width = 0, height = 0, hotX = 0, hotY = 0;
    std::vector<uint8_t> rgba;  // straight alpha, row-major

    std::vector<uint8_t> encode() const;
    bool decode(Reader& r);
};

struct HostStats {
    uint8_t streamId = 0;
    uint16_t captureFps = 0;    // frames captured in the last second
    uint16_t encodeUsAvg = 0;   // average encode time
    uint32_t bitrateKbps = 0;   // measured over the last second

    std::vector<uint8_t> encode() const;
    bool decode(Reader& r);
};

}  // namespace hl
