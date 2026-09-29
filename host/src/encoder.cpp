#include "encoder.h"

#include <climits>

#include "hyperlink/common.h"
#include "hyperlink/protocol.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
}

using Microsoft::WRL::ComPtr;

namespace {

struct Candidate {
    const char* name;
    uint8_t codec;
    bool hwFrames;  // takes D3D11 textures directly
};

const Candidate kCandidates[] = {
    {"hevc_nvenc", hl::CODEC_HEVC, true}, {"hevc_amf", hl::CODEC_HEVC, true},
    {"hevc_qsv", hl::CODEC_HEVC, false},  {"av1_nvenc", hl::CODEC_AV1, true},
    {"av1_amf", hl::CODEC_AV1, true},     {"av1_qsv", hl::CODEC_AV1, false},
    {"h264_nvenc", hl::CODEC_H264, true}, {"h264_amf", hl::CODEC_H264, true},
    {"h264_qsv", hl::CODEC_H264, false},  {"libx264", hl::CODEC_H264, false},
};

std::string averr(int e) {
    char b[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(e, b, sizeof b);
    return b;
}

void opt(AVCodecContext* c, const char* k, const char* v) {
    av_opt_set(c->priv_data, k, v, 0);  // unknown options are fine; encoders differ by version
}

}  // namespace

Encoder::~Encoder() { close(); }

void Encoder::close() {
    if (enc_) avcodec_free_context(&enc_);
    if (hwFrames_) av_buffer_unref(&hwFrames_);
    if (hwDevice_) av_buffer_unref(&hwDevice_);
    if (swFrame_) av_frame_free(&swFrame_);
    if (pkt_) av_packet_free(&pkt_);
    nv12_.Reset();
    staging_.Reset();
}

bool Encoder::init(ID3D11Device* device, std::recursive_mutex* gpuLock, const EncoderConfig& cfg) {
    device_ = device;
    lock_ = gpuLock;
    device_->GetImmediateContext(&ctx_);
    cfg_ = cfg;
    if (!conv_.init(device, cfg.srcWidth, cfg.srcHeight, cfg.width, cfg.height, cfg.rotation))
        return false;

    // Requested codec first, then HEVC, then H.264.
    uint8_t order[] = {cfg.codec, hl::CODEC_HEVC, hl::CODEC_H264};
    for (uint8_t c : order)
        for (auto& cand : kCandidates)
            if (cand.codec == c && open(cand.name, cand.codec, cand.hwFrames)) return true;
    hl::log("encoder: no usable encoder");
    return false;
}

bool Encoder::open(const char* encoderName, uint8_t codec, bool hwFrames) {
    close();
    const AVCodec* codecDef = avcodec_find_encoder_by_name(encoderName);
    if (!codecDef) return false;
    enc_ = avcodec_alloc_context3(codecDef);
    enc_->width = cfg_.width;
    enc_->height = cfg_.height;
    enc_->time_base = {1, cfg_.fps};
    enc_->framerate = {cfg_.fps, 1};
    enc_->bit_rate = (int64_t)cfg_.bitrateKbps * 1000;
    enc_->rc_max_rate = enc_->bit_rate;
    // Roughly one frame of VBV: bitrate spikes turn into latency on the wire otherwise.
    enc_->rc_buffer_size = (int)(enc_->bit_rate / cfg_.fps * 2);
    enc_->gop_size = cfg_.fps * 120;  // keyframes only on request (loss recovery)
    enc_->keyint_min = enc_->gop_size;
    enc_->max_b_frames = 0;
    enc_->refs = 1;
    enc_->thread_count = 1;
    enc_->flags |= AV_CODEC_FLAG_LOW_DELAY;
    enc_->flags2 |= AV_CODEC_FLAG2_FAST;
    enc_->color_range = AVCOL_RANGE_MPEG;
    enc_->colorspace = AVCOL_SPC_BT709;
    enc_->color_primaries = AVCOL_PRI_BT709;
    enc_->color_trc = AVCOL_TRC_BT709;

    std::string n = encoderName;
    if (n.find("nvenc") != std::string::npos) {
        opt(enc_, "preset", "p1");
        opt(enc_, "tune", "ull");
        opt(enc_, "rc", "cbr");
        opt(enc_, "zerolatency", "1");
        opt(enc_, "delay", "0");
        opt(enc_, "forced-idr", "1");
        opt(enc_, "rc-lookahead", "0");
        opt(enc_, "no-scenecut", "1");
        opt(enc_, "multipass", "disabled");
    } else if (n.find("amf") != std::string::npos) {
        opt(enc_, "usage", "ultralowlatency");
        opt(enc_, "quality", "speed");
        opt(enc_, "rc", "cbr");
        opt(enc_, "latency", "1");
        opt(enc_, "header_insertion_mode", "idr");
        opt(enc_, "preencode", "0");
        opt(enc_, "vbaq", "0");
        opt(enc_, "enforce_hrd", "1");
        opt(enc_, "filler_data", "0");
    } else if (n.find("qsv") != std::string::npos) {
        opt(enc_, "preset", "veryfast");
        opt(enc_, "async_depth", "1");
        opt(enc_, "look_ahead", "0");
        opt(enc_, "forced_idr", "1");
    } else if (n == "libx264") {
        opt(enc_, "preset", "ultrafast");
        opt(enc_, "tune", "zerolatency");
        enc_->thread_count = 4;
    }

    int err;
    if (hwFrames) {
        hwDevice_ = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
        auto* dc = reinterpret_cast<AVHWDeviceContext*>(hwDevice_->data);
        auto* d3d = reinterpret_cast<AVD3D11VADeviceContext*>(dc->hwctx);
        d3d->device = device_.Get();
        device_->AddRef();  // released by FFmpeg when the context is freed
        // FFmpeg must take the same lock we use around the shared immediate context.
        d3d->lock_ctx = lock_;
        d3d->lock = [](void* m) { static_cast<std::recursive_mutex*>(m)->lock(); };
        d3d->unlock = [](void* m) { static_cast<std::recursive_mutex*>(m)->unlock(); };
        if ((err = av_hwdevice_ctx_init(hwDevice_)) < 0) {
            hl::log("encoder %s: hw device init failed: %s", encoderName, averr(err).c_str());
            close();
            return false;
        }
        hwFrames_ = av_hwframe_ctx_alloc(hwDevice_);
        auto* fc = reinterpret_cast<AVHWFramesContext*>(hwFrames_->data);
        fc->format = AV_PIX_FMT_D3D11;
        fc->sw_format = AV_PIX_FMT_NV12;
        fc->width = cfg_.width;
        fc->height = cfg_.height;
        fc->initial_pool_size = 0;
        reinterpret_cast<AVD3D11VAFramesContext*>(fc->hwctx)->BindFlags = D3D11_BIND_RENDER_TARGET;
        if ((err = av_hwframe_ctx_init(hwFrames_)) < 0) {
            hl::log("encoder %s: hw frames init failed: %s", encoderName, averr(err).c_str());
            close();
            return false;
        }
        enc_->pix_fmt = AV_PIX_FMT_D3D11;
        enc_->hw_frames_ctx = av_buffer_ref(hwFrames_);
    } else {
        enc_->pix_fmt = AV_PIX_FMT_NV12;
        D3D11_TEXTURE2D_DESC td{};
        td.Width = cfg_.width;
        td.Height = cfg_.height;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_NV12;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (FAILED(device_->CreateTexture2D(&td, nullptr, &nv12_))) {
            close();
            return false;
        }
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(device_->CreateTexture2D(&td, nullptr, &staging_))) {
            close();
            return false;
        }
        swFrame_ = av_frame_alloc();
        swFrame_->format = AV_PIX_FMT_NV12;
        swFrame_->width = cfg_.width;
        swFrame_->height = cfg_.height;
        av_frame_get_buffer(swFrame_, 32);
    }

