/**
 * capture_stream.exe — persistent capture, frame-differenced stream
 *
 * Usage:   capture_stream.exe <hwnd>
 *
 * Window capture: Windows.Graphics.Capture FramePool (GPU, 2ms)
 * Desktop capture: DXGI → GDI fallback
 *
 * Frame protocol (LE binary to stdout):
 *   [w:4][h:4][ch:4][size:4][BGRA pixels: size bytes]
 *   size=0 → unchanged frame.  First line: method name.
 *
 * Stdin: "q\n" → quit. Stderr: debug info.
 */
#ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
#endif
#define NOMINMAX
#include <windows.h>
#include <dwmapi.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Foundation.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <thread>
#include <atomic>
#include <io.h>
#include <fcntl.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "windowsapp.lib")

using Microsoft::WRL::ComPtr;
namespace wgc = winrt::Windows::Graphics::Capture;
namespace wf = winrt::Windows::Foundation;
static std::atomic<bool> g_running{true};
static void w32(uint32_t v) { fwrite(&v, 4, 1, stdout); }

// ── DXGI desktop capture ────────────────────────────────
static bool dxgi_cap(std::vector<uint8_t>& p, int& w, int& h) {
    ComPtr<ID3D11Device> dev; ComPtr<ID3D11DeviceContext> ctx;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx))) return false;
    ComPtr<IDXGIDevice> d; dev.As(&d);
    ComPtr<IDXGIAdapter> a; d->GetAdapter(&a);
    ComPtr<IDXGIOutput> o; a->EnumOutputs(0, &o);
    ComPtr<IDXGIOutput1> o1; o.As(&o1);
    IDXGIOutputDuplication* dup = nullptr;
    if (FAILED(o1->DuplicateOutput(dev.Get(), &dup))) return false;
    IDXGIResource* res = nullptr; DXGI_OUTDUPL_FRAME_INFO fi = {};
    if (FAILED(dup->AcquireNextFrame(16, &fi, &res))) { dup->Release(); return false; }
    ComPtr<ID3D11Texture2D> src;
    res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)src.GetAddressOf()); res->Release();
    D3D11_TEXTURE2D_DESC desc; src->GetDesc(&desc);
    D3D11_TEXTURE2D_DESC sd = {};
    sd.Width=desc.Width; sd.Height=desc.Height; sd.MipLevels=1; sd.ArraySize=1;
    sd.Format=desc.Format; sd.SampleDesc.Count=1;
    sd.Usage=D3D11_USAGE_STAGING; sd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> st;
    if (FAILED(dev->CreateTexture2D(&sd, nullptr, &st))) { dup->ReleaseFrame(); dup->Release(); return false; }
    ctx->CopyResource(st.Get(), src.Get()); src.Reset();
    dup->ReleaseFrame(); dup->Release();
    D3D11_MAPPED_SUBRESOURCE m={};
    if (FAILED(ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
    int fw=(int)desc.Width, fh=(int)desc.Height, pitch=(int)m.RowPitch;
    p.resize(fw*fh*4); uint8_t* dst=p.data(); uint8_t* s=(uint8_t*)m.pData;
    for (int y=0; y<fh; y++) memcpy(dst+y*fw*4, s+y*pitch, fw*4);
    ctx->Unmap(st.Get(),0); w=fw; h=fh; return true;
}

// ── GDI desktop fallback ────────────────────────────────
static bool gdi_desk(std::vector<uint8_t>& p, int& w, int& h) {
    HDC dc=GetDC(nullptr); if(!dc) return false;
    w=GetSystemMetrics(SM_CXSCREEN); h=GetSystemMetrics(SM_CYSCREEN);
    HDC mem=CreateCompatibleDC(dc); HBITMAP bmp=CreateCompatibleBitmap(dc,w,h);
    SelectObject(mem,bmp); BitBlt(mem,0,0,w,h,dc,0,0,SRCCOPY);
    BITMAPINFOHEADER bi={}; bi.biSize=sizeof(bi); bi.biWidth=w; bi.biHeight=-h;
    bi.biPlanes=1; bi.biBitCount=32; bi.biCompression=BI_RGB;
    p.resize(w*h*4); GetDIBits(mem,bmp,0,h,p.data(),(BITMAPINFO*)&bi,DIB_RGB_COLORS);
    DeleteObject(bmp); DeleteDC(mem); ReleaseDC(nullptr,dc); return true;
}

// ── Scale BGRA to max 640px ─────────────────────────────
static void scale_frame(const uint8_t* src, int sw, int sh, std::vector<uint8_t>& dst, int& dw, int& dh) {
    float s = (640.0f / sw);
    if (s >= 1.0f) { dw=sw; dh=sh; dst.assign(src, src+sw*sh*4); return; }
    dw=(int)(sw*s); dh=(int)(sh*s);
    dst.resize(dw*dh*4);
    for (int y=0; y<dh; y++) {
        int sy=(int)(y/s);
        for (int x=0; x<dw; x++) {
            int sx=(int)(x/s);
            memcpy(dst.data()+(y*dw+x)*4, src+(sy*sw+sx)*4, 4);
        }
    }
}

// ── Frame differ ────────────────────────────────────────
static bool frames_equal(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    if (a.size() != b.size()) return false;
    return memcmp(a.data(), b.data(), a.size()) == 0;
}

// ── FramePool (Windows.Graphics.Capture) for window ═══════════════════════
// GPU-accelerated, ~2ms per frame, works for background/minimized windows
struct FramePoolCtx {
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool pool{nullptr};
    winrt::Windows::Graphics::Capture::GraphicsCaptureSession session{nullptr};
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11Texture2D> staging;  // reused across frames
    int staging_w = 0, staging_h = 0;
    winrt::Windows::Graphics::Capture::GraphicsCaptureItem item{nullptr};
    bool ok = false;
};

static FramePoolCtx g_fp;

static bool framepool_init(HWND hwnd) {
    // Get the correct GPU adapter for the target window's monitor
    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    ComPtr<IDXGIFactory1> dxgi_factory;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)dxgi_factory.GetAddressOf()))) {
        fprintf(stderr, "[framepool] CreateDXGIFactory1 failed\n");
        return false;
    }
    ComPtr<IDXGIAdapter1> adapter;
    bool found = false;
    for (UINT i = 0; dxgi_factory->EnumAdapters1(i, adapter.GetAddressOf()) != DXGI_ERROR_NOT_FOUND; i++) {
        ComPtr<IDXGIOutput> output;
        for (UINT j = 0; adapter->EnumOutputs(j, output.GetAddressOf()) != DXGI_ERROR_NOT_FOUND; j++) {
            DXGI_OUTPUT_DESC desc;
            if (SUCCEEDED(output->GetDesc(&desc)) && desc.Monitor == mon) {
                found = true; break;
            }
            output.Reset();
        }
        if (found) break;
        adapter.Reset();
    }
    if (!found) {
        fprintf(stderr, "[framepool] monitor not found on any adapter\n");
        adapter.Reset();  // fall through to default
    }

    // Create D3D11 device on correct adapter
    D3D_DRIVER_TYPE driver_type = adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE;
    IDXGIAdapter* adapter_ptr = adapter ? adapter.Get() : nullptr;
    if (FAILED(D3D11CreateDevice(adapter_ptr, driver_type, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
        &g_fp.device, nullptr, &g_fp.ctx))) {
        fprintf(stderr, "[framepool] D3D11CreateDevice failed\n");
        return false;
    }

    // Wrap as WinRT IDirect3DDevice via interop
    ComPtr<IDXGIDevice> dxgi_dev;
    if (FAILED(g_fp.device.As(&dxgi_dev))) {
        fprintf(stderr, "[framepool] No IDXGIDevice\n");
        return false;
    }
    winrt::com_ptr<::IInspectable> d3d_inspectable;
    HRESULT hr = CreateDirect3D11DeviceFromDXGIDevice(dxgi_dev.Get(), d3d_inspectable.put());
    if (FAILED(hr)) {
        fprintf(stderr, "[framepool] CreateDirect3D11DeviceFromDXGIDevice failed 0x%08lX\n", hr);
        return false;
    }
    auto d3d_device = d3d_inspectable.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();

    // Create GraphicsCaptureItem from HWND (interop)
    auto factory = winrt::get_activation_factory<wgc::GraphicsCaptureItem>();
    auto interop = factory.as<IGraphicsCaptureItemInterop>();
    winrt::com_ptr<::IUnknown> item_unk;
    hr = interop->CreateForWindow(hwnd, winrt::guid_of<wgc::GraphicsCaptureItem>(),
        item_unk.put_void());
    if (FAILED(hr)) {
        fprintf(stderr, "[framepool] CreateForWindow failed 0x%08lX\n", hr);
        return false;
    }
    g_fp.item = item_unk.as<wgc::GraphicsCaptureItem>();
    auto size = g_fp.item.Size();
    fprintf(stderr, "[framepool] item created %dx%d\n", (int)size.Width, (int)size.Height);

    // Create FramePool (2 buffer frames)
    g_fp.pool = wgc::Direct3D11CaptureFramePool::Create(
        d3d_device,
        winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
        2, size);
    if (!g_fp.pool) {
        fprintf(stderr, "[framepool] CreateFramePool failed\n");
        return false;
    }

    // Start capture (no event needed — we poll TryGetNextFrame)
    g_fp.session = g_fp.pool.CreateCaptureSession(g_fp.item);
    g_fp.session.StartCapture();
    g_fp.ok = true;
    fprintf(stderr, "[framepool] init OK, started\n");
    return true;
}

