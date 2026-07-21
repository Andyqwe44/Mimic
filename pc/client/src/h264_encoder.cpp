/**
 * h264_encoder.cpp — MF H.264: DXGI hardware MFT first, software last resort.
 *
 * Phase-1: 3-slot BGRA/NV12 ring. MFT_IN_FLIGHT slots are never GPU-written.
 * Slot FREE only on proven IMFTrackedSample final-release (not HaveOutput).
 */
#include "h264_encoder.h"
#include "../../logger/logger.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <d3d11_3.h>
#include <d3d11_4.h>
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
#include <mutex>
#include <atomic>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "wmcodecdspuuid.lib")

using Microsoft::WRL::ComPtr;

namespace {

constexpr int kTexPool = 3;

enum class TexSlotState : int {
    Free = 0,
    GpuProcessing = 1,
    MftInFlight = 2,
};

ULONG peek_refcount(IUnknown* p) {
    if (!p) return 0;
    ULONG after_add = p->AddRef();
    p->Release();
    return after_add > 0 ? after_add - 1 : 0;
}

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
        uint8_t profile = 0, level = 0;
        if (sps.size() >= 5) {
            size_t hdr = (sps.size() >= 4 && sps[0] == 0 && sps[1] == 0 && sps[2] == 0 && sps[3] == 1) ? 4u
                : (sps.size() >= 3 && sps[0] == 0 && sps[1] == 0 && sps[2] == 1) ? 3u : 0u;
            if (hdr && sps.size() >= hdr + 4) {
                profile = sps[hdr + 1];
                level = sps[hdr + 3];
            }
        }
        LOG("h264", "cached SPS+PPS total=%zu sps=%zu pps=%zu profile_idc=%u level_idc=%u",
            sps_pps.size(), sps.size(), pps.size(), (unsigned)profile, (unsigned)level);
    }
}

} // namespace

struct H264Encoder::Impl;

// IMFTrackedSample final-release → proven slot FREE (never HaveOutput).
struct TrackedReleaseCb : public IMFAsyncCallback {
    H264Encoder::Impl* impl = nullptr;
    LONG refs = 1;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFAsyncCallback)) {
            *ppv = static_cast<IMFAsyncCallback*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&refs); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG n = InterlockedDecrement(&refs);
        if (n == 0) delete this;
        return (ULONG)n;
    }
    HRESULT STDMETHODCALLTYPE GetParameters(DWORD* flags, DWORD* queue) override {
        if (flags) *flags = 0;
        if (queue) *queue = MFASYNC_CALLBACK_QUEUE_MULTITHREADED;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Invoke(IMFAsyncResult* result) override;
};

struct TexSlot {
    ComPtr<ID3D11Texture2D> upload_bgra;
    ComPtr<ID3D11Texture2D> nv12;
    ComPtr<ID3D11VideoProcessorInputView> vp_in;
    ComPtr<ID3D11VideoProcessorOutputView> vp_out;
    /// Non-owning identity pointer while MFT_IN_FLIGHT (no ComPtr — avoids false Release).
    IMFSample* sample_raw = nullptr;
    uint32_t frame_id = 0;
    uint32_t capture_id = 0;
    int slot_id = -1;
    LONGLONG sample_time = 0;
    uint64_t process_input_ms = 0;
    uint64_t release_ms = 0;
    bool release_proven = false;
    bool tracked_set = false;
    TexSlotState state = TexSlotState::Free;
};

struct H264Encoder::Impl {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11DeviceContext4> ctx4;
    ComPtr<ID3D11Fence> fence;
    UINT64 fence_value = 0;
    ComPtr<ID3D11Query> event_query;
    bool gpu_sync_ready = false;

    ComPtr<IMFDXGIDeviceManager> dev_mgr;
    UINT reset_token = 0;
    bool own_device = false;

    ComPtr<IMFTransform> xform;
    ComPtr<IMFMediaEventGenerator> events;
    ComPtr<IMFMediaType> in_type;
    ComPtr<IMFMediaType> out_type;
    ComPtr<ICodecAPI> codec;

