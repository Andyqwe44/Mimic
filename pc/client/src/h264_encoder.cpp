/**
 * h264_encoder.cpp — MF H.264: DXGI hardware MFT first, software last resort.
 */
#include "h264_encoder.h"
#include "../../logger/logger.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <codecapi.h>
#include <wmcodecdsp.h>
#include <wrl/client.h>
#include <vector>
#include <cstring>
#include <algorithm>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "wmcodecdspuuid.lib")

using Microsoft::WRL::ComPtr;

namespace {

void bgra_to_nv12(const uint8_t* bgra, int src_stride_px, int w, int h, uint8_t* nv12) {
    uint8_t* yplane = nv12;
    for (int y = 0; y < h; ++y) {
        const uint8_t* row = bgra + (size_t)y * src_stride_px * 4;
        uint8_t* yrow = yplane + (size_t)y * w;
        for (int x = 0; x < w; ++x) {
            int b = row[x * 4 + 0], g = row[x * 4 + 1], r = row[x * 4 + 2];
            int yv = (77 * r + 150 * g + 29 * b) >> 8;
            yrow[x] = (uint8_t)(yv < 0 ? 0 : (yv > 255 ? 255 : yv));
        }
    }
    uint8_t* uv = nv12 + (size_t)w * h;
    for (int y = 0; y < h; y += 2) {
        int y1 = (y + 1 < h) ? (y + 1) : (h - 1);
        const uint8_t* row0 = bgra + (size_t)y * src_stride_px * 4;
        const uint8_t* row1 = bgra + (size_t)y1 * src_stride_px * 4;
        uint8_t* uvrow = uv + (size_t)(y / 2) * w;
        for (int x = 0; x < w; x += 2) {
            int x1 = (x + 1 < w) ? (x + 1) : (w - 1);
            int b = (row0[x * 4] + row0[x1 * 4] + row1[x * 4] + row1[x1 * 4]) >> 2;
            int g = (row0[x * 4 + 1] + row0[x1 * 4 + 1] + row1[x * 4 + 1] + row1[x1 * 4 + 1]) >> 2;
            int r = (row0[x * 4 + 2] + row0[x1 * 4 + 2] + row1[x * 4 + 2] + row1[x1 * 4 + 2]) >> 2;
            int u = ((-43 * r - 85 * g + 128 * b) >> 8) + 128;
            int v = ((128 * r - 107 * g - 21 * b) >> 8) + 128;
            uvrow[x] = (uint8_t)(u < 0 ? 0 : (u > 255 ? 255 : u));
            uvrow[x + 1] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
        }
    }
}

bool to_annexb(const uint8_t* data, DWORD size, std::vector<uint8_t>& out, bool& keyframe) {
    out.clear();
    keyframe = false;
    if (!data || size < 4) return false;

    auto note_nal = [&](const uint8_t* nal, DWORD nal_len) {
        if (nal_len == 0) return;
        if ((nal[0] & 0x1F) == 5) keyframe = true;
        out.push_back(0); out.push_back(0); out.push_back(0); out.push_back(1);
        out.insert(out.end(), nal, nal + nal_len);
    };

    if (data[0] == 0 && data[1] == 0 && (data[2] == 1 || (data[2] == 0 && data[3] == 1))) {
        out.assign(data, data + size);
        for (DWORD i = 0; i + 4 < size; ++i) {
            if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 && data[i + 3] == 1) {
                if ((data[i + 4] & 0x1F) == 5) keyframe = true;
            } else if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
                if ((data[i + 3] & 0x1F) == 5) keyframe = true;
            }
        }
        return true;
    }

    DWORD i = 0;
    while (i + 4 <= size) {
        uint32_t nal_len = (data[i] << 24) | (data[i + 1] << 16) | (data[i + 2] << 8) | data[i + 3];
        i += 4;
        if (nal_len == 0 || i + nal_len > size) return !out.empty();
        note_nal(data + i, nal_len);
        i += nal_len;
    }
    return !out.empty();
}

bool annexb_has_nal_type(const std::vector<uint8_t>& ab, uint8_t nal_type) {
    const uint8_t* data = ab.data();
    size_t size = ab.size();
    for (size_t i = 0; i + 4 < size; ++i) {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 && data[i + 3] == 1) {
            if ((data[i + 4] & 0x1F) == nal_type) return true;
            i += 3;
        } else if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            if ((data[i + 3] & 0x1F) == nal_type) return true;
            i += 2;
        }
    }
    return false;
}