static bool framepool_capture(std::vector<uint8_t>& pixels, int& w, int& h) {
    if (!g_fp.ok) return false;

    auto frame = g_fp.pool.TryGetNextFrame();
    if (!frame) return false;

    auto surface = frame.Surface();

    // Get ID3D11Texture2D from WinRT surface via interop
    auto access = surface.as<Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    ComPtr<ID3D11Texture2D> tex;
    HRESULT hr = access->GetInterface(__uuidof(ID3D11Texture2D), (void**)tex.GetAddressOf());
    if (FAILED(hr) || !tex) return false;

    D3D11_TEXTURE2D_DESC desc;
    tex->GetDesc(&desc);
    int fw = (int)desc.Width, fh = (int)desc.Height;

    // Reuse staging texture if size unchanged
    if (!g_fp.staging || g_fp.staging_w != fw || g_fp.staging_h != fh) {
        D3D11_TEXTURE2D_DESC sd = {};
        sd.Width = desc.Width; sd.Height = desc.Height; sd.MipLevels = 1;
        sd.ArraySize = 1; sd.Format = desc.Format;
        sd.SampleDesc.Count = 1;
        sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        g_fp.staging.Reset();
        if (FAILED(g_fp.device->CreateTexture2D(&sd, nullptr, &g_fp.staging))) return false;
        g_fp.staging_w = fw; g_fp.staging_h = fh;
    }

    g_fp.ctx->CopyResource(g_fp.staging.Get(), tex.Get());

    D3D11_MAPPED_SUBRESOURCE m = {};
    if (FAILED(g_fp.ctx->Map(g_fp.staging.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;

    int pitch = (int)m.RowPitch;
    pixels.resize(fw * fh * 4);
    uint8_t* dst = pixels.data();
    uint8_t* src = (uint8_t*)m.pData;
    for (int y = 0; y < fh; y++) memcpy(dst + y * fw * 4, src + y * pitch, fw * 4);

    g_fp.ctx->Unmap(g_fp.staging.Get(), 0);
    w = fw; h = fh;
    return true;
}

static void framepool_shutdown() {
    if (g_fp.ok) {
        g_fp.session.Close();
        g_fp.pool.Close();
        g_fp.ok = false;
        fprintf(stderr, "[framepool] shutdown\n");
    }
}

static void stdin_thread() {
    char c; while(g_running&&fread(&c,1,1,stdin)>0&&c!='q'){}
    g_running=false;
}

// ── main ────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    _setmode(_fileno(stdout),_O_BINARY); _setmode(_fileno(stdin),_O_BINARY);

    HWND hwnd=(HWND)0;
    if(argc>1) hwnd=(HWND)(ULONG_PTR)_strtoui64(argv[1],nullptr,10);
    bool desk=(hwnd==0||hwnd==GetDesktopWindow());
    fprintf(stderr,"[stream] hwnd=%p desktop=%d\n",hwnd,(int)desk);

    // Window: FramePool first (GPU, now on correct adapter), PrintWindow fallback
    const char* method = desk ? "GDI" : "FramePool";
    bool use_fp = false;
    RECT wr={};

    if (!desk) {
        use_fp = framepool_init(hwnd);
        if (!use_fp) {
            fprintf(stderr,"[stream] FramePool failed, PrintWindow fallback\n");
            method = "PrintWindow";
            DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &wr, sizeof(wr));
            if (wr.right - wr.left <= 0) GetWindowRect(hwnd, &wr);
        }
    } else {
        // Desktop: test DXGI, fallback GDI
        std::vector<uint8_t> t; int tw=0,th=0;
        if(dxgi_cap(t,tw,th)&&t.size()>=16){
            int step=(int)t.size()/400; if(step<4)step=4;
            uint8_t r0=t[2],g0=t[1],b0=t[0]; int same=0,n=0;
            for(size_t i=0;i<t.size();i+=(size_t)step*4){n++;if(t[i+2]==r0&&t[i+1]==g0&&t[i]==b0)same++;}
            if(!(n>0&&same==n)) method="DXGI";
        }
    }

    // Handshake
    fprintf(stdout, "%s\n", method); fflush(stdout);
    fprintf(stderr,"[stream] method=%s\n",method);

    std::thread(stdin_thread).detach();

    std::vector<uint8_t> prev, cur;
    int pw=0, ph=0, frames=0, skipped=0;
    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);

    while(g_running) {
        int w=0,h=0; cur.clear(); bool ok=false;

        if (use_fp) {
            ok = framepool_capture(cur, w, h);
            if (!ok) { Sleep(1); continue; }
        } else if (desk) {
            ok=dxgi_cap(cur,w,h);
            if(ok){
                int step=(int)cur.size()/400; if(step<4)step=4;
                uint8_t r0=cur[2],g0=cur[1],b0=cur[0]; int same=0,n=0;
                for(size_t i=0;i<cur.size();i+=(size_t)step*4){n++;if(cur[i+2]==r0&&cur[i+1]==g0&&cur[i]==b0)same++;}
                if(n>0&&same==n) ok=gdi_desk(cur,w,h);
            } else ok=gdi_desk(cur,w,h);
        } else {
            // PrintWindow fallback
            int ww=wr.right-wr.left, wh=wr.bottom-wr.top;
            if(ww>0&&wh>0){
                HDC screen=GetDC(nullptr); if(screen){
                HDC mem=CreateCompatibleDC(screen);
                HBITMAP bmp=CreateCompatibleBitmap(screen,ww,wh);
                SelectObject(mem,bmp);
                RECT fill={0,0,ww,wh}; HBRUSH mBrush=CreateSolidBrush(RGB(255,0,255));
                FillRect(mem,&fill,mBrush); DeleteObject(mBrush);
                PrintWindow(hwnd,mem,PW_RENDERFULLCONTENT|PW_CLIENTONLY);
                BITMAPINFOHEADER bi={}; bi.biSize=sizeof(bi); bi.biWidth=ww; bi.biHeight=-wh;
                bi.biPlanes=1; bi.biBitCount=32; bi.biCompression=BI_RGB;
                cur.resize(ww*wh*4);
                GetDIBits(mem,bmp,0,wh,cur.data(),(BITMAPINFO*)&bi,DIB_RGB_COLORS);
                SelectObject(mem,(HBITMAP)GetStockObject(NULL_BRUSH)); DeleteObject(bmp);
                DeleteDC(mem); ReleaseDC(nullptr,screen);
                if(cur.size()>=16){
                    uint8_t r0=cur[2],g0=cur[1],b0=cur[0]; int same=0,n=0,step=(int)cur.size()/400; if(step<4)step=4;
                    for(size_t i=0;i<cur.size();i+=(size_t)step*4){n++;if(cur[i+2]==r0&&cur[i+1]==g0&&cur[i]==b0)same++;}
                    ok=!(n>0&&same==n);
                }}
            }
            if(!ok){ // DXGI crop
                std::vector<uint8_t> full; int fw=0,fh=0;
                if(dxgi_cap(full,fw,fh)){
                    int cx=wr.left>0?wr.left:0, cy=wr.top>0?wr.top:0;
                    int cw=ww<(fw-cx)?ww:(fw-cx), ch=wh<(fh-cy)?wh:(fh-cy);
                    if(cw>0&&ch>0){cur.resize(cw*ch*4);
                        for(int y=0;y<ch;y++){int si=((cy+y)*fw+cx)*4; memcpy(cur.data()+y*cw*4,full.data()+si,cw*4);}
                        w=cw; h=ch; ok=true;}}
            } else { w=ww; h=wh; }
        }

        if(!ok||w<=0||h<=0){Sleep(1); continue;}

        std::vector<uint8_t> scaled; int sw=0, sh=0;
        scale_frame(cur.data(), w, h, scaled, sw, sh);

        if(sw==pw && sh==ph && frames_equal(prev, scaled)) {
            w32((uint32_t)sw); w32((uint32_t)sh); w32(4); w32(0);
            fflush(stdout); skipped++;
        } else {
            uint32_t sz=(uint32_t)(sw*sh*4);
            w32((uint32_t)sw); w32((uint32_t)sh); w32(4); w32(sz);
            fwrite(scaled.data(),1,sz,stdout); fflush(stdout);
            prev.swap(scaled); pw=sw; ph=sh; frames++;
        }
        // Timing log every 60 frames
        if (frames > 0 && frames % 60 == 0) {
            QueryPerformanceCounter(&t1);
            double elapsed = (double)(t1.QuadPart - t0.QuadPart) / freq.QuadPart;
            fprintf(stderr, "[stream] %d frames in %.2fs = %.1f fps (method=%s)\n",
                frames, elapsed, frames/elapsed, method);
        }
        Sleep(1);
    }

    framepool_shutdown();
    fprintf(stderr,"[stream] exit: %d frames, %d skipped\n",frames,skipped);
    return 0;
}
