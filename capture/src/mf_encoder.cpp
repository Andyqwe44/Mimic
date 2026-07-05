/**
 * Media Foundation H.264 Hardware Encoder — implementation
 *
 * Pipeline:
 *   BGRA GPU tex → staging read → CPU BGRA→NV12 → upload NV12 tex →
 *   MFCreateDXGISurfaceBuffer → IMFSample → HW encoder → H.264 NAL units
 *
 * CPU NV12 conversion = ~0.3ms at 640px. GPU compute shader TODO for zero-copy.
 * H.264 output = ~15KB/frame (vs 900KB BMP). Compressed bitstream stays small.
 */
#include "mf_encoder.hpp"

#define NOMINMAX
#ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <codecapi.h>
#include <cstdio>
#include <cstring>
#include <new>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "dxgi.lib")

#ifndef MFT_TRANSFORM_CLSID_Attribute
  #define MFT_TRANSFORM_CLSID_Attribute MFTransform_Attribute_CLSID
  // {6821c42b-65a4-4e82-99bc-9a88205ecd0c}
  static const GUID MFTransform_Attribute_CLSID = { 0x6821c42b, 0x65a4, 0x4e82, { 0x99, 0xbc, 0x9a, 0x88, 0x20, 0x5e, 0xcd, 0x0c } };
#endif

// Missing from older SDKs
#ifndef CODECAPI_AVLowLatencyMode
  static const GUID CODECAPI_AVLowLatencyMode = { 0x9c27891a, 0xed7a, 0x40e1, { 0x88, 0xe8, 0xb2, 0x27, 0x27, 0xa0, 0x24, 0xee } };
#endif
#ifndef MF_E_ALREADY_INITIALIZED
  #define MF_E_ALREADY_INITIALIZED _HRESULT_TYPEDEF_(0xC00D36B0L)
#endif
#ifndef MF_E_TRANSFORM_NEED_MORE_INPUT
  #define MF_E_TRANSFORM_NEED_MORE_INPUT _HRESULT_TYPEDEF_(0xC00D6D72L)
#endif
#ifndef MFT_FRIENDLY_NAME_ATTR
  #define MFT_FRIENDLY_NAME_ATTR MFTransform_Attribute_FriendlyName
  // {314ffbae-5b41-4e95-9c19-4e7d500fa0d5}
  static const GUID MFTransform_Attribute_FriendlyName = { 0x314ffbae, 0x5b41, 0x4e95, { 0x9c, 0x19, 0x4e, 0x7d, 0x50, 0x0f, 0xa0, 0xd5 } };
#endif

MfH264Encoder::MfH264Encoder() = default;
MfH264Encoder::~MfH264Encoder() { shutdown(); }

// ── Init ───────────────────────────────────────────────────
bool MfH264Encoder::init(ComPtr<ID3D11Device> device, int width, int height,
                         int fps, int bitrate) {
    if (initialized_) return true;

    device_ = device;
    device_->GetImmediateContext(&ctx_);
    if (!ctx_) { fprintf(stderr, "[mf_enc] GetImmediateContext failed\n"); return false; }

    width_  = width;
    height_ = height;
    frame_duration_ = 10000000LL / fps;  // 100ns units, e.g. 166667 @60fps
    frame_pts_ = 0;

    HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
    if (FAILED(hr) && hr != MF_E_ALREADY_INITIALIZED) {
        fprintf(stderr, "[mf_enc] MFStartup failed: 0x%08lX\n", hr);
        return false;
    }

    if (!find_hardware_encoder()) {
        fprintf(stderr, "[mf_enc] No hardware H.264 encoder found\n");
        return false;
    }

    // CRITICAL: Set D3D11 device manager BEFORE type negotiation.
    // Hardware MFTs need the GPU device via MFT_MESSAGE_SET_D3D_MANAGER.
    {
        ComPtr<IMFDXGIDeviceManager> dev_mgr;
        UINT reset_token = 0;
        HRESULT hr_mgr = MFCreateDXGIDeviceManager(&reset_token, &dev_mgr);
        if (SUCCEEDED(hr_mgr) && dev_mgr) {
            hr_mgr = dev_mgr->ResetDevice(device_.Get(), reset_token);
            fprintf(stderr, "[mf_enc] IMFDXGIDeviceManager created + ResetDevice: 0x%08lX\n", hr_mgr);
            hr_mgr = encoder_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,
                reinterpret_cast<ULONG_PTR>(dev_mgr.Get()));
            fprintf(stderr, "[mf_enc] MFT_MESSAGE_SET_D3D_MANAGER: 0x%08lX\n", hr_mgr);
        } else {
            fprintf(stderr, "[mf_enc] MFCreateDXGIDeviceManager failed: 0x%08lX\n", hr_mgr);
        }
    }

    // Also set D3D11 awareness + codec props
    configure_encoder(width, height, fps, bitrate);

    if (!set_input_type(width, height, fps)) return false;
    if (!set_output_type()) return false;

    // Send MFT_MESSAGE_NOTIFY_BEGIN_STREAMING
    hr = encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    if (FAILED(hr)) fprintf(stderr, "[mf_enc] BEGIN_STREAMING warning: 0x%08lX\n", hr);

    // Send MFT_MESSAGE_NOTIFY_START_OF_STREAM
    hr = encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    if (FAILED(hr)) fprintf(stderr, "[mf_enc] START_OF_STREAM warning: 0x%08lX\n", hr);

    if (!create_resources(width, height)) return false;

    fprintf(stderr, "[mf_enc] init OK: %dx%d @%dfps %dkbps\n",
        width, height, fps, bitrate / 1000);
    initialized_ = true;
    return true;
}

