#include "decoder.h"

#include <media/NdkMediaFormat.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <vector>

#include "hyperlink/common.h"
#include "hyperlink/protocol.h"

namespace {

const char* mimeFor(uint8_t codec) {
    switch (codec) {
        case hl::CODEC_H264: return "video/avc";
        case hl::CODEC_AV1: return "video/av01";
        default: return "video/hevc";
    }
}

// Splits an Annex-B stream into NAL units: calls fn(start, size, nalType) for each.
template <typename F>
void forEachNal(const uint8_t* p, size_t n, uint8_t codec, F fn) {
    size_t i = 0;
    auto startCode = [&](size_t at, size_t& len) {
        if (at + 3 <= n && p[at] == 0 && p[at + 1] == 0 && p[at + 2] == 1) { len = 3; return true; }
        if (at + 4 <= n && p[at] == 0 && p[at + 1] == 0 && p[at + 2] == 0 && p[at + 3] == 1) { len = 4; return true; }
        return false;
    };
    size_t sc = 0;
    while (i < n && !startCode(i, sc)) i++;
    while (i < n) {
        size_t begin = i;
        size_t payload = i + sc;
        size_t j = payload;
        size_t nextSc = 0;
        while (j < n && !startCode(j, nextSc)) j++;
        if (payload < n) {
            int type = codec == hl::CODEC_H264 ? (p[payload] & 0x1f) : ((p[payload] >> 1) & 0x3f);
            fn(p + begin, j - begin, type);
        }
        i = j;
        sc = nextSc;
    }
}

bool isParamSet(uint8_t codec, int type) {
    if (codec == hl::CODEC_H264) return type == 7 || type == 8;
    if (codec == hl::CODEC_HEVC) return type == 32 || type == 33 || type == 34;
    return false;
}

}  // namespace

bool Decoder::start(ANativeWindow* window, uint8_t codec, int width, int height, int fps,
                    const uint8_t* keyframe, size_t size) {
    stop();
    const char* mime = mimeFor(codec);
    codec_ = AMediaCodec_createDecoderByType(mime);
    if (!codec_) {
        hl::log("decoder: no decoder for %s", mime);
        return false;
    }
    AMediaFormat* fmt = AMediaFormat_new();
    AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME, mime);
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, width);
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, height);
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_MAX_INPUT_SIZE, std::max(width * height, 1 << 20));
    AMediaFormat_setInt32(fmt, "frame-rate", fps);
    AMediaFormat_setInt32(fmt, "priority", 0);  // realtime
    // Low-latency switches: the standard one (Android 11+) and the vendor ones older
    // Qualcomm / Exynos / Kirin / MediaTek decoders understand. Unknown keys are ignored.
    AMediaFormat_setInt32(fmt, "low-latency", 1);
    AMediaFormat_setInt32(fmt, "vendor.qti-ext-dec-low-latency.enable", 1);
    AMediaFormat_setInt32(fmt, "vendor.qti-ext-dec-picture-order.enable", 1);
    AMediaFormat_setInt32(fmt, "vendor.rtc-ext-dec-low-latency.enable", 1);
    AMediaFormat_setInt32(fmt, "vendor.hisi-ext-low-latency-video-dec.video-scene-for-low-latency-req", 1);
    AMediaFormat_setInt32(fmt, "vendor.hisi-ext-low-latency-video-dec.video-scene-for-low-latency-rdy", -1);
    AMediaFormat_setInt32(fmt, "vendor.low-latency.enable", 1);

    // Parameter sets as codec-specific data; some decoders refuse in-band ones on the first frame.
    std::vector<uint8_t> csd;
    std::vector<uint8_t> sps, pps;
    forEachNal(keyframe, size, codec, [&](const uint8_t* nal, size_t len, int type) {
        if (!isParamSet(codec, type)) return;
        if (codec == hl::CODEC_H264) (type == 7 ? sps : pps).insert((type == 7 ? sps : pps).end(), nal, nal + len);
        else csd.insert(csd.end(), nal, nal + len);
    });
    if (codec == hl::CODEC_H264) {
        if (!sps.empty()) AMediaFormat_setBuffer(fmt, "csd-0", sps.data(), sps.size());
        if (!pps.empty()) AMediaFormat_setBuffer(fmt, "csd-1", pps.data(), pps.size());
    } else if (!csd.empty()) {
        AMediaFormat_setBuffer(fmt, "csd-0", csd.data(), csd.size());
    }

    media_status_t st = AMediaCodec_configure(codec_, fmt, window, nullptr, 0);
    AMediaFormat_delete(fmt);
    if (st != AMEDIA_OK || AMediaCodec_start(codec_) != AMEDIA_OK) {
        hl::log("decoder: configure/start failed (%d)", (int)st);
        AMediaCodec_delete(codec_);
        codec_ = nullptr;
        return false;
    }
    char* name = nullptr;
    if (AMediaCodec_getName(codec_, &name) == AMEDIA_OK && name) {
        hl::log("decoder: %s %dx%d", name, width, height);
        AMediaCodec_releaseName(codec_, name);
    }
    stop_ = false;
    failed_ = false;
    output_ = std::thread([this] { outputLoop(); });
    return true;
}

bool Decoder::submit(const uint8_t* data, size_t size, bool keyframe) {
    if (!codec_ || failed_) return false;
    ssize_t idx = AMediaCodec_dequeueInputBuffer(codec_, 8000);
    if (idx < 0) {
        std::lock_guard<std::mutex> lock(statsMutex_);
        stats_.dropped++;
        return idx == AMEDIACODEC_INFO_TRY_AGAIN_LATER;  // busy: drop the frame, keep going
    }
    size_t cap = 0;
    uint8_t* buf = AMediaCodec_getInputBuffer(codec_, idx, &cap);
    if (!buf || cap < size) {
        AMediaCodec_queueInputBuffer(codec_, idx, 0, 0, 0, 0);
        return false;
    }
    memcpy(buf, data, size);
    uint32_t flags = keyframe ? 1 /* BUFFER_FLAG_KEY_FRAME */ : 0;
    return AMediaCodec_queueInputBuffer(codec_, idx, 0, size, hl::nowUs(), flags) == AMEDIA_OK;
}

void Decoder::outputLoop() {
    while (!stop_) {
        AMediaCodecBufferInfo info;
        ssize_t idx = AMediaCodec_dequeueOutputBuffer(codec_, &info, 20000);
        if (idx >= 0) {
            uint64_t now = hl::nowUs();
            // Render immediately: the frame goes out on the very next display refresh.
            AMediaCodec_releaseOutputBuffer(codec_, idx, info.size != 0);
            std::lock_guard<std::mutex> lock(statsMutex_);
            stats_.framesDecoded++;
            if (info.presentationTimeUs > 0 && now > (uint64_t)info.presentationTimeUs)
                stats_.decodeUsSum += now - info.presentationTimeUs;
        } else if (idx != AMEDIACODEC_INFO_TRY_AGAIN_LATER &&
                   idx != AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED &&
                   idx != AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
            // Decoder error: the stream restarts it on the next keyframe.
            failed_ = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
}

void Decoder::stop() {
    if (!codec_) return;
    stop_ = true;
    if (output_.joinable()) output_.join();
    AMediaCodec_stop(codec_);
    AMediaCodec_delete(codec_);
    codec_ = nullptr;
}

Decoder::Stats Decoder::takeStats() {
    std::lock_guard<std::mutex> lock(statsMutex_);
    Stats s = stats_;
    stats_ = Stats();
    return s;
}
