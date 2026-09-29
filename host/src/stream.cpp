#include "stream.h"

#include <windows.h>
#include <avrt.h>

#include <algorithm>
#include <memory>

#include "capture.h"
#include "encoder.h"
#include "hyperlink/common.h"
#include "hyperlink/video.h"
#include "server.h"

Stream::Stream(Server& server, Session& session, const hl::StartStream& req, const HostMonitor& mon)
    : server_(server), session_(session), req_(req), mon_(mon) {
    thread_ = std::thread([this] { run(); });
}

Stream::~Stream() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
}

// Largest even size with the monitor's aspect ratio that fits in the client's box.
static void fitSize(int srcW, int srcH, int maxW, int maxH, int& w, int& h) {
    w = srcW;
    h = srcH;
    if (maxW > 0 && maxH > 0 && (w > maxW || h > maxH)) {
        double s = std::min((double)maxW / w, (double)maxH / h);
        w = (int)(w * s);
        h = (int)(h * s);
    }
    w = std::max(64, w & ~1);
    h = std::max(64, h & ~1);
}

static void sendError(Session& s, uint8_t streamId, const std::string& msg) {
    hl::log("stream %u: %s", streamId, msg.c_str());
    hl::Writer w(hl::MSG_STREAM_ERROR);
    w.u8(streamId).str(msg);
    s.sendControl(w.done());
}