    TexSlot slots[kTexPool];
    std::mutex slot_mtx;
    int mft_in_flight = 0;
    bool tracked_reliable = true; // flipped false if SetAllocator fails or callbacks never arrive
    uint32_t submits_since_proven = 0;
    uint32_t proven_releases = 0;

    ComPtr<ID3D11Texture2D> staging_bgra;
    ComPtr<ID3D11VideoDevice> video_dev;
    ComPtr<ID3D11VideoContext> video_ctx;
    ComPtr<ID3D11VideoProcessorEnumerator> vp_enum;
    ComPtr<ID3D11VideoProcessor> vp;
    bool vp_ready = false;

    std::vector<uint8_t> nv12_cpu;
    std::vector<uint8_t> sps_pps;
    LONGLONG sample_time = 0;
    LONGLONG sample_duration = 0;
    int fps = 30;
    bool mf_started = false;
    bool force_key_pending = true;
    bool use_dxgi = false;
    bool async_mft = false;
    int need_input = 0;
    GUID input_subtype = {};
    uint32_t next_submit_id = 1;
    uint32_t mft_output_seq = 0;

    void on_proven_release(IMFSample* sample);
    int count_free_unlocked() const;
};

HRESULT TrackedReleaseCb::Invoke(IMFAsyncResult* result) {
    if (!impl || !result) return E_POINTER;
    ComPtr<IUnknown> unk;
    result->GetObject(&unk);
    ComPtr<IMFSample> sample;
    if (unk) unk.As(&sample);
    if (sample)
        impl->on_proven_release(sample.Get());
    else
        LOG_WARN("h264", "sample_life PROVEN_RELEASE missing sample object");
    return S_OK;
}

void H264Encoder::Impl::on_proven_release(IMFSample* sample) {
    if (!sample) return;
    std::lock_guard<std::mutex> lk(slot_mtx);
    for (int i = 0; i < kTexPool; ++i) {
        TexSlot& s = slots[i];
        if (s.sample_raw != sample)
            continue;
        if (s.state != TexSlotState::MftInFlight) {
            LOG_WARN("h264",
                     "sample_life PROVEN_RELEASE unexpected state=%d sample=%p frame=%u slot=%d",
                     (int)s.state, (void*)sample, s.frame_id, s.slot_id);
        }
        s.release_ms = GetTickCount64();
        s.release_proven = true;
        proven_releases++;
        submits_since_proven = 0;
        LOG("h264",
            "sample_life PROVEN_RELEASE sample=%p frame=%u slot=%d capture=%u "
            "t_in=%llu t_rel=%llu hold_ms=%llu ref≈%lu in_flight=%d",
            (void*)sample, s.frame_id, s.slot_id, s.capture_id,
            (unsigned long long)s.process_input_ms, (unsigned long long)s.release_ms,
            (unsigned long long)(s.release_ms - s.process_input_ms),
            (unsigned long)peek_refcount(sample), mft_in_flight);
        s.sample_raw = nullptr;
        s.state = TexSlotState::Free;
        s.tracked_set = false;
        if (mft_in_flight > 0) mft_in_flight--;
        // TrackedSample hands sample back to allocator — release the returned ref.
        sample->Release();
        return;
    }
    LOG_WARN("h264", "sample_life PROVEN_RELEASE unmatched sample=%p ref≈%lu",
             (void*)sample, (unsigned long)peek_refcount(sample));
    sample->Release();
}