    if ((err = avcodec_open2(enc_, codecDef, nullptr)) < 0) {
        hl::log("encoder %s: open failed: %s", encoderName, averr(err).c_str());
        close();
        return false;
    }
    pkt_ = av_packet_alloc();
    hw_ = hwFrames;
    name_ = encoderName;
    codec_ = codec;
    pts_ = 0;
    hl::log("encoder: using %s %dx%d@%d %d kbps", encoderName, cfg_.width, cfg_.height, cfg_.fps,
            cfg_.bitrateKbps);
    return true;
}

void Encoder::setBitrate(int kbps) {
    if (!enc_) return;
    cfg_.bitrateKbps = kbps;
    enc_->bit_rate = (int64_t)kbps * 1000;
    enc_->rc_max_rate = enc_->bit_rate;
    enc_->rc_buffer_size = (int)(enc_->bit_rate / cfg_.fps * 2);
}

bool Encoder::encode(ID3D11Texture2D* bgra, bool forceKeyframe,
                     const std::function<void(const uint8_t*, size_t, bool)>& onPacket) {
    if (!enc_) return false;
    std::unique_lock<std::recursive_mutex> gpu(*lock_);
    AVFrame* frame = nullptr;
    if (hw_) {
        frame = av_frame_alloc();
        if (av_hwframe_get_buffer(hwFrames_, frame, 0) < 0) {
            av_frame_free(&frame);
            return false;
        }
        auto* tex = reinterpret_cast<ID3D11Texture2D*>(frame->data[0]);
        UINT slice = (UINT)(intptr_t)frame->data[1];
        if (!conv_.convert(bgra, tex, slice)) {
            av_frame_free(&frame);
            return false;
        }
    } else {
        if (!conv_.convert(bgra, nv12_.Get(), 0)) return false;
        ctx_->CopyResource(staging_.Get(), nv12_.Get());
        D3D11_MAPPED_SUBRESOURCE map;
        if (FAILED(ctx_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &map))) return false;
        av_frame_make_writable(swFrame_);
        const uint8_t* src = (const uint8_t*)map.pData;
        for (int y = 0; y < cfg_.height; y++)
            memcpy(swFrame_->data[0] + y * swFrame_->linesize[0], src + y * map.RowPitch, cfg_.width);
        const uint8_t* uv = src + map.RowPitch * cfg_.height;
        for (int y = 0; y < cfg_.height / 2; y++)
            memcpy(swFrame_->data[1] + y * swFrame_->linesize[1], uv + y * map.RowPitch, cfg_.width);
        ctx_->Unmap(staging_.Get(), 0);
        frame = av_frame_clone(swFrame_);
    }

    frame->pts = pts_++;
    if (forceKeyframe) {
        frame->pict_type = AV_PICTURE_TYPE_I;
#ifdef AV_FRAME_FLAG_KEY
        frame->flags |= AV_FRAME_FLAG_KEY;
#endif
    }
    int err = avcodec_send_frame(enc_, frame);
    av_frame_free(&frame);
    gpu.unlock();
    if (err < 0) {
        hl::log("encoder: send_frame failed: %s", averr(err).c_str());
        return false;
    }

    // Low-latency encoders hand the packet back right away; give slower ones a few ms so the
    // frame leaves now instead of one frame later.
    uint64_t deadline = hl::nowUs() + 20000;
    bool got = false;
    for (;;) {
        err = avcodec_receive_packet(enc_, pkt_);
        if (err == 0) {
            onPacket(pkt_->data, pkt_->size, (pkt_->flags & AV_PKT_FLAG_KEY) != 0);
            av_packet_unref(pkt_);
            got = true;
            continue;
        }
        if (err == AVERROR(EAGAIN) && !got && hl::nowUs() < deadline) {
            Sleep(0);
            continue;
        }
        break;
    }
    return err == AVERROR(EAGAIN) || err == 0;
}

bool Encoder::available(const char* encoderName) {
    return avcodec_find_encoder_by_name(encoderName) != nullptr;
}

uint32_t Encoder::probeCodecs(ID3D11Device* device, std::recursive_mutex* gpuLock) {
    uint32_t mask = 0;
    for (uint8_t c : {hl::CODEC_H264, hl::CODEC_HEVC, hl::CODEC_AV1}) {
        EncoderConfig cfg;
        cfg.width = cfg.srcWidth = 1280;
        cfg.height = cfg.srcHeight = 720;
        cfg.fps = 60;
        cfg.bitrateKbps = 5000;
        cfg.codec = c;
        Encoder e;
        e.device_ = device;
        e.lock_ = gpuLock;
        e.device_->GetImmediateContext(&e.ctx_);
        e.cfg_ = cfg;
        for (auto& cand : kCandidates)
            if (cand.codec == c && std::string(cand.name) != "libx264" &&
                e.open(cand.name, cand.codec, cand.hwFrames)) {
                mask |= 1u << c;
                break;
            }
    }
    return mask | (1u << hl::CODEC_H264);  // x264 always works as a last resort
}