// Rebuild cached SPS+PPS Annex-B from a packet that contains parameter sets.
void cache_sps_pps_from_annexb(const std::vector<uint8_t>& ab, std::vector<uint8_t>& sps_pps) {
    std::vector<uint8_t> sps, pps;
    const uint8_t* data = ab.data();
    size_t size = ab.size();
    size_t i = 0;
    while (i + 3 < size) {
        size_t sc = 0;
        if (i + 3 < size && data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 && data[i + 3] == 1)
            sc = 4;
        else if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)
            sc = 3;
        else { ++i; continue; }
        size_t nal_start = i + sc;
        size_t j = nal_start;
        while (j + 3 < size) {
            if (data[j] == 0 && data[j + 1] == 0 &&
                (data[j + 2] == 1 || (data[j + 2] == 0 && j + 3 < size && data[j + 3] == 1)))
                break;
            ++j;
        }
        if (nal_start < size) {
            uint8_t nt = data[nal_start] & 0x1F;
            if (nt == 7) {
                sps.assign(data + i, data + j);
            } else if (nt == 8) {
                pps.assign(data + i, data + j);
            }
        }
        i = j;
    }
    if (!sps.empty() && !pps.empty()) {
        sps_pps = sps;
        sps_pps.insert(sps_pps.end(), pps.begin(), pps.end());
    }
}

} // namespace

struct H264Encoder::Impl {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<IMFDXGIDeviceManager> dev_mgr;
    UINT reset_token = 0;
    bool own_device = false;

    ComPtr<IMFTransform> xform;
    ComPtr<IMFMediaEventGenerator> events; // async HW MFTs
    ComPtr<IMFMediaType> in_type;
    ComPtr<IMFMediaType> out_type;
    ComPtr<ICodecAPI> codec;

    ComPtr<ID3D11Texture2D> upload_bgra;   // CPU→GPU BGRA
    ComPtr<ID3D11Texture2D> nv12_tex;     // GPU NV12 for encoder
    ComPtr<ID3D11Texture2D> staging_bgra; // readback if VP unavailable
    ComPtr<ID3D11VideoDevice> video_dev;
    ComPtr<ID3D11VideoContext> video_ctx;
    ComPtr<ID3D11VideoProcessorEnumerator> vp_enum;
    ComPtr<ID3D11VideoProcessor> vp;
    ComPtr<ID3D11VideoProcessorInputView> vp_in;
    ComPtr<ID3D11VideoProcessorOutputView> vp_out;
    bool vp_ready = false;

    std::vector<uint8_t> nv12_cpu;
    std::vector<uint8_t> sps_pps; // Annex-B SPS+PPS cached for IDR prepend
    LONGLONG sample_time = 0;
    LONGLONG sample_duration = 0;
    int fps = 30;
    bool mf_started = false;
    bool force_key_pending = true;
    bool use_dxgi = false;
    bool async_mft = false;
    int need_input = 0; // queued METransformNeedInput credits
    GUID input_subtype = {};
};

H264Encoder::H264Encoder() = default;
H264Encoder::~H264Encoder() { shutdown(); }

void H264Encoder::shutdown() {
    if (impl_) {
        if (impl_->xform) {
            impl_->xform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
            impl_->xform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
            if (impl_->use_dxgi)
                impl_->xform->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, 0);
        }
        impl_->vp_in.Reset();
        impl_->vp_out.Reset();
        impl_->vp.Reset();
        impl_->vp_enum.Reset();
        impl_->video_ctx.Reset();
        impl_->video_dev.Reset();
        impl_->nv12_tex.Reset();
        impl_->upload_bgra.Reset();
        impl_->staging_bgra.Reset();
        impl_->xform.Reset();
        impl_->dev_mgr.Reset();
        if (impl_->mf_started) {
            MFShutdown();
            impl_->mf_started = false;
        }
        delete impl_;
        impl_ = nullptr;
    }
    ready_ = false;
    hardware_ = false;
    w_ = h_ = 0;
}

static bool create_d3d_device(ComPtr<ID3D11Device>& device, ComPtr<ID3D11DeviceContext>& ctx) {
    UINT flags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    D3D_FEATURE_LEVEL fl_out = D3D_FEATURE_LEVEL_11_0;
    const D3D_FEATURE_LEVEL fls[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1
    };
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                   fls, (UINT)_countof(fls), D3D11_SDK_VERSION,
                                   &device, &fl_out, &ctx);
    if (FAILED(hr)) {
        flags &= ~D3D11_CREATE_DEVICE_DEBUG;
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                               fls, (UINT)_countof(fls), D3D11_SDK_VERSION,
                               &device, &fl_out, &ctx);
    }
    if (FAILED(hr) || !device) return false;
    ComPtr<ID3D10Multithread> mt;
    if (SUCCEEDED(device.As(&mt)) && mt)
        mt->SetMultithreadProtected(TRUE);
    return true;
}