int H264Encoder::Impl::count_free_unlocked() const {
    int n = 0;
    for (int i = 0; i < kTexPool; ++i)
        if (slots[i].state == TexSlotState::Free) ++n;
    return n;
}

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
        {
            std::lock_guard<std::mutex> lk(impl_->slot_mtx);
            for (int i = 0; i < kTexPool; ++i) {
                TexSlot& s = impl_->slots[i];
                if (s.state == TexSlotState::MftInFlight) {
                    LOG_WARN("h264",
                             "shutdown force-drop MFT_IN_FLIGHT sample=%p frame=%u slot=%d "
                             "(no proven release)",
                             (void*)s.sample_raw, s.frame_id, s.slot_id);
                }
                s.sample_raw = nullptr;
                s.state = TexSlotState::Free;
                s.vp_in.Reset();
                s.vp_out.Reset();
                s.nv12.Reset();
                s.upload_bgra.Reset();
            }
            impl_->mft_in_flight = 0;
        }
        impl_->vp.Reset();
        impl_->vp_enum.Reset();
        impl_->video_ctx.Reset();
        impl_->video_dev.Reset();
        impl_->staging_bgra.Reset();
        impl_->event_query.Reset();
        impl_->fence.Reset();
        impl_->ctx4.Reset();
        impl_->xform.Reset();
        impl_->events.Reset();
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

    // GPU sync: fence preferred, QUERY_EVENT fallback (does not free slots).
    if (SUCCEEDED(impl_->ctx.As(&impl_->ctx4)) && impl_->ctx4) {
        ComPtr<ID3D11Device5> dev5;
        if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dev5))) && dev5) {
            if (SUCCEEDED(dev5->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&impl_->fence))))
                impl_->gpu_sync_ready = true;
        }
    }
    if (!impl_->gpu_sync_ready) {
        D3D11_QUERY_DESC qd = {};
        qd.Query = D3D11_QUERY_EVENT;
        if (SUCCEEDED(device->CreateQuery(&qd, &impl_->event_query)) && impl_->event_query)
            impl_->gpu_sync_ready = true;
    }
    LOG("h264", "gpu_sync=%s",
        impl_->fence ? "ID3D11Fence" : (impl_->event_query ? "QUERY_EVENT" : "none"));

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

    // 3-slot BGRA + NV12 pool
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = width;
    td.Height = height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    bool any_nv12 = false;
    for (int i = 0; i < kTexPool; ++i) {
        TexSlot& s = impl_->slots[i];
        s.slot_id = i;
        s.state = TexSlotState::Free;
        if (FAILED(device->CreateTexture2D(&td, nullptr, &s.upload_bgra))) {
            LOG_ERROR("h264", "CreateTexture2D BGRA slot=%d failed", i);
            shutdown();
            return false;
        }
        D3D11_TEXTURE2D_DESC nd = td;
        nd.Format = DXGI_FORMAT_NV12;
        nd.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(device->CreateTexture2D(&nd, nullptr, &s.nv12))) {
            nd.BindFlags = 0;
            if (FAILED(device->CreateTexture2D(&nd, nullptr, &s.nv12))) {
                LOG_WARN("h264", "CreateTexture2D NV12 slot=%d failed", i);
                s.nv12.Reset();
            }
        }
        if (s.nv12) any_nv12 = true;
    }
    if (!any_nv12)
        LOG_WARN("h264", "no NV12 textures — CPU NV12 + memory MFT path");

    if (any_nv12 && SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&impl_->video_dev))) &&
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
            int ok_views = 0;
            for (int i = 0; i < kTexPool; ++i) {
                TexSlot& s = impl_->slots[i];
                if (!s.nv12 || !s.upload_bgra) continue;
                ComPtr<ID3D11VideoProcessorInputView> inv;
                ComPtr<ID3D11VideoProcessorOutputView> outv;
                if (SUCCEEDED(impl_->video_dev->CreateVideoProcessorInputView(
                        s.upload_bgra.Get(), impl_->vp_enum.Get(), &ivd, &inv)) &&
                    SUCCEEDED(impl_->video_dev->CreateVideoProcessorOutputView(
                        s.nv12.Get(), impl_->vp_enum.Get(), &ovd, &outv))) {
                    s.vp_in = inv;
                    s.vp_out = outv;
                    ok_views++;
                }
            }
            impl_->vp_ready = ok_views == kTexPool;
            LOG("h264", "VP views ready=%d/%d", ok_views, kTexPool);
        }
    }

    auto unlock_async_mft = [](IMFTransform* xform) -> bool {
        if (!xform) return false;
        ComPtr<IMFAttributes> attrs;
        if (FAILED(xform->GetAttributes(&attrs)) || !attrs) return true;
        UINT32 is_async = 0;
        if (SUCCEEDED(attrs->GetUINT32(MF_TRANSFORM_ASYNC, &is_async)) && is_async) {
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
                LOG("h264", "using %s encoder #%u '%s' (DXGI=%d profile=%u) %dx%d @ %dfps %dkbps pool=%d",
                    label, i, friendly[0] ? friendly : "?", (int)hw, profile,
                    width, height, fps, bitrate_kbps, kTexPool);
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
        ULONG gop = (ULONG)((fps > 2) ? (fps / 3) : 10);
        if (gop < 5) gop = 5;
        set_u4(CODECAPI_AVEncMPVGOPSize, gop);
        set_u4(CODECAPI_AVEncMPVDefaultBPictureCount, 0);
        set_u4(CODECAPI_AVEncCommonQualityVsSpeed, 100);
    }

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
        pump_async_(warmup, 50);
        LOG_DEBUG("h264", "async warmup need_input=%d", impl_->need_input);
    }
    if (hardware_)
        LOG("h264", "encoder ready HARDWARE (DXGI) %dx%d async=%d vp=%d pool=%d",
            width, height, (int)impl_->async_mft, (int)impl_->vp_ready, kTexPool);
    else
        LOG_WARN("h264", "encoder ready SOFTWARE FALLBACK %dx%d pool=%d", width, height, kTexPool);
    return true;
}

