#include "hyperlink/video.h"

#include <algorithm>
#include <cstring>

#include "hyperlink/common.h"
#include "hyperlink/fec.h"

namespace hl {

void ShardHeader::write(uint8_t* p) const {
    std::memset(p, 0, kHeaderSize);
    p[0] = kPacketMagic;
    p[1] = kPacketVersion;
    p[2] = type;
    p[3] = flags;
    put16(p + 4, epoch);
    put16(p + 6, blockIndex);
    put32(p + 8, frameIndex);
    put32(p + 12, frameSize);
    put16(p + 16, blockCount);
    p[18] = shardIndex;
    p[19] = dataShards;
    p[20] = parityShards;
    p[21] = streamId;
    put32(p + 24, hostUs);
    put32(p + 28, captureUs);
}

bool ShardHeader::read(const uint8_t* p, size_t n) {
    if (n < kHeaderSize || p[0] != kPacketMagic || p[1] != kPacketVersion) return false;
    type = p[2];
    flags = p[3];
    epoch = get16(p + 4);
    blockIndex = get16(p + 6);
    frameIndex = get32(p + 8);
    frameSize = get32(p + 12);
    blockCount = get16(p + 16);
    shardIndex = p[18];
    dataShards = p[19];
    parityShards = p[20];
    streamId = p[21];
    hostUs = get32(p + 24);
    captureUs = get32(p + 28);
    if (type == PKT_VIDEO) {
        if (dataShards == 0 || dataShards > kMaxDataShards) return false;
        if (shardIndex >= dataShards + parityShards) return false;
        if (blockCount == 0 || blockIndex >= blockCount) return false;
        uint64_t maxBytes = (uint64_t)blockCount * kMaxDataShards * kShardSize;
        if (frameSize == 0 || frameSize > maxBytes) return false;
    }
    return true;
}

int peekStreamId(const uint8_t* p, size_t n) {
    if (n < kHeaderSize || p[0] != kPacketMagic || p[1] != kPacketVersion || p[2] != PKT_VIDEO)
        return -1;
    return p[21];
}

void Packetizer::packetize(const EncodedFrame& f,
                           const std::function<void(const uint8_t*, size_t)>& send) {
    if (!f.size) return;
    size_t totalData = (f.size + kShardSize - 1) / kShardSize;
    size_t blockCount = (totalData + kMaxDataShards - 1) / kMaxDataShards;

    ShardHeader h;
    h.type = PKT_VIDEO;
    h.flags = f.keyframe ? PF_KEYFRAME : 0;
    h.epoch = epoch_;
    h.streamId = streamId_;
    h.frameIndex = f.frameIndex;
    h.frameSize = (uint32_t)f.size;
    h.blockCount = (uint16_t)blockCount;
    h.hostUs = f.hostUs;
    h.captureUs = f.captureUs;

    for (size_t b = 0; b < blockCount; b++) {
        size_t first = b * kMaxDataShards;
        int k = (int)std::min<size_t>(kMaxDataShards, totalData - first);
        int m = fecPercent_ <= 0 ? 0 : std::max(1, (k * fecPercent_ + 99) / 100);
        m = std::min(m, 255 - k);

        shards_.assign((size_t)(k + m) * kShardSize, 0);
        size_t offset = first * kShardSize;
        size_t bytes = std::min(f.size - offset, (size_t)k * kShardSize);
        std::memcpy(shards_.data(), f.data + offset, bytes);

        std::vector<const uint8_t*> dataPtr(k);
        std::vector<uint8_t*> parityPtr(m);
        for (int i = 0; i < k; i++) dataPtr[i] = shards_.data() + i * kShardSize;
        for (int j = 0; j < m; j++) parityPtr[j] = shards_.data() + (k + j) * kShardSize;
        if (m) fec::encode(dataPtr.data(), k, parityPtr.data(), m, kShardSize);

        h.blockIndex = (uint16_t)b;
        h.dataShards = (uint8_t)k;
        h.parityShards = (uint8_t)m;
        for (int s = 0; s < k + m; s++) {
            h.shardIndex = (uint8_t)s;
            h.flags = (uint8_t)((f.keyframe ? PF_KEYFRAME : 0) | (s >= k ? PF_PARITY : 0));
            h.write(datagram_.data());
            std::memcpy(datagram_.data() + kHeaderSize, shards_.data() + s * kShardSize, kShardSize);
            send(datagram_.data(), kMaxDatagram);
        }
    }
}

void Reassembler::setEpoch(uint16_t e) {
    epoch_ = e;
    haveNext_ = false;
    pending_.clear();
}

static inline bool before(uint32_t a, uint32_t b) { return (int32_t)(a - b) < 0; }

void Reassembler::push(const uint8_t* p, size_t n, uint64_t nowUs) {
    ShardHeader h;
    if (!h.read(p, n) || h.type != PKT_VIDEO || h.epoch != epoch_) return;
    if (n < kHeaderSize + kShardSize) return;
    if (haveNext_ && before(h.frameIndex, next_)) return;  // late, already delivered or given up
    shardsReceived++;

    auto it = pending_.find(h.frameIndex);
    if (it == pending_.end()) {
        if (pending_.size() >= 64) pending_.erase(pending_.begin());
        Pending np;
        np.h = h;
        np.blocks.resize(h.blockCount);
        np.firstPacketUs = nowUs;
        it = pending_.emplace(h.frameIndex, std::move(np)).first;
    }
    Pending& pf = it->second;
    if (h.blockCount != pf.h.blockCount || h.frameSize != pf.h.frameSize) return;

    Block& b = pf.blocks[h.blockIndex];
    if (b.done) return;
    if (b.k == 0) {
        b.k = h.dataShards;
        b.m = h.parityShards;
        b.buf.assign((size_t)(b.k + b.m) * kShardSize, 0);
        b.present.assign(b.k + b.m, false);
    }
    if (h.dataShards != b.k || h.parityShards != b.m || b.present[h.shardIndex]) return;
    std::memcpy(b.buf.data() + (size_t)h.shardIndex * kShardSize, p + kHeaderSize, kShardSize);
    b.present[h.shardIndex] = true;
    b.count++;
    if (b.count < b.k) return;

    int missing = 0;
    for (int i = 0; i < b.k; i++) missing += !b.present[i];
    if (missing) {
        std::vector<uint8_t*> ptrs(b.k + b.m);
        std::vector<uint8_t> pres(b.k + b.m);
        for (int i = 0; i < b.k + b.m; i++) {
            ptrs[i] = b.buf.data() + (size_t)i * kShardSize;
            pres[i] = b.present[i];
        }
        if (!fec::decode(ptrs.data(), pres.data(), b.k, b.m,
                         kShardSize))
            return;
        pf.recovered += missing;
        shardsRecovered += missing;
    }
    b.done = true;
    b.buf.resize((size_t)b.k * kShardSize);  // parity no longer needed
    if (++pf.blocksDone == (int)pf.blocks.size()) {
        uint32_t index = h.frameIndex;
        finish(index, pf, nowUs);
    }
}

void Reassembler::dropOlderThan(uint32_t index) {
    for (auto it = pending_.begin(); it != pending_.end();) {
        if (before(it->first, index)) it = pending_.erase(it);
        else ++it;
    }
}

void Reassembler::finish(uint32_t index, Pending& p, uint64_t nowUs) {
    CompleteFrame f;
    f.keyframe = (p.h.flags & PF_KEYFRAME) != 0;
    f.frameIndex = index;
    f.hostUs = p.h.hostUs;
    f.captureUs = p.h.captureUs;
    f.firstPacketUs = p.firstPacketUs;
    f.completeUs = nowUs;
    f.recoveredShards = p.recovered;
    f.data.reserve(p.h.frameSize);
    for (auto& b : p.blocks) f.data.insert(f.data.end(), b.buf.begin(), b.buf.end());
    f.data.resize(p.h.frameSize);

    if (haveNext_ && before(next_, index)) {
        uint32_t lost = index - next_;
        framesLost += lost;
        if (onLoss) onLoss(next_, lost);
    }
    haveNext_ = true;
    next_ = index + 1;
    pending_.erase(index);  // p is dangling after this line
    dropOlderThan(next_);
    if (onFrame) onFrame(std::move(f));
}

}  // namespace hl