void Stream::run() {
    DWORD task = 0;
    HANDLE avrt = AvSetMmThreadCharacteristicsW(L"Games", &task);
    auto cap = Capture::get(mon_);
    if (!cap) {
        sendError(session_, req_.streamId, "Cannot capture " + mon_.info.name);
        return;
    }
    auto gpu = cap->gpu();
    const int fps = std::clamp<int>(req_.fps ? req_.fps : 60, 10, 240);
    const uint64_t interval = 1000000 / fps;
    const uint64_t slack = std::min<uint64_t>(1000, interval / 8);

    std::unique_ptr<Encoder> enc;
    hl::Packetizer packetizer(req_.fecPercent);
    packetizer.setStreamId(req_.streamId);
    uint64_t modeSeq = ~0ull, frameSeq = 0, cursorSeq = 0;
    uint64_t lastEncode = 0, dirtySince = 0;
    bool dirty = false, ready = false;
    uint32_t frameIndex = 0;
    int lastCursorX = -1, lastCursorY = -1;
    bool lastCursorVisible = false;

    uint64_t statStart = hl::nowUs(), encodeUsSum = 0, bytes = 0;
    int encodes = 0;

    while (!stop_) {
        int timeoutMs = 100;
        if (dirty) {
            uint64_t since = hl::nowUs() - lastEncode;
            timeoutMs = since + slack >= interval ? 0 : (int)((interval - slack - since) / 1000) + 1;
        }
        if (idr_ && ready) timeoutMs = std::min(timeoutMs, 4);
        auto snap = cap->wait(frameSeq, cursorSeq, timeoutMs);
        uint64_t now = hl::nowUs();

        if (snap.cursorSeq != cursorSeq) {
            cursorSeq = snap.cursorSeq;
            CursorState c = cap->cursor();
            if (c.shapeSeq && !c.rgba.empty()) {
                hl::CursorShape cs;
                cs.width = (uint16_t)c.width;
                cs.height = (uint16_t)c.height;
                cs.hotX = (uint16_t)c.hotX;
                cs.hotY = (uint16_t)c.hotY;
                cs.rgba = c.rgba;
                session_.sendCursorShape(c.shapeSeq, cs.encode());
            }
            if (c.x != lastCursorX || c.y != lastCursorY || c.visible != lastCursorVisible) {
                lastCursorX = c.x;
                lastCursorY = c.y;
                lastCursorVisible = c.visible;
                int w = std::max<int>(1, mon_.info.width - 1), h = std::max<int>(1, mon_.info.height - 1);
                hl::Writer m(hl::MSG_CURSOR_POS);
                m.u32(mon_.info.id)
                    .u16((uint16_t)std::clamp(c.x * 65535 / w, 0, 65535))
                    .u16((uint16_t)std::clamp(c.y * 65535 / h, 0, 65535))
                    .u8(c.visible ? 1 : 0);
                session_.sendControl(m.done());
            }
        }

        if (!snap.texture) continue;  // no desktop image yet

        if (snap.modeSeq != modeSeq) {
            modeSeq = snap.modeSeq;
            bool sideways = snap.rotation == DXGI_MODE_ROTATION_ROTATE90 ||
                            snap.rotation == DXGI_MODE_ROTATION_ROTATE270;
            int deskW = sideways ? snap.height : snap.width;
            int deskH = sideways ? snap.width : snap.height;
            EncoderConfig cfg;
            fitSize(deskW, deskH, req_.maxWidth, req_.maxHeight, cfg.width, cfg.height);
            cfg.srcWidth = snap.width;
            cfg.srcHeight = snap.height;
            cfg.rotation = snap.rotation;
            cfg.fps = fps;
            cfg.bitrateKbps = std::clamp<int>(req_.bitrateKbps, 1000, 500000);
            cfg.codec = req_.codec;
            enc = std::make_unique<Encoder>();
            if (!enc->init(gpu->device.Get(), &gpu->lock, cfg)) {
                sendError(session_, req_.streamId, "No video encoder could be started on this PC");
                break;
            }
            uint16_t epoch = server_.nextEpoch();
            packetizer.setEpoch(epoch);
            frameIndex = 0;
            hl::StreamStarted st;
            st.streamId = req_.streamId;
            st.monitorId = mon_.info.id;
            st.width = (uint16_t)cfg.width;
            st.height = (uint16_t)cfg.height;
            st.fps = (uint16_t)fps;
            st.codec = enc->codec();
            st.epoch = epoch;
            st.encoderName = enc->name();
            session_.sendControl(st.encode());
            idr_ = true;
            ready = true;
            dirty = true;
            dirtySince = now;
            frameSeq = snap.frameSeq;
        }
        if (!ready) continue;

        if (snap.frameSeq != frameSeq) {
            frameSeq = snap.frameSeq;
            if (!dirty) dirtySince = now;
            dirty = true;
        }
        bool wantKey = idr_.load();
        if (!(dirty || wantKey) || now - lastEncode + slack < interval) continue;
        if (!session_.hasVideoAddr()) {  // client hasn't told us where to send yet
            dirty = false;
            idr_ = true;
            continue;
        }

        bool key = idr_.exchange(false);
        uint64_t t0 = hl::nowUs();
        bool ok = enc->encode(snap.texture.Get(), key, [&](const uint8_t* p, size_t n, bool isKey) {
            hl::EncodedFrame f;
            f.data = p;
            f.size = n;
            f.keyframe = isKey;
            f.frameIndex = frameIndex++;
            f.captureUs = (uint32_t)dirtySince;
            f.hostUs = (uint32_t)(hl::nowUs() - dirtySince);
            packetizer.packetize(f, [&](const uint8_t* d, size_t len) { session_.sendVideo(d, len); });
            bytes += n;
        });
        uint64_t t1 = hl::nowUs();
        if (!ok) idr_ = true;
        encodeUsSum += t1 - t0;
        encodes++;
        lastEncode = t0;
        dirty = false;

        if (t1 - statStart >= 1000000) {
            hl::HostStats hs;
            hs.streamId = req_.streamId;
            hs.captureFps = cap->fpsLastSecond();
            hs.encodeUsAvg = (uint16_t)std::min<uint64_t>(65535, encodes ? encodeUsSum / encodes : 0);
            hs.bitrateKbps = (uint32_t)(bytes * 8 * 1000 / std::max<uint64_t>(1, t1 - statStart));
            session_.sendControl(hs.encode());
            statStart = t1;
            encodeUsSum = bytes = 0;
            encodes = 0;
        }
    }
    if (avrt) AvRevertMmThreadCharacteristics(avrt);
}