void H264Encoder::request_keyframe() {
    if (!ready_ || !impl_) return;
    impl_->force_key_pending = true;
}

int H264Encoder::acquire_free_slot_() {
    std::lock_guard<std::mutex> lk(impl_->slot_mtx);
    // Conservative: if TrackedSample appears unreliable and all proven stalled, keep holding.
    if (!impl_->tracked_reliable && impl_->submits_since_proven > (uint32_t)kTexPool * 4) {
        LOG_WARN("h264", "tracked_release unreliable — refusing early reuse (free=%d in_flight=%d)",
                 impl_->count_free_unlocked(), impl_->mft_in_flight);
    }
    for (int i = 0; i < kTexPool; ++i) {
        if (impl_->slots[i].state == TexSlotState::Free) {
            impl_->slots[i].state = TexSlotState::GpuProcessing;
            impl_->slots[i].release_proven = false;
            impl_->slots[i].sample_raw = nullptr;
            return i;
        }
    }
    return -1;
}

void H264Encoder::release_slot_prep_fail_(int slot_id) {
    if (slot_id < 0 || slot_id >= kTexPool) return;
    std::lock_guard<std::mutex> lk(impl_->slot_mtx);
    TexSlot& s = impl_->slots[slot_id];
    s.sample_raw = nullptr;
    s.state = TexSlotState::Free;
    s.tracked_set = false;
}

bool H264Encoder::wait_gpu_idle_() {
    if (!impl_ || !impl_->ctx) return true;
    if (impl_->fence && impl_->ctx4) {
        impl_->fence_value++;
        HRESULT hr = impl_->ctx4->Signal(impl_->fence.Get(), impl_->fence_value);
        if (FAILED(hr)) return false;
        if (impl_->fence->GetCompletedValue() >= impl_->fence_value) return true;
        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!ev) return false;
        hr = impl_->fence->SetEventOnCompletion(impl_->fence_value, ev);
        if (SUCCEEDED(hr))
            WaitForSingleObject(ev, 100);
        CloseHandle(ev);
        return true;
    }
    if (impl_->event_query) {
        impl_->ctx->End(impl_->event_query.Get());
        BOOL done = FALSE;
        for (int i = 0; i < 1000; ++i) {
            HRESULT hr = impl_->ctx->GetData(impl_->event_query.Get(), &done, sizeof(done), 0);
            if (hr == S_OK) return true;
            if (FAILED(hr)) break;
            Sleep(0);
        }
    }
    return true;
}