bool H264Encoder::init(int width, int height, int fps, int bitrate_kbps) {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    if (!create_d3d_device(device, ctx)) {
        LOG_ERROR("h264", "D3D11CreateDevice failed — cannot init encoder");
        return false;
    }
    bool ok = init(device.Get(), width, height, fps, bitrate_kbps);
    if (ok && impl_) impl_->own_device = true;
    return ok;
}

bool H264Encoder::init(ID3D11Device* device, int width, int height, int fps, int bitrate_kbps) {
    shutdown();
    if (!device) return false;
    width &= ~1;
    height &= ~1;
    if (width < 16 || height < 16) {
        LOG_ERROR("h264", "init: invalid size %dx%d", width, height);
        return false;
    }
    if (fps < 1) fps = 30;
    if (bitrate_kbps < 200) bitrate_kbps = 200;

    impl_ = new Impl();
    HRESULT hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) {
        LOG_ERROR("h264", "MFStartup failed hr=0x%08lx", hr);
        shutdown();
        return false;
    }
    impl_->mf_started = true;
    impl_->fps = fps;
    impl_->sample_duration = 10'000'000LL / fps;
    impl_->nv12_cpu.resize((size_t)width * height * 3 / 2);
    impl_->device = device;
    device->GetImmediateContext(&impl_->ctx);

    hr = MFCreateDXGIDeviceManager(&impl_->reset_token, &impl_->dev_mgr);
    if (FAILED(hr) || !impl_->dev_mgr) {
        LOG_ERROR("h264", "MFCreateDXGIDeviceManager hr=0x%08lx", hr);
        shutdown();
        return false;
    }
    hr = impl_->dev_mgr->ResetDevice(device, impl_->reset_token);
    if (FAILED(hr)) {
        LOG_ERROR("h264", "ResetDevice hr=0x%08lx", hr);
        shutdown();
        return false;
    }

    // GPU resources: BGRA upload + NV12 target
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = width;
    td.Height = height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device->CreateTexture2D(&td, nullptr, &impl_->upload_bgra))) {
        LOG_ERROR("h264", "CreateTexture2D BGRA failed");
        shutdown();
        return false;
    }
    td.Format = DXGI_FORMAT_NV12;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device->CreateTexture2D(&td, nullptr, &impl_->nv12_tex))) {
        // Some GPUs need NV12 without RT bind for encoder-only
        td.BindFlags = 0;
        td.MiscFlags = 0;
        if (FAILED(device->CreateTexture2D(&td, nullptr, &impl_->nv12_tex))) {
            LOG_WARN("h264", "CreateTexture2D NV12 failed — will use CPU NV12 + memory MFT path");
        }
    }

    // Optional Video Processor BGRA→NV12
    if (impl_->nv12_tex && SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&impl_->video_dev))) &&
        SUCCEEDED(impl_->ctx.As(&impl_->video_ctx)) && impl_->video_dev && impl_->video_ctx) {
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd = {};
        cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        cd.InputWidth = width;
        cd.InputHeight = height;
        cd.OutputWidth = width;
        cd.OutputHeight = height;
        cd.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
        if (SUCCEEDED(impl_->video_dev->CreateVideoProcessorEnumerator(&cd, &impl_->vp_enum)) &&
            impl_->vp_enum &&
            SUCCEEDED(impl_->video_dev->CreateVideoProcessor(impl_->vp_enum.Get(), 0, &impl_->vp))) {
            D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd = {};
            ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
            D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd = {};
            ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
            ComPtr<ID3D11VideoProcessorInputView> inv;
            ComPtr<ID3D11VideoProcessorOutputView> outv;
            if (SUCCEEDED(impl_->video_dev->CreateVideoProcessorInputView(
                    impl_->upload_bgra.Get(), impl_->vp_enum.Get(), &ivd, &inv)) &&
                SUCCEEDED(impl_->video_dev->CreateVideoProcessorOutputView(
                    impl_->nv12_tex.Get(), impl_->vp_enum.Get(), &ovd, &outv))) {
                impl_->vp_in = inv;
                impl_->vp_out = outv;
                impl_->vp_ready = true;
            }
        }
    }

    auto unlock_async_mft = [](IMFTransform* xform) -> bool {
        if (!xform) return false;
        ComPtr<IMFAttributes> attrs;
        if (FAILED(xform->GetAttributes(&attrs)) || !attrs) return true; // sync MFT
        UINT32 is_async = 0;
        if (SUCCEEDED(attrs->GetUINT32(MF_TRANSFORM_ASYNC, &is_async)) && is_async) {
            // Hardware encoders are async MFTs. Without this unlock, every
            // ProcessMessage (incl. SET_D3D_MANAGER) returns MF_E_TRANSFORM_ASYNC_LOCKED
            // (0xC00D6D77) — the failure seen in agent_20260716_233609.log.
            HRESULT uhr = attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
            if (FAILED(uhr)) {
                LOG_WARN("h264", "MF_TRANSFORM_ASYNC_UNLOCK hr=0x%08lx", uhr);
                return false;
            }
            LOG_DEBUG("h264", "async MFT unlocked");
        }
        return true;
    };

    auto mft_friendly = [](IMFActivate* act, char* buf, size_t buflen) {
        buf[0] = 0;
        if (!act || buflen < 2) return;
        WCHAR* name = nullptr;
        if (SUCCEEDED(act->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &name, nullptr)) && name) {
            WideCharToMultiByte(CP_UTF8, 0, name, -1, buf, (int)buflen, nullptr, nullptr);
            CoTaskMemFree(name);
        }
    };

    auto try_mft = [&](DWORD flags, bool hw, const char* label) -> bool {
        MFT_REGISTER_TYPE_INFO in_info = { MFMediaType_Video, MFVideoFormat_NV12 };
        MFT_REGISTER_TYPE_INFO out_info = { MFMediaType_Video, MFVideoFormat_H264 };
        IMFActivate** activates = nullptr;
        UINT32 count = 0;
        HRESULT ehr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags, &in_info, &out_info, &activates, &count);
        if (FAILED(ehr) || count == 0) {
            if (activates) CoTaskMemFree(activates);
            LOG_DEBUG("h264", "%s MFTEnumEx count=0 hr=0x%08lx", label, ehr);
            return false;
        }
        LOG("h264", "%s MFTEnumEx found %u candidate(s)", label, count);
        for (UINT32 i = 0; i < count; ++i) {
            char friendly[128] = {};
            mft_friendly(activates[i], friendly, sizeof(friendly));

            ComPtr<IMFTransform> xform;
            if (FAILED(activates[i]->ActivateObject(IID_PPV_ARGS(&xform))) || !xform) {
                LOG_DEBUG("h264", "%s #%u ActivateObject failed (%s)", label, i, friendly);
                continue;
            }
            if (!unlock_async_mft(xform.Get())) {
                LOG_DEBUG("h264", "%s #%u async unlock failed (%s)", label, i, friendly);
                continue;
            }

            if (hw) {
                ComPtr<IMFAttributes> attrs;
                UINT32 d3d11_aware = 0;
                if (SUCCEEDED(xform->GetAttributes(&attrs)) && attrs)
                    attrs->GetUINT32(MF_SA_D3D11_AWARE, &d3d11_aware);
                if (!d3d11_aware) {
                    LOG_DEBUG("h264", "%s #%u not D3D11-aware (%s)", label, i, friendly);
                    continue;
                }
                hr = xform->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,
                                           (ULONG_PTR)impl_->dev_mgr.Get());
                if (FAILED(hr)) {
                    LOG_WARN("h264", "%s #%u SET_D3D_MANAGER hr=0x%08lx (%s)",
                             label, i, hr, friendly);
                    continue;
                }
            }

            // Prefer Baseline (WebCodecs avc1.42E0xx); fall back to Main if needed.
            const UINT32 profiles[] = {
                (UINT32)eAVEncH264VProfile_Base,
                (UINT32)eAVEncH264VProfile_Main,
            };
            bool configured = false;
            for (UINT32 profile : profiles) {
                ComPtr<IMFMediaType> out_type;
                if (FAILED(MFCreateMediaType(&out_type))) break;
                out_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
                out_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
                MFSetAttributeSize(out_type.Get(), MF_MT_FRAME_SIZE, width, height);
                MFSetAttributeRatio(out_type.Get(), MF_MT_FRAME_RATE, fps, 1);
                MFSetAttributeRatio(out_type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
                out_type->SetUINT32(MF_MT_AVG_BITRATE, (UINT32)bitrate_kbps * 1000);
                out_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
                out_type->SetUINT32(MF_MT_MPEG2_PROFILE, profile);
                // Level 4.0 for 1080p30; some MFTs reject the attribute — retry without.
                out_type->SetUINT32(MF_MT_MPEG2_LEVEL, (UINT32)eAVEncH264VLevel4);

                hr = xform->SetOutputType(0, out_type.Get(), 0);
                if (FAILED(hr)) {
                    out_type->DeleteItem(MF_MT_MPEG2_LEVEL);
                    hr = xform->SetOutputType(0, out_type.Get(), 0);
                }
                if (FAILED(hr)) {
                    LOG_DEBUG("h264", "%s #%u SetOutputType profile=%u hr=0x%08lx",
                              label, i, profile, hr);
                    continue;
                }

                bool input_ok = false;
                ComPtr<IMFMediaType> avail;
                for (DWORD ti = 0; SUCCEEDED(xform->GetInputAvailableType(0, ti, &avail)); ++ti) {
                    GUID sub = {};
                    avail->GetGUID(MF_MT_SUBTYPE, &sub);
                    if (sub != MFVideoFormat_NV12) { avail.Reset(); continue; }
                    MFSetAttributeSize(avail.Get(), MF_MT_FRAME_SIZE, width, height);
                    MFSetAttributeRatio(avail.Get(), MF_MT_FRAME_RATE, fps, 1);
                    MFSetAttributeRatio(avail.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
                    avail->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
                    avail->SetUINT32(MF_MT_DEFAULT_STRIDE, (UINT32)width);
                    if (SUCCEEDED(xform->SetInputType(0, avail.Get(), 0))) {
                        impl_->in_type = avail;
                        impl_->input_subtype = sub;
                        input_ok = true;
                        break;
                    }
                    avail.Reset();
                }
                if (!input_ok) {
                    LOG_DEBUG("h264", "%s #%u SetInputType NV12 failed profile=%u", label, i, profile);
                    continue;
                }

                impl_->xform = xform;
                impl_->out_type = out_type;
                impl_->use_dxgi = hw;
                hardware_ = hw;
                LOG("h264", "using %s encoder #%u '%s' (DXGI=%d profile=%u) %dx%d @ %dfps %dkbps",
                    label, i, friendly[0] ? friendly : "?", (int)hw, profile,
                    width, height, fps, bitrate_kbps);
                configured = true;
                break;
            }
            if (!configured) continue;

            for (UINT32 j = 0; j < count; ++j) activates[j]->Release();
            CoTaskMemFree(activates);
            return true;
        }
        for (UINT32 j = 0; j < count; ++j) activates[j]->Release();
        CoTaskMemFree(activates);
        return false;
    };

    // Hardware DXGI first (核显 QSV / AMD VCN / NVENC), then software.
    bool ok = try_mft(MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, true, "hardware");
    if (!ok) {
        LOG_WARN("h264", "hardware H.264 MFT unavailable — trying software FALLBACK");
        ok = try_mft(
            MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER,
            false, "software");
    }
    if (!ok || !impl_->xform) {
        LOG_ERROR("h264", "no H.264 MFT available %dx%d", width, height);
        shutdown();
        return false;
    }

    impl_->codec.Reset();
    if (SUCCEEDED(impl_->xform.As(&impl_->codec)) && impl_->codec) {
        auto set_bool = [&](const GUID& g, bool b) {
            VARIANT v; VariantInit(&v); v.vt = VT_BOOL;
            v.boolVal = b ? VARIANT_TRUE : VARIANT_FALSE;
            impl_->codec->SetValue(&g, &v); VariantClear(&v);
        };
        auto set_u4 = [&](const GUID& g, ULONG u) {
            VARIANT v; VariantInit(&v); v.vt = VT_UI4; v.ulVal = u;
            impl_->codec->SetValue(&g, &v); VariantClear(&v);
        };
        set_bool(CODECAPI_AVLowLatencyMode, true);
        set_u4(CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR);
        set_u4(CODECAPI_AVEncCommonMeanBitRate, (ULONG)bitrate_kbps * 1000);
        // ~0.33s GOP → faster keyframe recovery after loss (B-frames off).
        ULONG gop = (ULONG)((fps > 2) ? (fps / 3) : 10);
        if (gop < 5) gop = 5;
        set_u4(CODECAPI_AVEncMPVGOPSize, gop);
        set_u4(CODECAPI_AVEncMPVDefaultBPictureCount, 0);
        // 0 = favor speed / lower latency on vendors that honor it.
        set_u4(CODECAPI_AVEncCommonQualityVsSpeed, 100);
    }

    // Detect async MFT (hardware encoders). After ASYNC_UNLOCK they still use
    // METransformNeedInput / METransformHaveOutput — sync ProcessOutput → E_UNEXPECTED.
    impl_->async_mft = false;
    impl_->need_input = 0;
    impl_->events.Reset();
    {
        ComPtr<IMFAttributes> attrs;
        UINT32 is_async = 0;
        if (SUCCEEDED(impl_->xform->GetAttributes(&attrs)) && attrs &&
            SUCCEEDED(attrs->GetUINT32(MF_TRANSFORM_ASYNC, &is_async)) && is_async) {
            if (SUCCEEDED(impl_->xform.As(&impl_->events)) && impl_->events) {
                impl_->async_mft = true;
                LOG("h264", "async event model enabled (NeedInput/HaveOutput)");
            }
        }
    }

    impl_->xform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    impl_->xform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    impl_->xform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    impl_->force_key_pending = true;
    w_ = width;
    h_ = height;
    ready_ = true;
    if (impl_->async_mft) {
        std::vector<H264Packet> warmup;
        pump_async_(warmup, 50); // collect initial METransformNeedInput credits
        LOG_DEBUG("h264", "async warmup need_input=%d", impl_->need_input);
    }
    if (hardware_)
        LOG("h264", "encoder ready HARDWARE (DXGI) %dx%d async=%d vp=%d",
            width, height, (int)impl_->async_mft, (int)impl_->vp_ready);
    else
        LOG_WARN("h264", "encoder ready SOFTWARE FALLBACK %dx%d", width, height);
    return true;
}