// ── Find hardware H.264 encoder ─────────────────────────────
bool MfH264Encoder::find_hardware_encoder() {
    // First: diagnostic dump of ALL hardware video encoders (no type filter)
    {
        IMFActivate** all_acts = nullptr;
        UINT32 all_count = 0;
        HRESULT dhr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
            MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
            nullptr, nullptr, &all_acts, &all_count);
        fprintf(stderr, "[mf_enc] ALL HW video encoders (no type filter): %u found (hr=0x%08lX)\n", all_count, dhr);
        for (UINT32 ai = 0; ai < all_count; ai++) {
            WCHAR* fn = nullptr;
            GUID clsid = {};
            all_acts[ai]->GetAllocatedString(MFT_FRIENDLY_NAME_ATTR, &fn, nullptr);
            all_acts[ai]->GetGUID(MFT_TRANSFORM_CLSID_Attribute, &clsid);
            fprintf(stderr, "[mf_enc]   [%u] %S CLSID={%08lX-...}\n",
                ai, fn ? fn : L"(unnamed)", clsid.Data1);
            if (fn) CoTaskMemFree(fn);
            all_acts[ai]->Release();
        }
        CoTaskMemFree(all_acts);
    }

    // Now: enumerate with NV12→H264 filter
    IMFActivate** activates = nullptr;
    UINT32 count = 0;

    MFT_REGISTER_TYPE_INFO input_type  = { MFMediaType_Video, MFVideoFormat_NV12 };
    MFT_REGISTER_TYPE_INFO output_type = { MFMediaType_Video, MFVideoFormat_H264 };

    HRESULT hr = MFTEnumEx(
        MFT_CATEGORY_VIDEO_ENCODER,
        MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
        &input_type,    // must accept NV12
        &output_type,   // must output H264
        &activates, &count);

    if (FAILED(hr) || count == 0) {
        // Try without filter — some encoders don't advertise NV12 but accept it
        fprintf(stderr, "[mf_enc] MFTEnumEx(HW,NV12→H264): 0x%08lX count=%u — retrying without type filter\n", hr, count);
        hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
            MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
            nullptr, nullptr, &activates, &count);
        if (FAILED(hr) || count == 0) {
            fprintf(stderr, "[mf_enc] No hardware encoder found (hr=0x%08lX count=%u)\n", hr, count);
            return false;
        }
    }

    // Log what we found and dump supported types
    for (UINT32 i = 0; i < count; i++) {
        WCHAR* friendly_name = nullptr;
        activates[i]->GetAllocatedString(MFT_FRIENDLY_NAME_ATTR, &friendly_name, nullptr);

        // Get CLSID of this activate object
        GUID clsid = {};
        activates[i]->GetGUID(MFT_TRANSFORM_CLSID_Attribute, &clsid);
        fprintf(stderr, "[mf_enc] HW encoder[%u]: %S CLSID={%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}\n",
            i, friendly_name ? friendly_name : L"(unnamed)",
            clsid.Data1, clsid.Data2, clsid.Data3,
            clsid.Data4[0], clsid.Data4[1], clsid.Data4[2], clsid.Data4[3],
            clsid.Data4[4], clsid.Data4[5], clsid.Data4[6], clsid.Data4[7]);
        if (friendly_name) CoTaskMemFree(friendly_name);

        // Activate temporarily to dump supported input AND output types
        ComPtr<IMFTransform> tmp;
        if (SUCCEEDED(activates[i]->ActivateObject(IID_PPV_ARGS(&tmp))) && tmp) {
            DWORD inc = 0, outc = 0;
            tmp->GetStreamCount(&inc, &outc);
            fprintf(stderr, "[mf_enc]   streams: %lu in, %lu out\n", inc, outc);

            for (DWORD si = 0; si < inc; si++) {
                fprintf(stderr, "[mf_enc]   input[%lu] types:", si);
                int ti_count = 0;
                for (DWORD ti = 0; ; ti++) {
                    ComPtr<IMFMediaType> avail;
                    HRESULT ghr = tmp->GetInputAvailableType(si, ti, &avail);
                    if (FAILED(ghr)) break;
                    ti_count++;
                    GUID subtype = {};
                    UINT32 mw = 0, mh = 0;
                    if (SUCCEEDED(avail->GetGUID(MF_MT_SUBTYPE, &subtype))) {
                        MFGetAttributeSize(avail.Get(), MF_MT_FRAME_SIZE, &mw, &mh);
                        fprintf(stderr, " %c%c%c%c(%dx%d)",
                            (char)(subtype.Data1 & 0xFF),
                            (char)((subtype.Data1 >> 8) & 0xFF),
                            (char)((subtype.Data1 >> 16) & 0xFF),
                            (char)((subtype.Data1 >> 24) & 0xFF),
                            (int)mw, (int)mh);
                    }
                }
                fprintf(stderr, " (%d total)\n", ti_count);
            }
            for (DWORD so = 0; so < outc; so++) {
                fprintf(stderr, "[mf_enc]   output[%lu] types:", so);
                int to_count = 0;
                for (DWORD to = 0; ; to++) {
                    ComPtr<IMFMediaType> avail;
                    HRESULT ghr = tmp->GetOutputAvailableType(so, to, &avail);
                    if (FAILED(ghr)) break;
                    to_count++;
                    GUID subtype = {};
                    if (SUCCEEDED(avail->GetGUID(MF_MT_SUBTYPE, &subtype))) {
                        fprintf(stderr, " %c%c%c%c",
                            (char)(subtype.Data1 & 0xFF),
                            (char)((subtype.Data1 >> 8) & 0xFF),
                            (char)((subtype.Data1 >> 16) & 0xFF),
                            (char)((subtype.Data1 >> 24) & 0xFF));
                    }
                }
                fprintf(stderr, " (%d total)\n", to_count);
            }
            tmp.Reset();
        }
    }

    // Activate first one
    hr = activates[0]->ActivateObject(IID_PPV_ARGS(&encoder_));
    for (UINT32 i = 0; i < count; i++) activates[i]->Release();
    CoTaskMemFree(activates);

    if (FAILED(hr) || !encoder_) {
        fprintf(stderr, "[mf_enc] ActivateObject failed: 0x%08lX\n", hr);
        return false;
    }

    return true;
}

