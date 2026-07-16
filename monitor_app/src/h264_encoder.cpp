/**
 * h264_encoder.cpp — MF H.264 MFT, low-latency, Annex-B output.
 */
#include "h264_encoder.h"
#include "../../logger/logger.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
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

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "wmcodecdspuuid.lib")

using Microsoft::WRL::ComPtr;

namespace {

void bgra_to_nv12(const uint8_t* bgra, int src_stride_px, int w, int h, uint8_t* nv12) {
    // Y plane (w/h must be even). src_stride_px = full BGRA width in pixels.
    uint8_t* yplane = nv12;
    for (int y = 0; y < h; ++y) {
        const uint8_t* row = bgra + (size_t)y * src_stride_px * 4;
        uint8_t* yrow = yplane + (size_t)y * w;
        for (int x = 0; x < w; ++x) {
            int b = row[x * 4 + 0];
            int g = row[x * 4 + 1];
            int r = row[x * 4 + 2];
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

// Convert encoder output to Annex-B. Handles AVCC (length-prefixed) or already-Annex-B.
bool to_annexb(const uint8_t* data, DWORD size, std::vector<uint8_t>& out, bool& keyframe) {
    out.clear();
    keyframe = false;
    if (!data || size < 4) return false;

    auto note_nal = [&](const uint8_t* nal, DWORD nal_len) {
        if (nal_len == 0) return;
        const uint8_t nal_type = nal[0] & 0x1F;
        if (nal_type == 5 || nal_type == 7 || nal_type == 8) keyframe = true;
        out.push_back(0); out.push_back(0); out.push_back(0); out.push_back(1);
        out.insert(out.end(), nal, nal + nal_len);
    };

    // Already Annex-B?
    if (data[0] == 0 && data[1] == 0 && (data[2] == 1 || (data[2] == 0 && data[3] == 1))) {
        out.assign(data, data + size);
        for (DWORD i = 0; i + 4 < size; ++i) {
            if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 && data[i + 3] == 1) {
                uint8_t t = data[i + 4] & 0x1F;
                if (t == 5 || t == 7 || t == 8) keyframe = true;
            } else if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
                uint8_t t = data[i + 3] & 0x1F;
                if (t == 5 || t == 7 || t == 8) keyframe = true;
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

} // namespace

struct H264Encoder::Impl {
    ComPtr<IMFTransform> xform;
    ComPtr<IMFMediaType> in_type;
    ComPtr<IMFMediaType> out_type;
    std::vector<uint8_t> nv12;
    LONGLONG sample_time = 0;
    LONGLONG sample_duration = 0;
    int fps = 60;
    bool mf_started = false;
};

H264Encoder::H264Encoder() = default;

H264Encoder::~H264Encoder() { shutdown(); }

void H264Encoder::shutdown() {
    if (impl_) {
        if (impl_->xform) {
            impl_->xform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
            impl_->xform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        }
        if (impl_->mf_started) {
            MFShutdown();
            impl_->mf_started = false;
        }
        delete impl_;
        impl_ = nullptr;
    }
    ready_ = false;
    w_ = h_ = 0;
}

bool H264Encoder::init(int width, int height, int fps, int bitrate_kbps) {
    shutdown();
    if (width < 16 || height < 16 || (width & 1) || (height & 1)) {
        // MF NV12 requires even dimensions — align down.
        width &= ~1;
        height &= ~1;
    }
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
    impl_->sample_duration = 10'000'000LL / fps; // 100ns units
    impl_->nv12.resize((size_t)width * height * 3 / 2);

    // System-memory NV12 works with software MFTs. Hardware MFTs often need
    // DXGI surfaces (MF_E_INVALIDMEDIATYPE / 0xc00d6d77) — try SW first, then HW.
    MFT_REGISTER_TYPE_INFO in_info = { MFMediaType_Video, MFVideoFormat_NV12 };
    MFT_REGISTER_TYPE_INFO out_info = { MFMediaType_Video, MFVideoFormat_H264 };

    auto try_activate_list = [&](DWORD flags, const char* label) -> bool {
        IMFActivate** activates = nullptr;
        UINT32 count = 0;
        HRESULT ehr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags, &in_info, &out_info, &activates, &count);
        if (FAILED(ehr) || count == 0) {
            if (activates) CoTaskMemFree(activates);
            return false;
        }
        for (UINT32 i = 0; i < count; ++i) {
            ComPtr<IMFTransform> xform;
            if (FAILED(activates[i]->ActivateObject(IID_PPV_ARGS(&xform))) || !xform)
                continue;

            ComPtr<IMFMediaType> out_type, in_type;
            if (FAILED(MFCreateMediaType(&out_type))) continue;
            out_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            out_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
            MFSetAttributeSize(out_type.Get(), MF_MT_FRAME_SIZE, width, height);
            MFSetAttributeRatio(out_type.Get(), MF_MT_FRAME_RATE, fps, 1);
            MFSetAttributeRatio(out_type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
            out_type->SetUINT32(MF_MT_AVG_BITRATE, (UINT32)bitrate_kbps * 1000);
            out_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
            out_type->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Base);
            if (FAILED(xform->SetOutputType(0, out_type.Get(), 0))) continue;

            if (FAILED(MFCreateMediaType(&in_type))) continue;
            in_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            in_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
            MFSetAttributeSize(in_type.Get(), MF_MT_FRAME_SIZE, width, height);
            MFSetAttributeRatio(in_type.Get(), MF_MT_FRAME_RATE, fps, 1);
            MFSetAttributeRatio(in_type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
            in_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
            in_type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
            in_type->SetUINT32(MF_MT_DEFAULT_STRIDE, (UINT32)width);
            bool input_ok = false;
            ComPtr<IMFMediaType> avail;
            for (DWORD ti = 0; SUCCEEDED(xform->GetInputAvailableType(0, ti, &avail)); ++ti) {
                GUID sub = {};
                if (SUCCEEDED(avail->GetGUID(MF_MT_SUBTYPE, &sub)) && sub == MFVideoFormat_NV12) {
                    MFSetAttributeSize(avail.Get(), MF_MT_FRAME_SIZE, width, height);
                    MFSetAttributeRatio(avail.Get(), MF_MT_FRAME_RATE, fps, 1);
                    MFSetAttributeRatio(avail.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
                    avail->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
                    avail->SetUINT32(MF_MT_DEFAULT_STRIDE, (UINT32)width);
                    if (SUCCEEDED(xform->SetInputType(0, avail.Get(), 0))) {
                        in_type = avail;
                        input_ok = true;
                        break;
                    }
                }
                avail.Reset();
            }
            if (!input_ok) {
                if (FAILED(xform->SetInputType(0, in_type.Get(), 0)))
                    continue;
                input_ok = true;
            }

            ComPtr<IMFMediaType> cur;
            if (FAILED(xform->GetInputCurrentType(0, &cur)) || !cur) continue;

            impl_->xform = xform;
            impl_->out_type = out_type;
            impl_->in_type = in_type ? in_type : cur;
            LOG("h264", "using %s encoder #%u", label, i);
            for (UINT32 j = 0; j < count; ++j) activates[j]->Release();
            CoTaskMemFree(activates);
            return true;
        }
        for (UINT32 j = 0; j < count; ++j) activates[j]->Release();
        CoTaskMemFree(activates);
        return false;
    };

    // Software first (system-memory NV12), then hardware (may still fail without DXGI).
    bool ok = try_activate_list(
        MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER,
        "software");
    if (!ok) {
        ok = try_activate_list(
            MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
            "hardware");
    }
    if (!ok || !impl_->xform) {
        LOG_ERROR("h264", "no H.264 MFT accepts system-memory NV12 %dx%d", width, height);
        shutdown();
        return false;
    }

    // Low-latency codec API (best-effort; ignore failures).
    ComPtr<ICodecAPI> codec;
    if (SUCCEEDED(impl_->xform.As(&codec)) && codec) {
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_BOOL; v.boolVal = VARIANT_TRUE;
        codec->SetValue(&CODECAPI_AVLowLatencyMode, &v);
        VariantClear(&v);
        VariantInit(&v);
        v.vt = VT_UI4; v.ulVal = eAVEncCommonRateControlMode_CBR;
        codec->SetValue(&CODECAPI_AVEncCommonRateControlMode, &v);
        VariantClear(&v);
        VariantInit(&v);
        v.vt = VT_UI4; v.ulVal = (ULONG)bitrate_kbps * 1000;
        codec->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &v);
        VariantClear(&v);
        VariantInit(&v);
        v.vt = VT_UI4; v.ulVal = (ULONG)fps; // ~1s GOP; balance latency vs size
        codec->SetValue(&CODECAPI_AVEncMPVGOPSize, &v);
        VariantClear(&v);
    }

    impl_->xform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    impl_->xform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    impl_->xform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

    w_ = width;
    h_ = height;
    ready_ = true;
    LOG("h264", "encoder ready %dx%d @ %dfps %dkbps (MF MFT)", width, height, fps, bitrate_kbps);
    return true;
}

bool H264Encoder::encode_bgra(const uint8_t* bgra, int src_w, int src_h, std::vector<H264Packet>& out) {
    out.clear();
    if (!ready_ || !impl_ || !bgra) return false;
    int ew = src_w & ~1, eh = src_h & ~1;
    if (ew != w_ || eh != h_) {
        int fps = impl_->fps;
        if (!init(ew, eh, fps)) return false;
    }

    bgra_to_nv12(bgra, src_w, w_, h_, impl_->nv12.data());

    ComPtr<IMFMediaBuffer> buf;
    DWORD nv12_size = (DWORD)(w_ * h_ * 3 / 2);
    HRESULT hr = MFCreateMemoryBuffer(nv12_size, &buf);
    if (FAILED(hr)) return false;
    BYTE* dst = nullptr;
    hr = buf->Lock(&dst, nullptr, nullptr);
    if (FAILED(hr)) return false;
    memcpy(dst, impl_->nv12.data(), nv12_size);
    buf->Unlock();
    buf->SetCurrentLength(nv12_size);

    ComPtr<IMFSample> sample;
    hr = MFCreateSample(&sample);
    if (FAILED(hr)) return false;
    sample->AddBuffer(buf.Get());
    sample->SetSampleTime(impl_->sample_time);
    sample->SetSampleDuration(impl_->sample_duration);
    impl_->sample_time += impl_->sample_duration;

    hr = impl_->xform->ProcessInput(0, sample.Get(), 0);
    if (hr == MF_E_NOTACCEPTING) {
        // Drain then retry once.
    } else if (FAILED(hr)) {
        LOG_WARN("h264", "ProcessInput hr=0x%08lx", hr);
        return false;
    }

    for (;;) {
        MFT_OUTPUT_STREAM_INFO info = {};
        impl_->xform->GetOutputStreamInfo(0, &info);

        ComPtr<IMFSample> out_sample;
        ComPtr<IMFMediaBuffer> out_buf;
        MFT_OUTPUT_DATA_BUFFER odb = {};
        odb.dwStreamID = 0;

        bool need_provide = !(info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES);
        if (need_provide) {
            DWORD sz = info.cbSize ? info.cbSize : (DWORD)(w_ * h_);
            if (FAILED(MFCreateSample(&out_sample))) break;
            if (FAILED(MFCreateMemoryBuffer(sz, &out_buf))) break;
            out_sample->AddBuffer(out_buf.Get());
            odb.pSample = out_sample.Get();
        }

        DWORD status = 0;
        hr = impl_->xform->ProcessOutput(0, 1, &odb, &status);
        if (odb.pEvents) { odb.pEvents->Release(); odb.pEvents = nullptr; }

        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) break;
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            ComPtr<IMFMediaType> mt;
            impl_->xform->GetOutputAvailableType(0, 0, &mt);
            if (mt) impl_->xform->SetOutputType(0, mt.Get(), 0);
            continue;
        }
        if (FAILED(hr)) {
            LOG_WARN("h264", "ProcessOutput hr=0x%08lx", hr);
            break;
        }

        IMFSample* s = odb.pSample ? odb.pSample : out_sample.Get();
        if (!s) continue;

        ComPtr<IMFMediaBuffer> contiguous;
        if (FAILED(s->ConvertToContiguousBuffer(&contiguous)) || !contiguous) {
            if (odb.pSample && need_provide == false) odb.pSample->Release();
            continue;
        }
        BYTE* data = nullptr;
        DWORD len = 0;
        if (FAILED(contiguous->Lock(&data, nullptr, &len)) || !data || len == 0) {
            if (odb.pSample && !need_provide) odb.pSample->Release();
            continue;
        }

        H264Packet pkt;
        pkt.w = w_;
        pkt.h = h_;
        bool ok = to_annexb(data, len, pkt.annexb, pkt.keyframe);
        contiguous->Unlock();
        if (odb.pSample && !need_provide) odb.pSample->Release();
        if (ok) out.push_back(std::move(pkt));
    }
    return true;
}
