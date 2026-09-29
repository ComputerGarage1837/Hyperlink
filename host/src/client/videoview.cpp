#include "videoview.h"

#include <windowsx.h>

#include <algorithm>

#include "../capture.h"
#include "hyperlink/common.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
}

using Microsoft::WRL::ComPtr;

// Not in every SDK's headers.
static const GUID kAv1Profile0 = {0xb8be4ccb, 0xcf53, 0x46ba, {0x8d, 0x59, 0xd6, 0xb8, 0xa6, 0xda, 0x5d, 0x2a}};

static const wchar_t* kClass = L"HyperlinkVideoView";

void VideoView::registerClass(HINSTANCE inst) {
    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
}

VideoView::VideoView(HWND parent, const hl::MonitorInfo& monitor, Input input)
    : monitor_(monitor), input_(std::move(input)) {
    gpu_ = GpuDevice::forAdapter(0);
    hwnd_ = CreateWindowExW(0, kClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0, 10, 10, parent,
                            nullptr, GetModuleHandleW(nullptr), this);
    if (gpu_) {
        gpu_->device.As(&vdev_);
        ComPtr<ID3D11DeviceContext> ctx;
        gpu_->device->GetImmediateContext(&ctx);
        ctx.As(&vctx_);
    }
}

VideoView::~VideoView() {
    {
        std::lock_guard<std::mutex> lock(m_);
        closeDecoder();
        outView_.Reset();
        swap_.Reset();
    }
    if (hwnd_) {
        SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
        DestroyWindow(hwnd_);
    }
}

void VideoView::setCursor(HCURSOR c, bool visible) {
    cursor_ = c;
    cursorVisible_ = visible;
    POINT p;
    GetCursorPos(&p);
    if (WindowFromPoint(p) == hwnd_) SetCursor(visible ? (c ? c : LoadCursor(nullptr, IDC_ARROW)) : nullptr);
}

uint32_t VideoView::decodableCodecs() {
    uint32_t mask = 0;
    auto gpu = GpuDevice::forAdapter(0);
    ComPtr<ID3D11VideoDevice> vd;
    if (!gpu || FAILED(gpu->device.As(&vd))) return 1u << hl::CODEC_H264;
    UINT n = vd->GetVideoDecoderProfileCount();
    for (UINT i = 0; i < n; i++) {
        GUID g;
        if (FAILED(vd->GetVideoDecoderProfile(i, &g))) continue;
        if (g == D3D11_DECODER_PROFILE_H264_VLD_NOFGT) mask |= 1u << hl::CODEC_H264;
        if (g == D3D11_DECODER_PROFILE_HEVC_VLD_MAIN) mask |= 1u << hl::CODEC_HEVC;
        if (g == kAv1Profile0) mask |= 1u << hl::CODEC_AV1;
    }
    return mask ? mask : 1u << hl::CODEC_H264;
}

static enum AVPixelFormat pickD3D11(AVCodecContext*, const enum AVPixelFormat* fmts) {
    for (const enum AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; p++)
        if (*p == AV_PIX_FMT_D3D11) return *p;
    return fmts[0];
}

bool VideoView::openDecoder(uint8_t codec) {
    closeDecoder();
    if (!gpu_) return false;
    AVCodecID id = codec == hl::CODEC_H264 ? AV_CODEC_ID_H264 : codec == hl::CODEC_AV1 ? AV_CODEC_ID_AV1 : AV_CODEC_ID_HEVC;
    const AVCodec* c = avcodec_find_decoder(id);
    if (!c) return false;
    hwDevice_ = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
    auto* dc = reinterpret_cast<AVHWDeviceContext*>(hwDevice_->data);
    auto* d3d = reinterpret_cast<AVD3D11VADeviceContext*>(dc->hwctx);
    d3d->device = gpu_->device.Get();
    gpu_->device->AddRef();
    d3d->lock_ctx = &gpu_->lock;
    d3d->lock = [](void* m) { static_cast<std::recursive_mutex*>(m)->lock(); };
    d3d->unlock = [](void* m) { static_cast<std::recursive_mutex*>(m)->unlock(); };
    if (av_hwdevice_ctx_init(hwDevice_) < 0) {
        closeDecoder();
        return false;
    }
    dec_ = avcodec_alloc_context3(c);
    dec_->hw_device_ctx = av_buffer_ref(hwDevice_);
    dec_->get_format = pickD3D11;
    dec_->flags |= AV_CODEC_FLAG_LOW_DELAY;
    dec_->flags2 |= AV_CODEC_FLAG2_FAST;
    dec_->thread_count = 1;  // frame threading would add a frame of delay per thread
    if (avcodec_open2(dec_, c, nullptr) < 0) {
        closeDecoder();
        return false;
    }
    pkt_ = av_packet_alloc();
    frame_ = av_frame_alloc();
    codec_ = codec;
    hl::log("client: decoding %s for %s", hl::codecName(codec), monitor_.name.c_str());
    return true;
}

