#include "capture.h"

#include <windows.h>
#include <avrt.h>
#include <dxgi1_5.h>

#include <map>

#include "hyperlink/common.h"

using Microsoft::WRL::ComPtr;

static ComPtr<IDXGIAdapter1> adapterAt(int index) {
    ComPtr<IDXGIFactory1> f;
    ComPtr<IDXGIAdapter1> a;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&f)))) f->EnumAdapters1(index, &a);
    return a;
}

std::shared_ptr<GpuDevice> GpuDevice::forAdapter(int adapterIndex) {
    static std::mutex m;
    static std::map<int, std::weak_ptr<GpuDevice>> cache;
    std::lock_guard<std::mutex> lock(m);
    if (auto g = cache[adapterIndex].lock()) return g;

    auto adapter = adapterAt(adapterIndex);
    if (!adapter) return nullptr;
    auto g = std::make_shared<GpuDevice>();
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    UINT flags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    HRESULT hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, levels, 2,
                                   D3D11_SDK_VERSION, &g->device, nullptr, &g->ctx);
    if (FAILED(hr)) {
        hl::log("gpu: D3D11CreateDevice failed on adapter %d: 0x%08lx", adapterIndex, hr);
        return nullptr;
    }
    ComPtr<ID3D10Multithread> mt;
    if (SUCCEEDED(g->device.As(&mt))) mt->SetMultithreadProtected(TRUE);
    // Keep capture and encode responsive while a game saturates the GPU.
    ComPtr<IDXGIDevice> dxgi;
    if (SUCCEEDED(g->device.As(&dxgi))) dxgi->SetGPUThreadPriority(7);
    cache[adapterIndex] = g;
    return g;
}

std::shared_ptr<Capture> Capture::get(const HostMonitor& m) {
    static std::mutex mx;
    static std::map<uint32_t, std::weak_ptr<Capture>> running;
    std::lock_guard<std::mutex> lock(mx);
    auto& slot = running[m.info.id];
    if (auto c = slot.lock()) return c;
    std::shared_ptr<Capture> c(new Capture(m));
    if (!c->gpu_) return nullptr;
    c->thread_ = std::thread([p = c.get()] { p->run(); });
    slot = c;
    return c;
}

Capture::Capture(const HostMonitor& m) : mon_(m) { gpu_ = GpuDevice::forAdapter(m.adapterIndex); }

Capture::~Capture() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
}

bool Capture::setup() {
    // Follow the input desktop so capture resumes after UAC prompts or unlocking.
    if (HDESK desk = OpenInputDesktop(0, FALSE, GENERIC_ALL)) {
        SetThreadDesktop(desk);
        CloseDesktop(desk);
    }
    auto adapter = adapterAt(mon_.adapterIndex);
    if (!adapter) return false;
    ComPtr<IDXGIOutput> out;
    if (FAILED(adapter->EnumOutputs(mon_.outputIndex, &out))) return false;

    HRESULT hr = E_FAIL;
    ComPtr<IDXGIOutput5> out5;
    if (SUCCEEDED(out.As(&out5))) {
        DXGI_FORMAT fmts[] = {DXGI_FORMAT_B8G8R8A8_UNORM};
        hr = out5->DuplicateOutput1(gpu_->device.Get(), 0, 1, fmts, &dup_);
    }
    if (FAILED(hr)) {
        ComPtr<IDXGIOutput1> out1;
        if (FAILED(out.As(&out1))) return false;
        hr = out1->DuplicateOutput(gpu_->device.Get(), &dup_);
    }
    if (FAILED(hr)) {
        dup_.Reset();
        return false;
    }
    DXGI_OUTDUPL_DESC d;
    dup_->GetDesc(&d);
    std::lock_guard<std::mutex> lock(m_);
    if (snap_.rotation != d.Rotation) {
        snap_.rotation = d.Rotation;
        snap_.modeSeq++;
    }
    hl::log("capture: %s duplicating (%ux%u, rotation %d)", mon_.info.name.c_str(), d.ModeDesc.Width,
            d.ModeDesc.Height, (int)d.Rotation);
    return true;
}