void H264Encoder::request_keyframe() {
    if (!ready_ || !impl_) return;
    impl_->force_key_pending = true;
}

bool H264Encoder::process_one_output_(std::vector<H264Packet>& out) {
    MFT_OUTPUT_STREAM_INFO info = {};
    impl_->xform->GetOutputStreamInfo(0, &info);
    ComPtr<IMFSample> out_sample;
    ComPtr<IMFMediaBuffer> out_buf;
    MFT_OUTPUT_DATA_BUFFER odb = {};
    odb.dwStreamID = 0;
    bool need_provide = !(info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES);
    if (need_provide) {
        DWORD sz = info.cbSize ? info.cbSize : (DWORD)(w_ * h_);
        if (FAILED(MFCreateSample(&out_sample))) return false;
        if (FAILED(MFCreateMemoryBuffer(sz, &out_buf))) return false;
        out_sample->AddBuffer(out_buf.Get());
        odb.pSample = out_sample.Get();
    }
    DWORD status = 0;
    HRESULT hr = impl_->xform->ProcessOutput(0, 1, &odb, &status);
    if (odb.pEvents) { odb.pEvents->Release(); odb.pEvents = nullptr; }
    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return false;
    if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
        ComPtr<IMFMediaType> mt;
        impl_->xform->GetOutputAvailableType(0, 0, &mt);
        if (mt) impl_->xform->SetOutputType(0, mt.Get(), 0);
        return process_one_output_(out);
    }
    if (FAILED(hr)) {
        LOG_WARN("h264", "ProcessOutput hr=0x%08lx", hr);
        return false;
    }
    IMFSample* s = odb.pSample ? odb.pSample : out_sample.Get();
    if (!s) return false;
    ComPtr<IMFMediaBuffer> contiguous;
    if (FAILED(s->ConvertToContiguousBuffer(&contiguous)) || !contiguous) {
        if (odb.pSample && !need_provide) odb.pSample->Release();
        return false;
    }
    BYTE* data = nullptr;
    DWORD len = 0;
    if (FAILED(contiguous->Lock(&data, nullptr, &len)) || !data || len == 0) {
        if (odb.pSample && !need_provide) odb.pSample->Release();
        return false;
    }
    H264Packet pkt;
    pkt.w = w_;
    pkt.h = h_;
    bool ok = to_annexb(data, len, pkt.annexb, pkt.keyframe);
    contiguous->Unlock();
    if (odb.pSample && !need_provide) odb.pSample->Release();
    if (ok) {
        if (annexb_has_nal_type(pkt.annexb, 7) || annexb_has_nal_type(pkt.annexb, 8))
            cache_sps_pps_from_annexb(pkt.annexb, impl_->sps_pps);
        // Decoder reconfigure / joiners need SPS+PPS before IDR (parity with Android).
        if (pkt.keyframe && !impl_->sps_pps.empty() && !annexb_has_nal_type(pkt.annexb, 7)) {
            std::vector<uint8_t> merged = impl_->sps_pps;
            merged.insert(merged.end(), pkt.annexb.begin(), pkt.annexb.end());
            pkt.annexb = std::move(merged);
        }
        pkt.ts_ms = (uint32_t)(GetTickCount64() & 0xffffffffu);
        out.push_back(std::move(pkt));
    }
    return ok;
}

