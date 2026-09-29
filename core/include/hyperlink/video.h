// Video transport: an encoded frame is cut into fixed-size shards, grouped into FEC
// blocks, and each shard travels in one UDP datagram.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <vector>

namespace hl {

constexpr uint8_t kPacketMagic = 0x48;  // 'H'
constexpr uint8_t kPacketVersion = 1;
constexpr size_t kShardSize = 1200;     // header + shard stays well under a 1500 byte MTU
constexpr size_t kHeaderSize = 32;
constexpr size_t kMaxDatagram = kHeaderSize + kShardSize;
constexpr int kMaxDataShards = 128;     // per FEC block

enum PacketType : uint8_t {
    PKT_VIDEO = 0,
    PKT_PUNCH = 1,  // client -> host: "send video here", carries the session id
};

enum PacketFlags : uint8_t {
    PF_KEYFRAME = 1,
    PF_PARITY = 2,
};

struct ShardHeader {
    uint8_t type = PKT_VIDEO;
    uint8_t flags = 0;
    uint8_t streamId = 0;     // one stream per monitor inside a session
    uint16_t epoch = 0;       // bumps every time a stream (re)starts; stale packets are dropped
    uint32_t frameIndex = 0;
    uint32_t frameSize = 0;   // bytes of encoded data in the whole frame
    uint16_t blockIndex = 0;
    uint16_t blockCount = 0;
    uint8_t shardIndex = 0;   // 0..data+parity-1 inside the block
    uint8_t dataShards = 0;
    uint8_t parityShards = 0;
    uint32_t hostUs = 0;      // capture -> first packet sent, measured on the host
    uint32_t captureUs = 0;   // host clock (low 32 bits, microseconds) when the frame was captured

    void write(uint8_t* p) const;
    bool read(const uint8_t* p, size_t n);
};

struct EncodedFrame {
    const uint8_t* data = nullptr;
    size_t size = 0;
    bool keyframe = false;
    uint32_t frameIndex = 0;
    uint32_t hostUs = 0;
    uint32_t captureUs = 0;
};

// Returns the stream id of a video datagram, or -1 if it is not one.
int peekStreamId(const uint8_t* p, size_t n);

// Host side. Calls send(datagram, length) for every datagram of the frame.
class Packetizer {
public:
    explicit Packetizer(int fecPercent = 20) : fecPercent_(fecPercent) {}
    void setFecPercent(int p) { fecPercent_ = p; }
    void setEpoch(uint16_t e) { epoch_ = e; }
    void setStreamId(uint8_t id) { streamId_ = id; }
    void packetize(const EncodedFrame& f, const std::function<void(const uint8_t*, size_t)>& send);

private:
    int fecPercent_;
    uint16_t epoch_ = 0;
    uint8_t streamId_ = 0;
    std::vector<uint8_t> shards_;
    std::vector<uint8_t> datagram_ = std::vector<uint8_t>(kMaxDatagram);
};

struct CompleteFrame {
    std::vector<uint8_t> data;
    bool keyframe = false;
    uint32_t frameIndex = 0;
    uint32_t hostUs = 0;
    uint32_t captureUs = 0;
    uint64_t firstPacketUs = 0;  // client clock when the first shard arrived
    uint64_t completeUs = 0;     // client clock when the frame became decodable
    int recoveredShards = 0;
};

// Client side. Feed every datagram in; complete frames come out in order.
// Frames that can no longer be completed are reported through onLoss.
class Reassembler {
public:
    std::function<void(CompleteFrame&&)> onFrame;
    std::function<void(uint32_t firstLost, uint32_t count)> onLoss;

    void setEpoch(uint16_t e);
    void push(const uint8_t* p, size_t n, uint64_t nowUs);

    uint64_t shardsReceived = 0;
    uint64_t shardsRecovered = 0;
    uint64_t framesLost = 0;

private:
    struct Block {
        int k = 0, m = 0;
        std::vector<uint8_t> buf;       // (k + m) * kShardSize
        std::vector<bool> present;
        int count = 0;
        bool done = false;
    };
    struct Pending {
        ShardHeader h;
        std::vector<Block> blocks;
        int blocksDone = 0;
        uint64_t firstPacketUs = 0;
        int recovered = 0;
    };

    void finish(uint32_t index, Pending& p, uint64_t nowUs);
    void dropOlderThan(uint32_t index);

    uint16_t epoch_ = 0;
    bool haveNext_ = false;
    uint32_t next_ = 0;  // next frame index we expect to deliver
    std::map<uint32_t, Pending> pending_;
};

}  // namespace hl
