#include "converter.h"

#include "hyperlink/common.h"

using Microsoft::WRL::ComPtr;

bool Converter::init(ID3D11Device* device, int inW, int inH, int outW, int outH,
                     DXGI_MODE_ROTATION rotation) {
    ComPtr<ID3D11DeviceContext> ctx;
    device->GetImmediateContext(&ctx);
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&vdev_))) ||
        FAILED(ctx->QueryInterface(IID_PPV_ARGS(&vctx_)))) {
        hl::log("converter: no D3D11 video device");
        return false;
    }

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
    cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    cd.InputWidth = inW;
    cd.InputHeight = inH;
    cd.OutputWidth = outW;
    cd.OutputHeight = outH;
    cd.InputFrameRate = {120, 1};
    cd.OutputFrameRate = {120, 1};
    cd.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;
    if (FAILED(vdev_->CreateVideoProcessorEnumerator(&cd, &enum_)) ||
        FAILED(vdev_->CreateVideoProcessor(enum_.Get(), 0, &proc_))) {
        hl::log("converter: cannot create video processor");
        return false;
    }

    // Desktop is full-range sRGB; the stream is BT.709 limited range, which every decoder expects.
    ComPtr<ID3D11VideoContext1> vctx1;
    if (SUCCEEDED(vctx_.As(&vctx1))) {
        vctx1->VideoProcessorSetStreamColorSpace1(proc_.Get(), 0, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
        vctx1->VideoProcessorSetOutputColorSpace1(proc_.Get(), DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709);
    } else {
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE in{}, out{};
        in.RGB_Range = 0;
        out.YCbCr_Matrix = 1;
        out.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
        vctx_->VideoProcessorSetStreamColorSpace(proc_.Get(), 0, &in);
        vctx_->VideoProcessorSetOutputColorSpace(proc_.Get(), &out);
    }

    D3D11_VIDEO_PROCESSOR_ROTATION rot = D3D11_VIDEO_PROCESSOR_ROTATION_IDENTITY;
    switch (rotation) {
        case DXGI_MODE_ROTATION_ROTATE90: rot = D3D11_VIDEO_PROCESSOR_ROTATION_90; break;
        case DXGI_MODE_ROTATION_ROTATE180: rot = D3D11_VIDEO_PROCESSOR_ROTATION_180; break;
        case DXGI_MODE_ROTATION_ROTATE270: rot = D3D11_VIDEO_PROCESSOR_ROTATION_270; break;
        default: break;
    }
    vctx_->VideoProcessorSetStreamRotation(proc_.Get(), 0, rot != D3D11_VIDEO_PROCESSOR_ROTATION_IDENTITY, rot);
    vctx_->VideoProcessorSetStreamFrameFormat(proc_.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    vctx_->VideoProcessorSetStreamAutoProcessingMode(proc_.Get(), 0, FALSE);
    RECT src{0, 0, inW, inH}, dst{0, 0, outW, outH};
    vctx_->VideoProcessorSetStreamSourceRect(proc_.Get(), 0, TRUE, &src);
    vctx_->VideoProcessorSetStreamDestRect(proc_.Get(), 0, TRUE, &dst);
    vctx_->VideoProcessorSetOutputTargetRect(proc_.Get(), TRUE, &dst);
    return true;
}

bool Converter::convert(ID3D11Texture2D* in, ID3D11Texture2D* out, UINT slice) {
    auto& iv = inViews_[in];
    if (!iv) {
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC d{};
        d.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        if (FAILED(vdev_->CreateVideoProcessorInputView(in, enum_.Get(), &d, &iv))) {
            inViews_.erase(in);
            return false;
        }
    }
    auto& ov = outViews_[{out, slice}];
    if (!ov) {
        D3D11_TEXTURE2D_DESC td;
        out->GetDesc(&td);
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC d{};
        if (td.ArraySize > 1) {
            d.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2DARRAY;
            d.Texture2DArray.FirstArraySlice = slice;
            d.Texture2DArray.ArraySize = 1;
        } else {
            d.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        }
        if (FAILED(vdev_->CreateVideoProcessorOutputView(out, enum_.Get(), &d, &ov))) {
            outViews_.erase({out, slice});
            return false;
        }
    }
    D3D11_VIDEO_PROCESSOR_STREAM s{};
    s.Enable = TRUE;
    s.pInputSurface = iv.Get();
    return SUCCEEDED(vctx_->VideoProcessorBlt(proc_.Get(), ov.Get(), 0, 1, &s));
}