bool H264Encoder::drain_output_(std::vector<H264Packet>& out) {
    // Sync MFTs: pull until NEED_MORE_INPUT.
    for (;;) {
        size_t before = out.size();
        if (!process_one_output_(out)) break;
        if (out.size() == before) break;
    }
    return true;
}

/// Pump async MFT events. Returns true if at least one output packet was produced.
bool H264Encoder::pump_async_(std::vector<H264Packet>& out, int timeout_ms) {
    if (!impl_->events) return false;
    ULONGLONG deadline = GetTickCount64() + (ULONGLONG)(timeout_ms > 0 ? timeout_ms : 0);
    bool got_out = false;
    for (;;) {
        ComPtr<IMFMediaEvent> ev;
        HRESULT hr = impl_->events->GetEvent(MF_EVENT_FLAG_NO_WAIT, &ev);
        if (hr == MF_E_NO_EVENTS_AVAILABLE) {
            if (got_out) break;
            if (GetTickCount64() >= deadline) break;
            Sleep(1);
            continue;
        }
        if (FAILED(hr) || !ev) break;
        MediaEventType type = MEUnknown;
        ev->GetType(&type);
        if (type == METransformNeedInput) {
            impl_->need_input++;
        } else if (type == METransformHaveOutput) {
            if (process_one_output_(out)) got_out = true;
        } else if (type == MEError) {
            HRESULT status = S_OK;
            ev->GetStatus(&status);
            LOG_WARN("h264", "MEError status=0x%08lx", status);
            break;
        }
    }
    return got_out;
}

