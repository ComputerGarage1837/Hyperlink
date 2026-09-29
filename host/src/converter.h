// GPU colour conversion and scaling (BGRA desktop -> NV12 for the encoder) using the
// D3D11 video processor, which runs on the GPU's fixed-function video hardware.
#pragma once

#include <d3d11_1.h>
#include <wrl/client.h>

#include <map>

class Converter {
public:
    bool init(ID3D11Device* device, int inW, int inH, int outW, int outH, DXGI_MODE_ROTATION rotation);
    // Writes the converted image into array slice `slice` of the NV12 texture `out`.
    bool convert(ID3D11Texture2D* in, ID3D11Texture2D* out, UINT slice);

private:
    Microsoft::WRL::ComPtr<ID3D11VideoDevice> vdev_;
    Microsoft::WRL::ComPtr<ID3D11VideoContext> vctx_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> enum_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessor> proc_;
    std::map<ID3D11Texture2D*, Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView>> inViews_;
    std::map<std::pair<ID3D11Texture2D*, UINT>, Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView>> outViews_;
};