// ── Configure encoder BEFORE type negotiation ─────────────────
// AMD/NVIDIA/Intel hardware MFTs need D3D11 awareness + codec props
// set via IMFAttributes BEFORE they expose their input/output types.
void MfH264Encoder::configure_encoder(int width, int height, int fps, int bitrate) {
    if (!encoder_) return;

    // GUIDs for D3D11-aware encoding + codec properties
    const GUID GUID_D3D11_AWARE = { 0xe8fadc4b, 0x72ef, 0x4e94, { 0x84, 0xe6, 0x8b, 0x8d, 0x3d, 0x78, 0x6b, 0xaf } };
    const GUID GUID_D3D11_SHARED = { 0x39a7e28d, 0x81db, 0x4e33, { 0x95, 0x4c, 0x1d, 0xae, 0x13, 0xdf, 0x90, 0x5a } };

    // Get IMFAttributes from encoder
    IMFAttributes* attrs = nullptr;
    HRESULT hr_attr = encoder_->QueryInterface(IID_PPV_ARGS(&attrs));
    if (FAILED(hr_attr) || !attrs) {
        // Try GetAttributes (some MFTs expose attrs this way)
        hr_attr = encoder_->GetAttributes(&attrs);
    }
    if (!attrs) {
        fprintf(stderr, "[mf_enc] Cannot get IMFAttributes from encoder\n");
        return;
    }

    // 1) D3D11 awareness — MUST be set before type negotiation
    HRESULT hr = attrs->SetUINT32(GUID_D3D11_AWARE, TRUE);
    fprintf(stderr, "[mf_enc] D3D11_AWARE: 0x%08lX\n", hr);
    attrs->SetUINT32(GUID_D3D11_SHARED, TRUE); // optional, ignore result

    // 2) Low latency mode — no B-frames
    attrs->SetUINT32(CODECAPI_AVLowLatencyMode, TRUE);

    // 3) Rate control: Unconstrained VBR
    attrs->SetUINT32(CODECAPI_AVEncCommonRateControlMode,
        eAVEncCommonRateControlMode_UnconstrainedVBR);

    // 4) Quality level (0-100, VBR mode)
    attrs->SetUINT32(CODECAPI_AVEncCommonQuality, 70);

    // 5) GOP size (~2 seconds)
    attrs->SetUINT32(CODECAPI_AVEncMPVGOPSize, (UINT32)(fps * 2));

    // 6) Mean bitrate
    attrs->SetUINT32(CODECAPI_AVEncCommonMeanBitRate, (UINT32)bitrate);

    attrs->Release();
    fprintf(stderr, "[mf_enc] configure_encoder: D3D11+codec props set (result=0x%08lX)\n", hr);
}