bool H264Encoder::gpu_write_slot_(int slot_id, ID3D11Texture2D* src_bgra, const uint8_t* cpu_bgra,
                                  int src_w) {
    if (!impl_ || slot_id < 0 || slot_id >= kTexPool) return false;
    TexSlot* slot = nullptr;
    {
        std::lock_guard<std::mutex> lk(impl_->slot_mtx);
        TexSlot& s = impl_->slots[slot_id];
        if (s.state == TexSlotState::MftInFlight) {
            LOG_ERROR("h264", "sample_life BLOCK_GPU slot=%d state=MFT_IN_FLIGHT frame=%u — refuse write",
                      slot_id, s.frame_id);
            return false;
        }
        if (s.state != TexSlotState::GpuProcessing) {
            LOG_ERROR("h264", "sample_life BLOCK_GPU slot=%d unexpected state=%d",
                      slot_id, (int)s.state);
            return false;
        }
        slot = &s;
    }
    // Textures stable while GpuProcessing (only this thread writes).
    TexSlot& s = impl_->slots[slot_id];
    if (!s.upload_bgra) return false;

    if (src_bgra) {
        if (src_bgra != s.upload_bgra.Get())
            impl_->ctx->CopyResource(s.upload_bgra.Get(), src_bgra);
    } else if (cpu_bgra) {
        impl_->ctx->UpdateSubresource(s.upload_bgra.Get(), 0, nullptr, cpu_bgra,
                                      (UINT)(src_w * 4), 0);
    } else {
        return false;
    }

    bool have_nv12 = false;
    if (impl_->vp_ready && impl_->vp && s.vp_in && s.vp_out && s.nv12) {
        D3D11_VIDEO_PROCESSOR_STREAM stream = {};
        stream.Enable = TRUE;
        stream.pInputSurface = s.vp_in.Get();
        HRESULT hr = impl_->video_ctx->VideoProcessorBlt(impl_->vp.Get(), s.vp_out.Get(), 0, 1, &stream);
        have_nv12 = SUCCEEDED(hr);
    }
    if (!have_nv12) {
        if (!s.nv12) return false;
        D3D11_TEXTURE2D_DESC stdesc = {};
        s.upload_bgra->GetDesc(&stdesc);
        stdesc.Usage = D3D11_USAGE_STAGING;
        stdesc.BindFlags = 0;
        stdesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        stdesc.MiscFlags = 0;
        if (!impl_->staging_bgra) {
            if (FAILED(impl_->device->CreateTexture2D(&stdesc, nullptr, &impl_->staging_bgra)))
                return false;
        }
        impl_->ctx->CopyResource(impl_->staging_bgra.Get(), s.upload_bgra.Get());
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (FAILED(impl_->ctx->Map(impl_->staging_bgra.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
            return false;
        bgra_to_nv12((const uint8_t*)mapped.pData, (int)mapped.RowPitch / 4, w_, h_, impl_->nv12_cpu.data());
        impl_->ctx->Unmap(impl_->staging_bgra.Get(), 0);

        D3D11_TEXTURE2D_DESC nd = {};
        s.nv12->GetDesc(&nd);
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
        impl_->ctx->CopyResource(s.nv12.Get(), st_nv12.Get());
    }

    wait_gpu_idle_();
    (void)slot;
    return true;
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

    LONGLONG out_time = 0;
    s->GetSampleTime(&out_time);
    uint32_t matched_frame = 0;
    int matched_slot = -1;
    {
        std::lock_guard<std::mutex> lk(impl_->slot_mtx);
        for (int i = 0; i < kTexPool; ++i) {
            if (impl_->slots[i].state == TexSlotState::MftInFlight &&
                impl_->slots[i].sample_time == out_time) {
                matched_frame = impl_->slots[i].frame_id;
                matched_slot = i;
                break;
            }
        }
    }
    impl_->mft_output_seq++;
    LOG("h264", "mft_output_id=%u frame=%u slot=%d sample_time=%lld (HaveOutput — slot NOT freed)",
        impl_->mft_output_seq, matched_frame, matched_slot, (long long)out_time);

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
    for (;;) {
        size_t before = out.size();
        if (!process_one_output_(out)) break;
        if (out.size() == before) break;
    }
    return true;
}

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
            // Pull NALs only — never free texture slots here.
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

bool H264Encoder::feed_nv12_and_drain_(int slot_id, uint32_t capture_id, uint32_t submit_id,
                                       std::vector<H264Packet>& out) {
    if (slot_id < 0 || slot_id >= kTexPool) return false;
    TexSlot& slot = impl_->slots[slot_id];

    if (impl_->force_key_pending && impl_->codec) {
        VARIANT v;
        VariantInit(&v); v.vt = VT_UI4; v.ulVal = 1;
        impl_->codec->SetValue(&CODECAPI_AVEncVideoForceKeyFrame, &v);
        VariantClear(&v);
        impl_->force_key_pending = false;
    }

    ComPtr<IMFSample> sample;
    bool tracked = false;
    {
        ComPtr<IMFTrackedSample> tracked_sample;
        if (SUCCEEDED(MFCreateTrackedSample(&tracked_sample)) && tracked_sample) {
            if (SUCCEEDED(tracked_sample.As(&sample)) && sample)
                tracked = true;
        }
        if (!sample) {
            if (FAILED(MFCreateSample(&sample)) || !sample) {
                release_slot_prep_fail_(slot_id);
                return false;
            }
            LOG_WARN("h264", "MFCreateTrackedSample unavailable — conservative hold (no early FREE)");
            impl_->tracked_reliable = false;
        }
    }

    HRESULT hr;
    if (impl_->use_dxgi && slot.nv12) {
        ComPtr<IMFMediaBuffer> buf;
        hr = MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), slot.nv12.Get(), 0, FALSE, &buf);
        if (FAILED(hr)) {
            LOG_WARN("h264", "MFCreateDXGISurfaceBuffer hr=0x%08lx", hr);
            release_slot_prep_fail_(slot_id);
            return false;
        }
        sample->AddBuffer(buf.Get());
    } else {
        // Memory path: still bind ownership to slot (no GPU overwrite of in-flight DXGI).
        if (!slot.nv12) {
            // CPU already filled nv12_cpu in gpu_write via staging path requiring nv12 —
            // if no nv12 tex, convert from upload via staging.
            D3D11_TEXTURE2D_DESC stdesc = {};
            slot.upload_bgra->GetDesc(&stdesc);
            stdesc.Usage = D3D11_USAGE_STAGING;
            stdesc.BindFlags = 0;
            stdesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            stdesc.MiscFlags = 0;
            if (!impl_->staging_bgra) {
                if (FAILED(impl_->device->CreateTexture2D(&stdesc, nullptr, &impl_->staging_bgra))) {
                    release_slot_prep_fail_(slot_id);
                    return false;
                }
            }
            impl_->ctx->CopyResource(impl_->staging_bgra.Get(), slot.upload_bgra.Get());
            D3D11_MAPPED_SUBRESOURCE mapped = {};
            if (FAILED(impl_->ctx->Map(impl_->staging_bgra.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
                release_slot_prep_fail_(slot_id);
                return false;
            }
            bgra_to_nv12((const uint8_t*)mapped.pData, (int)mapped.RowPitch / 4, w_, h_,
                         impl_->nv12_cpu.data());
            impl_->ctx->Unmap(impl_->staging_bgra.Get(), 0);
        }
        ComPtr<IMFMediaBuffer> buf;
        DWORD nv12_size = (DWORD)(w_ * h_ * 3 / 2);
        if (FAILED(MFCreateMemoryBuffer(nv12_size, &buf))) {
            release_slot_prep_fail_(slot_id);
            return false;
        }
        BYTE* dst = nullptr;
        if (FAILED(buf->Lock(&dst, nullptr, nullptr))) {
            release_slot_prep_fail_(slot_id);
            return false;
        }
        memcpy(dst, impl_->nv12_cpu.data(), nv12_size);
        buf->Unlock();
        buf->SetCurrentLength(nv12_size);
        sample->AddBuffer(buf.Get());
    }

    LONGLONG st = impl_->sample_time;
    sample->SetSampleTime(st);
    sample->SetSampleDuration(impl_->sample_duration);
    impl_->sample_time += impl_->sample_duration;

    if (tracked) {
        ComPtr<IMFTrackedSample> ts;
        if (SUCCEEDED(sample.As(&ts)) && ts) {
            TrackedReleaseCb* cb = new TrackedReleaseCb();
            cb->impl = impl_;
            hr = ts->SetAllocator(cb, nullptr);
            cb->Release(); // SetAllocator AddRefs; drop ctor ref
            if (FAILED(hr)) {
                LOG_WARN("h264", "IMFTrackedSample::SetAllocator hr=0x%08lx — conservative mode", hr);
                impl_->tracked_reliable = false;
                tracked = false;
            }
        } else {
            tracked = false;
            impl_->tracked_reliable = false;
        }
    }

    // Gate: need NeedInput credit (async) and in-flight < pool.
    if (impl_->async_mft) {
        if (impl_->need_input <= 0)
            pump_async_(out, 8);
        int in_flight = 0;
        {
            std::lock_guard<std::mutex> lk(impl_->slot_mtx);
            in_flight = impl_->mft_in_flight;
        }
        if (impl_->need_input <= 0 || in_flight >= kTexPool) {
            LOG_WARN("h264",
                     "drop frame capture=%u submit=%u slot=%d need_input=%d in_flight=%d — force IDR",
                     capture_id, submit_id, slot_id, impl_->need_input, in_flight);
            impl_->force_key_pending = true;
            release_slot_prep_fail_(slot_id);
            return !out.empty();
        }
    } else {
        int in_flight = 0;
        {
            std::lock_guard<std::mutex> lk(impl_->slot_mtx);
            in_flight = impl_->mft_in_flight;
        }
        if (in_flight >= kTexPool) {
            LOG_WARN("h264", "drop frame submit=%u — in_flight=%d >= pool — force IDR",
                     submit_id, in_flight);
            impl_->force_key_pending = true;
            release_slot_prep_fail_(slot_id);
            return !out.empty();
        }
    }

    ULONG ref_before = peek_refcount(sample.Get());
    hr = impl_->xform->ProcessInput(0, sample.Get(), 0);
    if (FAILED(hr) && hr != MF_E_NOTACCEPTING) {
        LOG_WARN("h264", "ProcessInput hr=0x%08lx", hr);
        release_slot_prep_fail_(slot_id);
        return false;
    }
    if (hr == MF_E_NOTACCEPTING) {
        LOG_WARN("h264", "ProcessInput NOTACCEPTING submit=%u — drop + force IDR", submit_id);
        impl_->force_key_pending = true;
        release_slot_prep_fail_(slot_id);
        return !out.empty();
    }

    uint64_t t_in = GetTickCount64();
    {
        std::lock_guard<std::mutex> lk(impl_->slot_mtx);
        slot.frame_id = submit_id;
        slot.capture_id = capture_id;
        slot.sample_time = st;
        slot.process_input_ms = t_in;
        slot.release_ms = 0;
        slot.release_proven = false;
        slot.tracked_set = tracked;
        slot.sample_raw = sample.Get(); // identity only — ComPtr below will LOCAL_RELEASE
        slot.state = TexSlotState::MftInFlight;
        impl_->mft_in_flight++;
        impl_->submits_since_proven++;
        if (impl_->async_mft)
            impl_->need_input--;
    }

    LOG("h264",
        "sample_life SUBMIT sample=%p frame=%u slot=%d capture=%u encode_submit_id=%u "
        "mft_input_id=%u ref≈%lu t_in=%llu tracked=%d in_flight=%d",
        (void*)sample.Get(), submit_id, slot_id, capture_id, submit_id, submit_id,
        (unsigned long)ref_before, (unsigned long long)t_in, (int)tracked, impl_->mft_in_flight);

    // Drop OUR ComPtr hold so TrackedSample final-release can fire when MFT releases.
    // This LOCAL_RELEASE must never mark the slot FREE.
    {
        ULONG ref_before_local = peek_refcount(sample.Get());
        sample.Reset();
        LOG_DEBUG("h264",
                  "sample_life LOCAL_RELEASE sample_was frame=%u slot=%d ref_before≈%lu "
                  "(slot stays MFT_IN_FLIGHT)",
                  submit_id, slot_id, (unsigned long)ref_before_local);
    }

    // Heuristic: many submits without any proven release → mark unreliable (still no early FREE).
    if (impl_->tracked_reliable && impl_->proven_releases == 0 &&
        impl_->submits_since_proven >= (uint32_t)kTexPool * 8) {
        LOG_WARN("h264",
                 "no PROVEN_RELEASE after %u submits — TrackedSample may be unreliable; "
                 "holding slots (correctness)",
                 impl_->submits_since_proven);
        impl_->tracked_reliable = false;
    }

    if (impl_->async_mft) {
        pump_async_(out, 20);
        return !out.empty();
    }
    return drain_output_(out);
}

bool H264Encoder::encode_texture(ID3D11Texture2D* bgra_tex, int src_w, int src_h,
                                 std::vector<H264Packet>& out, uint32_t capture_id) {
    out.clear();
    if (!ready_ || !impl_ || !bgra_tex) return false;
    int ew = src_w & ~1, eh = src_h & ~1;
    if (ew != w_ || eh != h_) {
        ComPtr<ID3D11Device> dev = impl_->device;
        int fps = impl_->fps;
        if (!init(dev.Get(), ew, eh, fps)) return false;
    }

    int slot_id = acquire_free_slot_();
    if (slot_id < 0) {
        LOG_WARN("h264", "no FREE slot capture=%u — drop + force IDR (in_flight cap)", capture_id);
        impl_->force_key_pending = true;
        if (impl_->async_mft)
            pump_async_(out, 5);
        return !out.empty();
    }

    uint32_t submit_id = impl_->next_submit_id++;
    LOG("h264", "encode_submit_id=%u capture_id=%u slot=%d", submit_id, capture_id, slot_id);

    if (!gpu_write_slot_(slot_id, bgra_tex, nullptr, ew)) {
        release_slot_prep_fail_(slot_id);
        return false;
    }
    return feed_nv12_and_drain_(slot_id, capture_id, submit_id, out);
}

bool H264Encoder::encode_bgra(const uint8_t* bgra, int src_w, int src_h, std::vector<H264Packet>& out,
                              uint32_t capture_id) {
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

    int slot_id = acquire_free_slot_();
    if (slot_id < 0) {
        LOG_WARN("h264", "no FREE slot capture=%u (bgra) — drop + force IDR", capture_id);
        impl_->force_key_pending = true;
        return !out.empty();
    }

    uint32_t submit_id = impl_->next_submit_id++;
    LOG("h264", "encode_submit_id=%u capture_id=%u slot=%d (bgra)", submit_id, capture_id, slot_id);

    if (!gpu_write_slot_(slot_id, nullptr, bgra, src_w)) {
        release_slot_prep_fail_(slot_id);
        return false;
    }
    return feed_nv12_and_drain_(slot_id, capture_id, submit_id, out);
}