void Capture::run() {
    DWORD task = 0;
    HANDLE avrt = AvSetMmThreadCharacteristicsW(L"Capture", &task);
    uint64_t secondStart = hl::nowUs();
    int frames = 0;

    while (!stop_) {
        if (!dup_ && !setup()) {
            Sleep(200);
            continue;
        }
        DXGI_OUTDUPL_FRAME_INFO info{};
        ComPtr<IDXGIResource> res;
        HRESULT hr = dup_->AcquireNextFrame(100, &info, &res);
        uint64_t now = hl::nowUs();
        if (now - secondStart >= 1000000) {
            fps_ = (uint16_t)frames;
            frames = 0;
            secondStart = now;
        }
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) continue;
        if (FAILED(hr)) {
            // ACCESS_LOST: mode change, secure desktop, fullscreen switch. Start over.
            dup_.Reset();
            Sleep(10);
            continue;
        }

        bool imageChanged = info.LastPresentTime.QuadPart != 0;
        bool cursorChanged = info.LastMouseUpdateTime.QuadPart != 0;
        if (imageChanged) {
            ComPtr<ID3D11Texture2D> tex;
            res.As(&tex);
            D3D11_TEXTURE2D_DESC td;
            tex->GetDesc(&td);
            std::lock_guard<std::mutex> lock(m_);
            D3D11_TEXTURE2D_DESC cur{};
            if (snap_.texture) snap_.texture->GetDesc(&cur);
            if (!snap_.texture || cur.Width != td.Width || cur.Height != td.Height || cur.Format != td.Format) {
                D3D11_TEXTURE2D_DESC nd{};
                nd.Width = td.Width;
                nd.Height = td.Height;
                nd.MipLevels = 1;
                nd.ArraySize = 1;
                nd.Format = td.Format;
                nd.SampleDesc.Count = 1;
                nd.Usage = D3D11_USAGE_DEFAULT;
                nd.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
                snap_.texture.Reset();
                gpu_->device->CreateTexture2D(&nd, nullptr, &snap_.texture);
                snap_.width = td.Width;
                snap_.height = td.Height;
                snap_.modeSeq++;
            }
            if (snap_.texture) {
                std::lock_guard<std::recursive_mutex> g(gpu_->lock);
                gpu_->ctx->CopyResource(snap_.texture.Get(), tex.Get());
            }
            snap_.frameSeq++;
            frames++;
        }
        if (cursorChanged || info.PointerShapeBufferSize) updatePointer(dup_.Get(), info);
        dup_->ReleaseFrame();
        if (imageChanged || cursorChanged) cv_.notify_all();
    }
    if (avrt) AvRevertMmThreadCharacteristics(avrt);
}

void Capture::updatePointer(IDXGIOutputDuplication* dup, const DXGI_OUTDUPL_FRAME_INFO& info) {
    std::lock_guard<std::mutex> lock(m_);
    if (info.LastMouseUpdateTime.QuadPart) {
        cursor_.visible = info.PointerPosition.Visible != FALSE;
        cursor_.x = info.PointerPosition.Position.x;
        cursor_.y = info.PointerPosition.Position.y;
    }
    if (info.PointerShapeBufferSize) {
        shapeBuf_.resize(info.PointerShapeBufferSize);
        UINT needed = 0;
        DXGI_OUTDUPL_POINTER_SHAPE_INFO si{};
        if (SUCCEEDED(dup->GetFramePointerShape((UINT)shapeBuf_.size(), shapeBuf_.data(), &needed, &si))) {
            int w = (int)si.Width;
            int h = si.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME ? (int)si.Height / 2 : (int)si.Height;
            std::vector<uint8_t> rgba((size_t)w * h * 4, 0);
            std::vector<uint8_t> invert((size_t)w * h, 0);
            for (int y = 0; y < h; y++) {
                for (int x = 0; x < w; x++) {
                    uint8_t* o = &rgba[((size_t)y * w + x) * 4];
                    if (si.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME) {
                        int byte = x / 8, bit = 7 - (x % 8);
                        bool andBit = (shapeBuf_[y * si.Pitch + byte] >> bit) & 1;
                        bool xorBit = (shapeBuf_[(y + h) * si.Pitch + byte] >> bit) & 1;
                        if (!andBit) { uint8_t c = xorBit ? 255 : 0; o[0] = o[1] = o[2] = c; o[3] = 255; }
                        else if (xorBit) { o[3] = 255; invert[(size_t)y * w + x] = 1; }  // screen inversion
                    } else {
                        const uint8_t* s = &shapeBuf_[y * si.Pitch + x * 4];
                        if (si.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR) {
                            o[0] = s[2]; o[1] = s[1]; o[2] = s[0]; o[3] = s[3];
                        } else {  // masked colour: alpha 0 = opaque colour, 0xFF = XOR with screen
                            bool xorPix = s[3] != 0;
                            if (!xorPix) { o[0] = s[2]; o[1] = s[1]; o[2] = s[0]; o[3] = 255; }
                            else if (s[0] | s[1] | s[2]) { o[3] = 255; invert[(size_t)y * w + x] = 1; }
                        }
                    }
                }
            }
            // Inverting pixels (the text I-beam) can't be drawn on the client: show them black
            // with a white outline so they read on any background.
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++) {
                    if (!invert[(size_t)y * w + x]) continue;
                    for (int dy = -1; dy <= 1; dy++)
                        for (int dx = -1; dx <= 1; dx++) {
                            int nx = x + dx, ny = y + dy;
                            if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                            uint8_t* n = &rgba[((size_t)ny * w + nx) * 4];
                            if (n[3] == 0) { n[0] = n[1] = n[2] = 255; n[3] = 255; }
                        }
                }
            cursor_.width = w;
            cursor_.height = h;
            cursor_.hotX = (int)si.HotSpot.x;
            cursor_.hotY = (int)si.HotSpot.y;
            cursor_.rgba = std::move(rgba);
            cursor_.shapeSeq++;
        }
    }
    snap_.cursorSeq++;
}

Capture::Snapshot Capture::wait(uint64_t frameSeq, uint64_t cursorSeq, int timeoutMs) {
    std::unique_lock<std::mutex> lock(m_);
    cv_.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                 [&] { return snap_.frameSeq != frameSeq || snap_.cursorSeq != cursorSeq; });
    return snap_;
}

CursorState Capture::cursor() {
    std::lock_guard<std::mutex> lock(m_);
    return cursor_;
}