// ── Set input media type — try supported formats in order ──
bool MfH264Encoder::set_input_type(int width, int height, int fps) {
    DWORD in_count = 0, out_count = 0;
    HRESULT hr = encoder_->GetStreamCount(&in_count, &out_count);
    if (FAILED(hr)) { fprintf(stderr, "[mf_enc] GetStreamCount failed: 0x%08lX\n", hr); return false; }

    input_stream_id_ = 0;
    for (DWORD i = 0; i < in_count; i++) {
        MFT_INPUT_STREAM_INFO info = {};
        hr = encoder_->GetInputStreamInfo(i, &info);
        if (SUCCEEDED(hr) && (info.dwFlags & MFT_INPUT_STREAM_WHOLE_SAMPLES)) {
            input_stream_id_ = i; break;
        }
    }

    // Log supported input subtypes
    fprintf(stderr, "[mf_enc] supported input subtypes:");
    for (DWORD i = 0; ; i++) {
        ComPtr<IMFMediaType> avail;
        if (FAILED(encoder_->GetInputAvailableType(input_stream_id_, i, &avail))) break;
        GUID subtype = {};
        if (SUCCEEDED(avail->GetGUID(MF_MT_SUBTYPE, &subtype))) {
            fprintf(stderr, " [%lu]", i);
        }
    }
    fprintf(stderr, "\n");

    // Try common hardware encoder formats in priority order
    const GUID* formats[] = {
        &MFVideoFormat_NV12,
        &MFVideoFormat_YV12,
        &MFVideoFormat_YUY2,
        &MFVideoFormat_IYUV,
        &MFVideoFormat_I420,
    };

    for (int fi = 0; fi < 5; fi++) {
        ComPtr<IMFMediaType> mt;
        hr = MFCreateMediaType(&mt);
        if (FAILED(hr)) continue;

        mt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        mt->SetGUID(MF_MT_SUBTYPE, *formats[fi]);
        MFSetAttributeSize(mt.Get(), MF_MT_FRAME_SIZE, width, height);
        MFSetAttributeRatio(mt.Get(), MF_MT_FRAME_RATE, fps, 1);
        MFSetAttributeRatio(mt.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        mt->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        mt->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);

        hr = encoder_->SetInputType(input_stream_id_, mt.Get(), 0);
        if (SUCCEEDED(hr)) {
            fprintf(stderr, "[mf_enc] SetInputType OK: format[%d] %dx%d @%d (stream %lu)\n",
                fi, width, height, fps, input_stream_id_);
            return true;
        }
        fprintf(stderr, "[mf_enc] SetInputType format[%d] %dx%d @%d: 0x%08lX\n",
            fi, width, height, fps, hr);
    }

    // All specific formats failed — try partial types that the encoder may complete
    // Some encoders reject fully-specified types but accept incomplete ones
    {
        // Try: major type only (let encoder pick everything)
        ComPtr<IMFMediaType> mt;
        MFCreateMediaType(&mt);
        mt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        hr = encoder_->SetInputType(input_stream_id_, mt.Get(), 0);
        fprintf(stderr, "[mf_enc] SetInputType(major-only): 0x%08lX\n", hr);
        if (SUCCEEDED(hr)) {
            ComPtr<IMFMediaType> current;
            encoder_->GetInputCurrentType(input_stream_id_, &current);
            GUID sub = {};
            UINT32 mw=0, mh=0;
            current->GetGUID(MF_MT_SUBTYPE, &sub);
            MFGetAttributeSize(current.Get(), MF_MT_FRAME_SIZE, &mw, &mh);
            fprintf(stderr, "[mf_enc] encoder chose: %c%c%c%c %dx%d\n",
                (char)(sub.Data1&0xFF),(char)((sub.Data1>>8)&0xFF),
                (char)((sub.Data1>>16)&0xFF),(char)((sub.Data1>>24)&0xFF), (int)mw, (int)mh);
            return true;
        }
    }
    {
        // Try: major type + frame size only
        ComPtr<IMFMediaType> mt;
        MFCreateMediaType(&mt);
        mt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        MFSetAttributeSize(mt.Get(), MF_MT_FRAME_SIZE, width, height);
        hr = encoder_->SetInputType(input_stream_id_, mt.Get(), 0);
        fprintf(stderr, "[mf_enc] SetInputType(major+size %dx%d): 0x%08lX\n", width, height, hr);
        if (SUCCEEDED(hr)) {
            ComPtr<IMFMediaType> current;
            encoder_->GetInputCurrentType(input_stream_id_, &current);
            GUID sub = {};
            current->GetGUID(MF_MT_SUBTYPE, &sub);
            fprintf(stderr, "[mf_enc] encoder chose: %c%c%c%c\n",
                (char)(sub.Data1&0xFF),(char)((sub.Data1>>8)&0xFF),
                (char)((sub.Data1>>16)&0xFF),(char)((sub.Data1>>24)&0xFF));
            return true;
        }
    }
    {
        // Try: major type + frame size + framerate
        ComPtr<IMFMediaType> mt;
        MFCreateMediaType(&mt);
        mt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        MFSetAttributeSize(mt.Get(), MF_MT_FRAME_SIZE, width, height);
        MFSetAttributeRatio(mt.Get(), MF_MT_FRAME_RATE, fps, 1);
        mt->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        hr = encoder_->SetInputType(input_stream_id_, mt.Get(), 0);
        fprintf(stderr, "[mf_enc] SetInputType(major+size+fps %dx%d@%d): 0x%08lX\n", width, height, fps, hr);
        if (SUCCEEDED(hr)) {
            ComPtr<IMFMediaType> current;
            encoder_->GetInputCurrentType(input_stream_id_, &current);
            GUID sub = {};
            current->GetGUID(MF_MT_SUBTYPE, &sub);
            fprintf(stderr, "[mf_enc] encoder chose: %c%c%c%c\n",
                (char)(sub.Data1&0xFF),(char)((sub.Data1>>8)&0xFF),
                (char)((sub.Data1>>16)&0xFF),(char)((sub.Data1>>24)&0xFF));
            return true;
        }
    }
    {
        // Windows 11 24H2+ often uses ARGB32 for GPU capture → try BGRA
        ComPtr<IMFMediaType> mt;
        MFCreateMediaType(&mt);
        mt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        mt->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_ARGB32);
        MFSetAttributeSize(mt.Get(), MF_MT_FRAME_SIZE, width, height);
        MFSetAttributeRatio(mt.Get(), MF_MT_FRAME_RATE, fps, 1);
        mt->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        hr = encoder_->SetInputType(input_stream_id_, mt.Get(), 0);
        fprintf(stderr, "[mf_enc] SetInputType(ARGB32 %dx%d@%d): 0x%08lX\n", width, height, fps, hr);
        if (SUCCEEDED(hr)) return true;
    }

    fprintf(stderr, "[mf_enc] SetInputType: ALL approaches failed\n");
    return false;
}