bool H264Encoder::feed_nv12_and_drain_(std::vector<H264Packet>& out) {
    if (impl_->force_key_pending && impl_->codec) {
        VARIANT v;
        VariantInit(&v); v.vt = VT_UI4; v.ulVal = 1;
        impl_->codec->SetValue(&CODECAPI_AVEncVideoForceKeyFrame, &v);
        VariantClear(&v);
        impl_->force_key_pending = false;
    }

    ComPtr<IMFSample> sample;
    if (FAILED(MFCreateSample(&sample))) return false;

    HRESULT hr;
    if (impl_->use_dxgi && impl_->nv12_tex) {
        bool have_nv12 = false;
        if (impl_->vp_ready && impl_->vp && impl_->vp_in && impl_->vp_out) {
            D3D11_VIDEO_PROCESSOR_STREAM stream = {};
            stream.Enable = TRUE;
            stream.pInputSurface = impl_->vp_in.Get();
            hr = impl_->video_ctx->VideoProcessorBlt(impl_->vp.Get(), impl_->vp_out.Get(), 0, 1, &stream);
            have_nv12 = SUCCEEDED(hr);
        }
        if (!have_nv12) {
            D3D11_TEXTURE2D_DESC stdesc = {};
            impl_->upload_bgra->GetDesc(&stdesc);
            stdesc.Usage = D3D11_USAGE_STAGING;
            stdesc.BindFlags = 0;
            stdesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            stdesc.MiscFlags = 0;
            if (!impl_->staging_bgra) {
                if (FAILED(impl_->device->CreateTexture2D(&stdesc, nullptr, &impl_->staging_bgra)))
                    return false;
            }
            impl_->ctx->CopyResource(impl_->staging_bgra.Get(), impl_->upload_bgra.Get());
            D3D11_MAPPED_SUBRESOURCE mapped = {};
            if (FAILED(impl_->ctx->Map(impl_->staging_bgra.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
                return false;
            bgra_to_nv12((const uint8_t*)mapped.pData, (int)mapped.RowPitch / 4, w_, h_, impl_->nv12_cpu.data());
            impl_->ctx->Unmap(impl_->staging_bgra.Get(), 0);

            // Staging NV12 write then GPU copy (UpdateSubresource on NV12 is unreliable).
            D3D11_TEXTURE2D_DESC nd = {};
            impl_->nv12_tex->GetDesc(&nd);
            nd.Usage = D3D11_USAGE_STAGING;
            nd.BindFlags = 0;
            nd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            nd.MiscFlags = 0;
            ComPtr<ID3D11Texture2D> st_nv12;
            if (FAILED(impl_->device->CreateTexture2D(&nd, nullptr, &st_nv12))) return false;
            D3D11_MAPPED_SUBRESOURCE nm = {};
            if (FAILED(impl_->ctx->Map(st_nv12.Get(), 0, D3D11_MAP_WRITE, 0, &nm))) return false;
            for (int y = 0; y < h_; ++y)
                memcpy((uint8_t*)nm.pData + y * nm.RowPitch, impl_->nv12_cpu.data() + (size_t)y * w_, w_);
            uint8_t* uv_dst = (uint8_t*)nm.pData + nm.RowPitch * h_;
            const uint8_t* uv_src = impl_->nv12_cpu.data() + (size_t)w_ * h_;
            for (int y = 0; y < h_ / 2; ++y)
                memcpy(uv_dst + y * nm.RowPitch, uv_src + (size_t)y * w_, w_);
            impl_->ctx->Unmap(st_nv12.Get(), 0);
            impl_->ctx->CopyResource(impl_->nv12_tex.Get(), st_nv12.Get());
        }

        ComPtr<IMFMediaBuffer> buf;
        hr = MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), impl_->nv12_tex.Get(), 0, FALSE, &buf);
        if (FAILED(hr)) {
            LOG_WARN("h264", "MFCreateDXGISurfaceBuffer hr=0x%08lx", hr);
            return false;
        }
        sample->AddBuffer(buf.Get());
    } else {
        D3D11_TEXTURE2D_DESC stdesc = {};
        impl_->upload_bgra->GetDesc(&stdesc);
        stdesc.Usage = D3D11_USAGE_STAGING;
        stdesc.BindFlags = 0;
        stdesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (!impl_->staging_bgra) {
            if (FAILED(impl_->device->CreateTexture2D(&stdesc, nullptr, &impl_->staging_bgra)))
                return false;
        }
        impl_->ctx->CopyResource(impl_->staging_bgra.Get(), impl_->upload_bgra.Get());
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (FAILED(impl_->ctx->Map(impl_->staging_bgra.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
            return false;
        bgra_to_nv12((const uint8_t*)mapped.pData, (int)mapped.RowPitch / 4, w_, h_, impl_->nv12_cpu.data());
        impl_->ctx->Unmap(impl_->staging_bgra.Get(), 0);

        ComPtr<IMFMediaBuffer> buf;
        DWORD nv12_size = (DWORD)(w_ * h_ * 3 / 2);
        if (FAILED(MFCreateMemoryBuffer(nv12_size, &buf))) return false;
        BYTE* dst = nullptr;
        if (FAILED(buf->Lock(&dst, nullptr, nullptr))) return false;
        memcpy(dst, impl_->nv12_cpu.data(), nv12_size);
        buf->Unlock();
        buf->SetCurrentLength(nv12_size);
        sample->AddBuffer(buf.Get());
    }

    sample->SetSampleTime(impl_->sample_time);
    sample->SetSampleDuration(impl_->sample_duration);
    impl_->sample_time += impl_->sample_duration;

    if (impl_->async_mft) {
        // Wait for a NeedInput credit (AMD/Intel/NVENC async HW MFTs).
        if (impl_->need_input <= 0) {
            pump_async_(out, 8); // may also collect prior HaveOutput
        }
        if (impl_->need_input <= 0) {
            // Still no slot — drop this frame to keep latency bounded.
            return !out.empty();
        }
        hr = impl_->xform->ProcessInput(0, sample.Get(), 0);
        if (FAILED(hr) && hr != MF_E_NOTACCEPTING) {
            LOG_WARN("h264", "ProcessInput hr=0x%08lx", hr);
            return false;
        }
        if (SUCCEEDED(hr)) impl_->need_input--;
        // Pull encoded NALs (low-latency: wait up to ~one frame).
        pump_async_(out, 20);
        return !out.empty();
    }

    hr = impl_->xform->ProcessInput(0, sample.Get(), 0);
    if (FAILED(hr) && hr != MF_E_NOTACCEPTING) {
        LOG_WARN("h264", "ProcessInput hr=0x%08lx", hr);
        return false;
    }
    return drain_output_(out);
}

bool H264Encoder::encode_texture(ID3D11Texture2D* bgra_tex, int src_w, int src_h,
                                 std::vector<H264Packet>& out) {
    out.clear();
    if (!ready_ || !impl_ || !bgra_tex) return false;
    int ew = src_w & ~1, eh = src_h & ~1;
    if (ew != w_ || eh != h_) {
        ComPtr<ID3D11Device> dev = impl_->device;
        int fps = impl_->fps;
        if (!init(dev.Get(), ew, eh, fps)) return false;
    }
    if (bgra_tex != impl_->upload_bgra.Get())
        impl_->ctx->CopyResource(impl_->upload_bgra.Get(), bgra_tex);
    return feed_nv12_and_drain_(out);
}

bool H264Encoder::encode_bgra(const uint8_t* bgra, int src_w, int src_h, std::vector<H264Packet>& out) {
    out.clear();
    if (!ready_ || !impl_ || !bgra) return false;
    int ew = src_w & ~1, eh = src_h & ~1;
    if (ew != w_ || eh != h_) {
        ComPtr<ID3D11Device> dev = impl_->device;
        int fps = impl_->fps;
        if (dev) {
            if (!init(dev.Get(), ew, eh, fps)) return false;
        } else if (!init(ew, eh, fps)) {
            return false;
        }
    }
    impl_->ctx->UpdateSubresource(impl_->upload_bgra.Get(), 0, nullptr, bgra, (UINT)(src_w * 4), 0);
    return feed_nv12_and_drain_(out);
}
