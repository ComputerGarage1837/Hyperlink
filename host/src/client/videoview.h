// A child window showing one remote monitor: hardware decode (D3D11VA through FFmpeg, any
// GPU vendor) straight into a flip-model swap chain, presented the moment a frame is ready.
#pragma once

#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_5.h>
#include <wrl/client.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>

#include "hyperlink/client.h"

struct AVCodecContext;
struct AVBufferRef;
struct AVFrame;
struct AVPacket;
struct GpuDevice;

class VideoView : public hl::FrameSink, public std::enable_shared_from_this<VideoView> {
public:
    struct Input {
        std::function<void(uint32_t monitor, uint16_t x, uint16_t y)> move;
        std::function<void(uint8_t button, bool down)> button;
        std::function<void(int dy, int dx)> scroll;
        std::function<void()> focusParent;
        std::function<void(int x, int y)> hover;  // pointer position in the view (for the toolbar)
    };

    VideoView(HWND parent, const hl::MonitorInfo& monitor, Input input);
    ~VideoView() override;

    HWND hwnd() const { return hwnd_; }
    const hl::MonitorInfo& monitor() const { return monitor_; }
    void setCursor(HCURSOR c, bool visible);

    // hl::FrameSink, called on the network thread.
    bool onFrame(const hl::CompleteFrame& f, const hl::StreamStarted& info) override;

    struct Stats { int frames = 0; double decodeMsSum = 0; };
    Stats takeStats();

    // Which codecs this PC's GPU can decode (bitmask of hl::Codec).
    static uint32_t decodableCodecs();
    static void registerClass(HINSTANCE inst);

private:
    static LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);
    bool openDecoder(uint8_t codec);
    void closeDecoder();
    bool present(AVFrame* f);
    bool ensureSwapChain();
    RECT videoRect() const;  // where the picture sits inside the client area

    HWND hwnd_ = nullptr;
    hl::MonitorInfo monitor_;
    Input input_;
    std::shared_ptr<GpuDevice> gpu_;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> swap_;
    Microsoft::WRL::ComPtr<ID3D11VideoDevice> vdev_;
    Microsoft::WRL::ComPtr<ID3D11VideoContext> vctx_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> venum_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessor> vproc_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> outView_;
    int vpInW = 0, vpInH = 0, vpOutW = 0, vpOutH = 0;
    std::atomic<int> clientW_{0}, clientH_{0};
    int swapW_ = 0, swapH_ = 0;
    std::atomic<int> videoW_{0}, videoH_{0};

    AVCodecContext* dec_ = nullptr;
    AVBufferRef* hwDevice_ = nullptr;
    AVPacket* pkt_ = nullptr;
    AVFrame* frame_ = nullptr;
    uint8_t codec_ = 255;
    std::mutex m_;
    Stats stats_;
    HCURSOR cursor_ = nullptr;
    bool cursorVisible_ = true;
    int buttonsDown_ = 0;
};