// ── Set output media type ──────────────────────────────────
bool MfH264Encoder::set_output_type() {
    // Find output stream
    DWORD in_count = 0, out_count = 0;
    encoder_->GetStreamCount(&in_count, &out_count);
    output_stream_id_ = 0;
    for (DWORD i = 0; i < out_count; i++) {
        MFT_OUTPUT_STREAM_INFO info = {};
        HRESULT hr = encoder_->GetOutputStreamInfo(i, &info);
        if (SUCCEEDED(hr) && (info.dwFlags & MFT_OUTPUT_STREAM_WHOLE_SAMPLES)) {
            output_stream_id_ = i;
            break;
        }
    }

    ComPtr<IMFMediaType> media_type;
    HRESULT hr = MFCreateMediaType(&media_type);
    if (FAILED(hr)) return false;

    media_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    media_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);

    hr = MFSetAttributeSize(media_type.Get(), MF_MT_FRAME_SIZE, width_, height_);
    if (FAILED(hr)) return false;

    hr = MFSetAttributeRatio(media_type.Get(), MF_MT_FRAME_RATE,
        (UINT32)(10000000LL / frame_duration_), 1);
    if (FAILED(hr)) return false;

    hr = MFSetAttributeRatio(media_type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (FAILED(hr)) return false;

    media_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    media_type->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Main);

    hr = encoder_->SetOutputType(output_stream_id_, media_type.Get(), 0);
    if (FAILED(hr)) {
        fprintf(stderr, "[mf_enc] SetOutputType failed: 0x%08lX\n", hr);
        return false;
    }
    fprintf(stderr, "[mf_enc] SetOutputType OK (stream %lu)\n", output_stream_id_);
    return true;
}

