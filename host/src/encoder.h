// Hardware video encoder through FFmpeg: NVIDIA NVENC, AMD AMF or Intel QSV, with the
// frames staying on the GPU where the encoder allows it. Falls back to x264 in software.
#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "converter.h"

struct AVCodecContext;
struct AVBufferRef;
struct AVFrame;
struct AVPacket;

struct EncoderConfig {
    int width = 0, height = 0;          // output size
    int srcWidth = 0, srcHeight = 0;    // captured texture size (before rotation)
    DXGI_MODE_ROTATION rotation = DXGI_MODE_ROTATION_IDENTITY;
    int fps = 120;
    int bitrateKbps = 50000;
    uint8_t codec = 1;                  // hl::Codec
};

class Encoder {
public:
    ~Encoder();
    // Tries the hardware encoders for the requested codec, then other codecs, then software.
    // gpuLock guards the device's immediate context, shared with capture and other encoders.
    bool init(ID3D11Device* device, std::recursive_mutex* gpuLock, const EncoderConfig& cfg);
    // Encodes one BGRA desktop texture. onPacket receives the Annex-B bitstream.
    bool encode(ID3D11Texture2D* bgra, bool forceKeyframe,
                const std::function<void(const uint8_t*, size_t, bool key)>& onPacket);
    void setBitrate(int kbps);

    const std::string& name() const { return name_; }
    uint8_t codec() const { return codec_; }
    // Which codecs this machine can hardware-encode (bitmask of hl::Codec), probed once.
    static bool available(const char* encoderName);
    static uint32_t probeCodecs(ID3D11Device* device, std::recursive_mutex* gpuLock);

private:
    bool open(const char* encoderName, uint8_t codec, bool hwFrames);
    void close();

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> ctx_;
    std::recursive_mutex* lock_ = nullptr;
    EncoderConfig cfg_;
    Converter conv_;
    AVCodecContext* enc_ = nullptr;
    AVBufferRef* hwDevice_ = nullptr;
    AVBufferRef* hwFrames_ = nullptr;
    AVFrame* swFrame_ = nullptr;
    AVPacket* pkt_ = nullptr;
    bool hw_ = false;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> nv12_, staging_;  // software path only
    int64_t pts_ = 0;
    std::string name_;
    uint8_t codec_ = 0;
};