void VideoView::closeDecoder() {
    if (dec_) avcodec_free_context(&dec_);
    if (hwDevice_) av_buffer_unref(&hwDevice_);
    if (pkt_) av_packet_free(&pkt_);
    if (frame_) av_frame_free(&frame_);
    codec_ = 255;
}

bool VideoView::onFrame(const hl::CompleteFrame& f, const hl::StreamStarted& info) {
    std::lock_guard<std::mutex> lock(m_);
    if (!dec_ || codec_ != info.codec) {
        if (!f.keyframe || !openDecoder(info.codec)) return false;
    }
    videoW_ = info.width;
    videoH_ = info.height;
    uint64_t t0 = hl::nowUs();
    pkt_->data = const_cast<uint8_t*>(f.data.data());
    pkt_->size = (int)f.data.size();
    if (f.keyframe) pkt_->flags |= AV_PKT_FLAG_KEY;
    int err = avcodec_send_packet(dec_, pkt_);
    av_packet_unref(pkt_);
    if (err < 0 && err != AVERROR(EAGAIN)) {
        closeDecoder();
        return false;
    }
    bool ok = true;
    while (avcodec_receive_frame(dec_, frame_) == 0) {
        ok = present(frame_) && ok;
        av_frame_unref(frame_);
        stats_.frames++;
        stats_.decodeMsSum += (hl::nowUs() - t0) / 1000.0;
    }
    return true;
}

bool VideoView::ensureSwapChain() {
    int w = std::max(1, clientW_.load()), h = std::max(1, clientH_.load());
    if (swap_ && w == swapW_ && h == swapH_) return true;
    outView_.Reset();
    if (!swap_) {
        ComPtr<IDXGIDevice> dxgiDev;
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<IDXGIFactory2> factory;
        gpu_->device.As(&dxgiDev);
        dxgiDev->GetAdapter(&adapter);
        adapter->GetParent(IID_PPV_ARGS(&factory));
        DXGI_SWAP_CHAIN_DESC1 d{};
        d.Width = w;
        d.Height = h;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        d.BufferCount = 3;
        d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        d.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
        if (FAILED(factory->CreateSwapChainForHwnd(gpu_->device.Get(), hwnd_, &d, nullptr, nullptr, &swap_))) {
            d.Flags = 0;
            if (FAILED(factory->CreateSwapChainForHwnd(gpu_->device.Get(), hwnd_, &d, nullptr, nullptr, &swap_)))
                return false;
        }
        factory->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);
    } else {
        DXGI_SWAP_CHAIN_DESC1 d{};
        swap_->GetDesc1(&d);
        if (FAILED(swap_->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, d.Flags))) return false;
    }
    swapW_ = w;
    swapH_ = h;
    return true;
}

RECT VideoView::videoRect() const {
    int cw = std::max(1, clientW_.load()), ch = std::max(1, clientH_.load());
    int vw = std::max(1, videoW_.load() ? videoW_.load() : (int)monitor_.width);
    int vh = std::max(1, videoH_.load() ? videoH_.load() : (int)monitor_.height);
    double s = std::min((double)cw / vw, (double)ch / vh);
    int w = (int)(vw * s), h = (int)(vh * s);
    RECT r{(cw - w) / 2, (ch - h) / 2, (cw - w) / 2 + w, (ch - h) / 2 + h};
    return r;
}