// ── Create NV12 texture + staging BGRA ──────────────────────
bool MfH264Encoder::create_resources(int width, int height) {
    // NV12 texture: encoder input, needs D3D11_RESOURCE_MISC_SHARED for MFCreateDXGISurfaceBuffer
    D3D11_TEXTURE2D_DESC nv12_desc = {};
    nv12_desc.Width              = (UINT)width;
    nv12_desc.Height             = (UINT)height;
    nv12_desc.MipLevels          = 1;
    nv12_desc.ArraySize          = 1;
    nv12_desc.Format             = DXGI_FORMAT_NV12;
    nv12_desc.SampleDesc.Count   = 1;
    nv12_desc.Usage              = D3D11_USAGE_DEFAULT;
    nv12_desc.BindFlags          = D3D11_BIND_SHADER_RESOURCE;  // NV12 can't be RT
    nv12_desc.MiscFlags          = D3D11_RESOURCE_MISC_SHARED;  // needed for MFCreateDXGISurfaceBuffer
    // No CPU access — we upload via staging texture

    HRESULT hr = device_->CreateTexture2D(&nv12_desc, nullptr, &nv12_tex_);
    if (FAILED(hr)) {
        fprintf(stderr, "[mf_enc] CreateTexture2D(NV12 shared) failed: 0x%08lX\n", hr);
        return false;
    }

    // Staging BGRA for reading captured frame back to CPU
    D3D11_TEXTURE2D_DESC sb_desc = {};
    sb_desc.Width              = (UINT)width;
    sb_desc.Height             = (UINT)height;
    sb_desc.MipLevels          = 1;
    sb_desc.ArraySize          = 1;
    sb_desc.Format             = DXGI_FORMAT_B8G8R8A8_UNORM;
    sb_desc.SampleDesc.Count   = 1;
    sb_desc.Usage              = D3D11_USAGE_STAGING;
    sb_desc.CPUAccessFlags     = D3D11_CPU_ACCESS_READ;

    hr = device_->CreateTexture2D(&sb_desc, nullptr, &staging_bgra_);
    if (FAILED(hr)) {
        fprintf(stderr, "[mf_enc] CreateTexture2D(staging) failed: 0x%08lX\n", hr);
        return false;
    }
    staging_w_ = width;
    staging_h_ = height;

    return true;
}

// ── BGRA → NV12 CPU conversion (BT.601) ─────────────────────
// NV12 layout: Y plane (W×H), then UV plane interleaved (W/2 × H/2)
// Each UV pair covers a 2×2 block. UV samples at even positions.
void MfH264Encoder::bgra_to_nv12_cpu(const uint8_t* bgra, int w, int h,
                                     int row_pitch, uint8_t* nv12_y, uint8_t* nv12_uv,
                                     int nv12_pitch) {
    // BT.601 coefficients
    const float wr = 0.299f, wg = 0.587f, wb = 0.114f;
    const float u_max = 0.436f, v_max = 0.615f;

    int uv_w = w / 2;
    int uv_h = h / 2;

    for (int y = 0; y < h; y++) {
        const uint8_t* src_row = bgra + y * row_pitch;
        uint8_t*       dst_y   = nv12_y + y * nv12_pitch;

        for (int x = 0; x < w; x++) {
            float b = (float)src_row[x * 4 + 0];     // BGRA order
            float g = (float)src_row[x * 4 + 1];
            float r = (float)src_row[x * 4 + 2];

            float y_val = wr * r + wg * g + wb * b;
            if (y_val < 0.f) y_val = 0.f;
            if (y_val > 255.f) y_val = 255.f;
            dst_y[x] = (uint8_t)(y_val + 0.5f);
        }

        // UV for even rows only
        if ((y & 1) == 0) {
            const uint8_t* src_row1 = bgra + (y + 1) * row_pitch;
            uint8_t* dst_uv = nv12_uv + (y / 2) * nv12_pitch;

            for (int x = 0; x < w; x += 2) {
                // Average the 2×2 block
                int i00 = x * 4, i10 = (x + 1) * 4;
                float r_avg = ((float)src_row[i00+2]  + (float)src_row[i10+2] +
                               (float)src_row1[i00+2] + (float)src_row1[i10+2]) / 4.0f;
                float g_avg = ((float)src_row[i00+1]  + (float)src_row[i10+1] +
                               (float)src_row1[i00+1] + (float)src_row1[i10+1]) / 4.0f;
                float b_avg = ((float)src_row[i00+0]  + (float)src_row[i10+0] +
                               (float)src_row1[i00+0] + (float)src_row1[i10+0]) / 4.0f;

                float y_val = wr * r_avg + wg * g_avg + wb * b_avg;
                float u_val = u_max * (b_avg - y_val) / (1.0f - wb) + 128.0f;
                float v_val = v_max * (r_avg - y_val) / (1.0f - wr) + 128.0f;

                if (u_val < 0.f) u_val = 0.f; if (u_val > 255.f) u_val = 255.f;
                if (v_val < 0.f) v_val = 0.f; if (v_val > 255.f) v_val = 255.f;

                int uv_idx = (x / 2) * 2;
                dst_uv[uv_idx + 0] = (uint8_t)(u_val + 0.5f);
                dst_uv[uv_idx + 1] = (uint8_t)(v_val + 0.5f);
            }
        }
    }
}

