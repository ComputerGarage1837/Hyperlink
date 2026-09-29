// Hardware video decoder (MediaCodec) rendering straight to a Surface, tuned for latency.
#pragma once

#include <android/native_window.h>
#include <media/NdkMediaCodec.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>

class Decoder {
public:
    ~Decoder() { stop(); }
    // Starts decoding into `window`, using the parameter sets found in the first keyframe.
    bool start(ANativeWindow* window, uint8_t codec, int width, int height, int fps,
               const uint8_t* keyframe, size_t size);
    // Queues one access unit. Returns false if the decoder is broken and needs a restart.
    bool submit(const uint8_t* data, size_t size, bool keyframe);
    void stop();
    bool running() const { return codec_ != nullptr; }

    // Stats, reset by takeStats().
    struct Stats {
        int framesDecoded = 0;
        uint64_t decodeUsSum = 0;
        int dropped = 0;
    };
    Stats takeStats();

private:
    void outputLoop();

    AMediaCodec* codec_ = nullptr;
    std::thread output_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> failed_{false};
    std::mutex statsMutex_;
    Stats stats_;
};