bool VideoView::present(AVFrame* f) {
    if (f->format != AV_PIX_FMT_D3D11) return false;
    auto* tex = reinterpret_cast<ID3D11Texture2D*>(f->data[0]);
    UINT slice = (UINT)(intptr_t)f->data[1];
    std::lock_guard<std::recursive_mutex> gpu(gpu_->lock);
    if (!ensureSwapChain() || !vdev_ || !vctx_) return false;

    if (!vproc_ || vpInW != f->width || vpInH != f->height || vpOutW != swapW_ || vpOutH != swapH_) {
        vproc_.Reset();
        venum_.Reset();
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
        cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        cd.InputWidth = f->width;
        cd.InputHeight = f->height;
        cd.OutputWidth = swapW_;
        cd.OutputHeight = swapH_;
        cd.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;
        if (FAILED(vdev_->CreateVideoProcessorEnumerator(&cd, &venum_)) ||
            FAILED(vdev_->CreateVideoProcessor(venum_.Get(), 0, &vproc_)))
            return false;
        ComPtr<ID3D11VideoContext1> v1;
        if (SUCCEEDED(vctx_.As(&v1))) {
            v1->VideoProcessorSetStreamColorSpace1(vproc_.Get(), 0, DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709);
            v1->VideoProcessorSetOutputColorSpace1(vproc_.Get(), DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
        }
        vctx_->VideoProcessorSetStreamFrameFormat(vproc_.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
        vctx_->VideoProcessorSetStreamAutoProcessingMode(vproc_.Get(), 0, FALSE);
        D3D11_VIDEO_COLOR black{};
        black.RGBA.A = 1.0f;
        vctx_->VideoProcessorSetOutputBackgroundColor(vproc_.Get(), FALSE, &black);
        vpInW = f->width;
        vpInH = f->height;
        vpOutW = swapW_;
        vpOutH = swapH_;
        outView_.Reset();
    }
    ComPtr<ID3D11Texture2D> back;
    if (FAILED(swap_->GetBuffer(0, IID_PPV_ARGS(&back)))) return false;
    ComPtr<ID3D11VideoProcessorOutputView> ov;
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC od{};
    od.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    if (FAILED(vdev_->CreateVideoProcessorOutputView(back.Get(), venum_.Get(), &od, &ov))) return false;
    ComPtr<ID3D11VideoProcessorInputView> iv;
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC id{};
    id.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    id.Texture2D.ArraySlice = slice;
    if (FAILED(vdev_->CreateVideoProcessorInputView(tex, venum_.Get(), &id, &iv))) return false;

    RECT src{0, 0, f->width, f->height};
    RECT dst = videoRect();
    RECT full{0, 0, swapW_, swapH_};
    vctx_->VideoProcessorSetStreamSourceRect(vproc_.Get(), 0, TRUE, &src);
    vctx_->VideoProcessorSetStreamDestRect(vproc_.Get(), 0, TRUE, &dst);
    vctx_->VideoProcessorSetOutputTargetRect(vproc_.Get(), TRUE, &full);
    D3D11_VIDEO_PROCESSOR_STREAM s{};
    s.Enable = TRUE;
    s.pInputSurface = iv.Get();
    if (FAILED(vctx_->VideoProcessorBlt(vproc_.Get(), ov.Get(), 0, 1, &s))) return false;
    DXGI_SWAP_CHAIN_DESC1 d{};
    swap_->GetDesc1(&d);
    // Present immediately, without waiting for vsync: the lowest-latency path.
    swap_->Present(0, (d.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) ? DXGI_PRESENT_ALLOW_TEARING : 0);
    return true;
}

VideoView::Stats VideoView::takeStats() {
    std::lock_guard<std::mutex> lock(m_);
    Stats s = stats_;
    stats_ = Stats();
    return s;
}

LRESULT CALLBACK VideoView::proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
    }
    auto* self = reinterpret_cast<VideoView*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (self && self->hwnd_ == nullptr) self->hwnd_ = h;
    return self ? self->handle(msg, wp, lp) : DefWindowProcW(h, msg, wp, lp);
}

LRESULT VideoView::handle(UINT msg, WPARAM wp, LPARAM lp) {
    auto toRemote = [&](int x, int y) {
        RECT r = videoRect();
        double nx = std::clamp((double)(x - r.left) / std::max<LONG>(1, r.right - r.left), 0.0, 1.0);
        double ny = std::clamp((double)(y - r.top) / std::max<LONG>(1, r.bottom - r.top), 0.0, 1.0);
        if (input_.move) input_.move(monitor_.id, (uint16_t)(nx * 65535), (uint16_t)(ny * 65535));
    };
    auto button = [&](uint8_t b, bool down) {
        if (input_.focusParent && down) input_.focusParent();
        if (down) {
            if (!buttonsDown_++) SetCapture(hwnd_);
        } else if (buttonsDown_ > 0 && !--buttonsDown_) {
            ReleaseCapture();
        }
        if (input_.button) input_.button(b, down);
    };
    switch (msg) {
        case WM_SIZE:
            clientW_ = LOWORD(lp);
            clientH_ = HIWORD(lp);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd_, &ps);
            if (!swap_) FillRect(ps.hdc, &ps.rcPaint, (HBRUSH)GetStockObject(BLACK_BRUSH));
            EndPaint(hwnd_, &ps);
            return 0;
        }
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                SetCursor(cursorVisible_ ? (cursor_ ? cursor_ : LoadCursor(nullptr, IDC_ARROW)) : nullptr);
                return TRUE;
            }
            break;
        case WM_MOUSEMOVE: toRemote(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)); return 0;
        case WM_LBUTTONDOWN: toRemote(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)); button(hl::MOUSE_LEFT, true); return 0;
        case WM_LBUTTONUP: button(hl::MOUSE_LEFT, false); return 0;
        case WM_RBUTTONDOWN: toRemote(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)); button(hl::MOUSE_RIGHT, true); return 0;
        case WM_RBUTTONUP: button(hl::MOUSE_RIGHT, false); return 0;
        case WM_MBUTTONDOWN: button(hl::MOUSE_MIDDLE, true); return 0;
        case WM_MBUTTONUP: button(hl::MOUSE_MIDDLE, false); return 0;
        case WM_XBUTTONDOWN:
        case WM_XBUTTONUP:
            button(GET_XBUTTON_WPARAM(wp) == XBUTTON1 ? hl::MOUSE_X1 : hl::MOUSE_X2, msg == WM_XBUTTONDOWN);
            return TRUE;
        case WM_MOUSEWHEEL:
            if (input_.scroll) input_.scroll(GET_WHEEL_DELTA_WPARAM(wp), 0);
            return 0;
        case WM_MOUSEHWHEEL:
            if (input_.scroll) input_.scroll(0, GET_WHEEL_DELTA_WPARAM(wp));
            return 0;
        case WM_CAPTURECHANGED:
            buttonsDown_ = 0;
            return 0;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}