// ── Encode one frame ───────────────────────────────────────
bool MfH264Encoder::encode(ComPtr<ID3D11Texture2D> bgra_texture,
                           std::vector<uint8_t>& h264_output) {
    if (!initialized_ || !bgra_texture) return false;

    // ── Step 1: Copy BGRA GPU→CPU staging ──────────────────
    D3D11_TEXTURE2D_DESC src_desc;
    bgra_texture->GetDesc(&src_desc);

    // Recreate staging if size changed
    if ((int)src_desc.Width != staging_w_ || (int)src_desc.Height != staging_h_) {
        staging_bgra_.Reset();
        D3D11_TEXTURE2D_DESC sb = {};
        sb.Width = src_desc.Width; sb.Height = src_desc.Height;
        sb.MipLevels = 1; sb.ArraySize = 1;
        sb.Format = src_desc.Format;
        sb.SampleDesc.Count = 1;
        sb.Usage = D3D11_USAGE_STAGING;
        sb.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(device_->CreateTexture2D(&sb, nullptr, &staging_bgra_))) return false;
        staging_w_ = (int)src_desc.Width;
        staging_h_ = (int)src_desc.Height;
    }

    ctx_->CopyResource(staging_bgra_.Get(), bgra_texture.Get());

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(ctx_->Map(staging_bgra_.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;
    auto* bgra_data = (const uint8_t*)mapped.pData;
    if (!bgra_data) { ctx_->Unmap(staging_bgra_.Get(), 0); return false; }

    // ── Step 2: BGRA→NV12 on CPU ──────────────────────────
    int sw = staging_w_, sh = staging_h_;
    int nv12_size_y  = sw * sh;
    int nv12_size_uv = (sw / 2) * (sh / 2) * 2;
    std::vector<uint8_t> nv12_buf(nv12_size_y + nv12_size_uv);
    uint8_t* nv12_y  = nv12_buf.data();
    uint8_t* nv12_uv = nv12_buf.data() + nv12_size_y;
    int nv12_pitch = sw;

    bgra_to_nv12_cpu(bgra_data, sw, sh, (int)mapped.RowPitch, nv12_y, nv12_uv, nv12_pitch);
    ctx_->Unmap(staging_bgra_.Get(), 0);

    // ── Step 3: Upload NV12 to GPU texture ────────────────
    // Use staging NV12 for upload
    D3D11_TEXTURE2D_DESC nv12_desc;
    nv12_tex_->GetDesc(&nv12_desc);

    ComPtr<ID3D11Texture2D> nv12_staging;
    D3D11_TEXTURE2D_DESC ns_desc = {};
    ns_desc.Width  = nv12_desc.Width;
    ns_desc.Height = nv12_desc.Height;
    ns_desc.MipLevels = 1;
    ns_desc.ArraySize = 1;
    ns_desc.Format    = DXGI_FORMAT_NV12;
    ns_desc.SampleDesc.Count = 1;
    ns_desc.Usage     = D3D11_USAGE_STAGING;
    ns_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    if (FAILED(device_->CreateTexture2D(&ns_desc, nullptr, &nv12_staging))) return false;

    D3D11_MAPPED_SUBRESOURCE ns_mapped = {};
    if (FAILED(ctx_->Map(nv12_staging.Get(), 0, D3D11_MAP_WRITE, 0, &ns_mapped))) return false;

    // Copy Y plane
    uint8_t* dst_y = (uint8_t*)ns_mapped.pData;
    for (int y = 0; y < sh; y++) {
        memcpy(dst_y + y * (int)ns_mapped.RowPitch, nv12_y + y * nv12_pitch, sw);
    }
    // Copy UV plane (offset = ns_mapped.RowPitch * sh for NV12 staging)
    uint8_t* dst_uv = dst_y + (int)ns_mapped.RowPitch * sh;
    for (int y = 0; y < sh / 2; y++) {
        memcpy(dst_uv + y * (int)ns_mapped.RowPitch, nv12_uv + y * nv12_pitch, sw);
    }

    ctx_->Unmap(nv12_staging.Get(), 0);
    ctx_->CopyResource(nv12_tex_.Get(), nv12_staging.Get());

    // ── Step 4: Create IMFSample from NV12 D3D11 texture ──
    ComPtr<IDXGISurface> dxgi_surface;
    HRESULT hr = nv12_tex_.As(&dxgi_surface);
    if (FAILED(hr)) {
        fprintf(stderr, "[mf_enc] nv12_tex_→IDXGISurface failed: 0x%08lX\n", hr);
        return false;
    }

    ComPtr<IMFMediaBuffer> media_buffer;
    hr = MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), dxgi_surface.Get(), 0, FALSE, &media_buffer);
    if (FAILED(hr)) {
        fprintf(stderr, "[mf_enc] MFCreateDXGISurfaceBuffer failed: 0x%08lX\n", hr);
        return false;
    }

    ComPtr<IMFSample> sample;
    hr = MFCreateSample(&sample);
    if (FAILED(hr)) return false;

    sample->AddBuffer(media_buffer.Get());

    // Set timestamp
    sample->SetSampleTime(frame_pts_);
    sample->SetSampleDuration(frame_duration_);

    // ── Step 5: ProcessInput ──────────────────────────────
    if (need_keyframe_) {
        // Force IDR: set UINT32 attribute on sample
        // MFSampleExtension_CleanPoint tells encoder to make this a keyframe
        sample->SetUINT32(MFSampleExtension_CleanPoint, TRUE);
        need_keyframe_ = false;
    }

    hr = encoder_->ProcessInput(input_stream_id_, sample.Get(), 0);
    if (FAILED(hr)) {
        fprintf(stderr, "[mf_enc] ProcessInput failed: 0x%08lX\n", hr);
        return false;
    }

    frame_pts_ += frame_duration_;

    // ── Step 6: ProcessOutput (loop — may have buffered frames) ──
    return drain(h264_output);
}

