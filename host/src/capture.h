// Desktop capture. One Capture per monitor, shared by every stream showing that monitor
// (several clients can watch the same screen). It keeps the newest desktop image in a GPU
// texture and bumps a sequence number whenever the image or the cursor changes.
#pragma once

#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "monitors.h"

// One D3D11 device per graphics adapter, shared by capture and encoders on that adapter.
struct GpuDevice {
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> ctx;
    std::recursive_mutex lock;  // guards the immediate context (FFmpeg uses it too)

    static std::shared_ptr<GpuDevice> forAdapter(int adapterIndex);
};

struct CursorState {
    bool visible = false;
    int x = 0, y = 0;             // relative to the monitor, desktop orientation
    uint64_t shapeSeq = 0;
    int width = 0, height = 0, hotX = 0, hotY = 0;
    std::vector<uint8_t> rgba;
};

class Capture {
public:
    // Returns the running capture for this monitor, starting one if needed.
    static std::shared_ptr<Capture> get(const HostMonitor& m);
    ~Capture();

    struct Snapshot {
        uint64_t frameSeq = 0;    // changes when the image changes
        uint64_t cursorSeq = 0;   // changes when the cursor moves or changes shape
        uint64_t modeSeq = 0;     // changes when size or rotation changes
        int width = 0, height = 0;  // texture size (unrotated)
        DXGI_MODE_ROTATION rotation = DXGI_MODE_ROTATION_IDENTITY;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    };

    // Waits until frameSeq or cursorSeq differs from the given values, or the timeout passes.
    Snapshot wait(uint64_t frameSeq, uint64_t cursorSeq, int timeoutMs);
    CursorState cursor();
    std::shared_ptr<GpuDevice> gpu() const { return gpu_; }
    const HostMonitor& monitor() const { return mon_; }
    uint16_t fpsLastSecond() const { return fps_; }

private:
    explicit Capture(const HostMonitor& m);
    void run();
    bool setup();
    void updatePointer(IDXGIOutputDuplication* dup, const DXGI_OUTDUPL_FRAME_INFO& info);

    HostMonitor mon_;
    std::shared_ptr<GpuDevice> gpu_;
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> dup_;
    std::thread thread_;
    std::atomic<bool> stop_{false};

    std::mutex m_;
    std::condition_variable cv_;
    Snapshot snap_;
    CursorState cursor_;
    std::vector<uint8_t> shapeBuf_;
    std::atomic<uint16_t> fps_{0};
};