// ── Drain encoder output ────────────────────────────────────
bool MfH264Encoder::drain(std::vector<uint8_t>& h264_output) {
    bool got_output = false;

    while (true) {
        // Get output buffer requirements
        MFT_OUTPUT_STREAM_INFO stream_info = {};
        HRESULT hr = encoder_->GetOutputStreamInfo(output_stream_id_, &stream_info);
        if (FAILED(hr)) break;

        DWORD buf_size = stream_info.cbSize;
        if (buf_size < 65536) buf_size = 65536;  // min 64KB

        ComPtr<IMFMediaBuffer> out_buffer;
        hr = MFCreateMemoryBuffer(buf_size, &out_buffer);
        if (FAILED(hr)) break;

        ComPtr<IMFSample> out_sample;
        hr = MFCreateSample(&out_sample);
        if (FAILED(hr)) break;
        out_sample->AddBuffer(out_buffer.Get());

        DWORD status = 0;
        MFT_OUTPUT_DATA_BUFFER output_data = {};
        output_data.dwStreamID = output_stream_id_;
        output_data.pSample    = out_sample.Get();

        hr = encoder_->ProcessOutput(0, 1, &output_data, &status);

        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            break;  // Normal — encoder buffered this frame
        }
        if (FAILED(hr)) {
            fprintf(stderr, "[mf_enc] ProcessOutput failed: 0x%08lX\n", hr);
            break;
        }

        // Extract H.264 NAL units from output buffer
        DWORD out_len = 0;
        BYTE* out_data = nullptr;
        out_buffer->Lock(&out_data, nullptr, &out_len);
        if (out_data && out_len > 0) {
            // Prepend start code if not present
            // MF encoder outputs NAL units. Check if start code (00 00 00 01) exists.
            size_t prev_size = h264_output.size();
            if (out_len >= 4 && !(out_data[0]==0 && out_data[1]==0 && out_data[2]==0 && out_data[3]==1)) {
                // No start code — add one
                h264_output.push_back(0x00);
                h264_output.push_back(0x00);
                h264_output.push_back(0x00);
                h264_output.push_back(0x01);
            }
            h264_output.insert(h264_output.end(), out_data, out_data + out_len);
            got_output = true;
        }
        out_buffer->Unlock();

        if (status & MFT_OUTPUT_DATA_BUFFER_INCOMPLETE) {
            // Partial output — continue to get rest
            continue;
        }
    }

    return got_output;
}

// ── Request keyframe ────────────────────────────────────────
void MfH264Encoder::request_keyframe() {
    need_keyframe_ = true;
}

// ── Shutdown ────────────────────────────────────────────────
void MfH264Encoder::shutdown() {
    if (encoder_) {
        // Drain any remaining frames
        std::vector<uint8_t> dummy;
        // Send drain command
        encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        drain(dummy);  // get remaining frames

        encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        encoder_.Reset();
    }
    nv12_tex_.Reset();
    staging_bgra_.Reset();
    device_.Reset();
    ctx_.Reset();
    initialized_ = false;
}
