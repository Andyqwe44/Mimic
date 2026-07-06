/**
 * capture_stream.exe — persistent capture, frame-differenced stream
 *
 * Usage:   capture_stream.exe <hwnd>
 *
 * Window capture: WGC FramePool via shared WgcCapture library (GPU, 2ms)
 * Desktop capture: DXGI (configurable) → GDI fallback
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
#define _SILENCE_EXPERIMENTAL_COROUTINE_DEPRECATION_WARNINGS
#include <windows.h>
#include <dwmapi.h>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <thread>
#include <atomic>
#include <io.h>
#include <fcntl.h>

#include "capture.hpp"
#include "../include/capture_wgc.hpp"
#include "../../common/include/capture_helpers.hpp"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "windowsapp.lib")

namespace ch = capture_helpers;
static std::atomic<bool> g_running{true};

// ── Frame output helpers ────────────────────────────────
static void emit_unchanged(int pw, int ph) {
    uint8_t buf[16];
    ch::w32_le(buf,     (uint32_t)pw);
    ch::w32_le(buf + 4, (uint32_t)ph);
    ch::w32_le(buf + 8, 4);
    ch::w32_le(buf + 12, 0);
    fwrite(buf, 1, 16, stdout);
}

static void emit_frame(const uint8_t* data, int w, int h) {
    uint8_t buf[16];
    uint32_t sz = (uint32_t)(w * h * 4);
    ch::w32_le(buf,     (uint32_t)w);
    ch::w32_le(buf + 4, (uint32_t)h);
    ch::w32_le(buf + 8, 4);
    ch::w32_le(buf + 12, sz);
    fwrite(buf, 1, 16, stdout);
    fwrite(data, 1, sz, stdout);
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

    // Use shared WgcCapture library for window capture
    wgc::WgcCapture wgc_cap;
    // Use shared DxgiCapture with options for desktop
    DxgiOptions dxgi_opts;
    dxgi_opts.skip_solid_outputs = true;
    dxgi_opts.min_output_width  = 320;
    auto dxgi_backend = create_capture_backend(dxgi_opts);
    auto gdi_backend  = create_capture_backend();  // GDI fallback (factory auto-falls-back)

    const char* method = desk ? "GDI" : "WGC";
    bool use_wgc = false;
    RECT wr={};

    if (!desk) {
        use_wgc = wgc_cap.init(hwnd);
        if (!use_wgc) {
            fprintf(stderr,"[stream] WGC failed, PrintWindow fallback\n");
            method = "PrintWindow";
            DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &wr, sizeof(wr));
            if (wr.right - wr.left <= 0) GetWindowRect(hwnd, &wr);
        }
    } else {
        // Desktop: test DXGI, fallback GDI
        if (dxgi_backend) {
            FrameBuffer fb;
            if (dxgi_backend->capture(fb) && !ch::is_solid_color(fb.data.data(), fb.data.size()))
                method = "DXGI";
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

        if (use_wgc) {
            wgc::WgcFrame wf;
            ok = wgc_cap.capture(wf);
            if (ok) {
                cur = std::move(wf.pixels);
                w = wf.width; h = wf.height;
            } else {
                if (pw > 0 && ph > 0) {
                    emit_unchanged(pw, ph);
                    fflush(stdout); skipped++;
                }
                Sleep(1); continue;
            }
        } else if (desk) {
            // Use shared backends
            if (method && strcmp(method, "DXGI") == 0 && dxgi_backend) {
                FrameBuffer fb;
                ok = dxgi_backend->capture(fb);
                if (ok && !ch::is_solid_color(fb.data.data(), fb.data.size())) {
                    cur = std::move(fb.data);
                    w = fb.width; h = fb.height;
                } else { ok = false; }
            }
            if (!ok && gdi_backend) {
                FrameBuffer fb;
                ok = gdi_backend->capture(fb);
                if (ok) { cur = std::move(fb.data); w = fb.width; h = fb.height; }
            }
        } else {
            // PrintWindow fallback for window capture
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
                ok = !ch::is_solid_color(cur.data(), cur.size())
                  && !ch::has_magenta_sentinel(cur.data(), cur.size());
                }}
            }
            if(!ok && dxgi_backend){
                FrameBuffer fb;
                if(dxgi_backend->capture(fb)){
                    int cx=wr.left>0?wr.left:0, cy=wr.top>0?wr.top:0;
                    int cw=ww<(fb.width-cx)?ww:(fb.width-cx), ch_=wh<(fb.height-cy)?wh:(fb.height-cy);
                    if(cw>0&&ch_>0){cur.resize(cw*ch_*4);
                        for(int y=0;y<ch_;y++){int si=((cy+y)*fb.width+cx)*4; memcpy(cur.data()+y*cw*4,fb.data.data()+si,cw*4);}
                        w=cw; h=ch_; ok=true;}}
            } else if (ok) { w=ww; h=wh; }
        }

        if(!ok||w<=0||h<=0){Sleep(1); continue;}

        // Scale BGRA using shared helper
        auto [scaled_px, dims] = ch::scale_bgra(cur.data(), w, h, 640);
        int sw = dims.first, sh = dims.second;

        // Frame differ using shared helper
        if(sw==pw && sh==ph && ch::frames_equal(prev.data(), scaled_px.data(), scaled_px.size())) {
            emit_unchanged(sw, sh);
            fflush(stdout); skipped++;
        } else {
            emit_frame(scaled_px.data(), sw, sh);
            fflush(stdout);
            prev.swap(scaled_px); pw=sw; ph=sh; frames++;
        }
        if (frames > 0 && frames % 60 == 0) {
            QueryPerformanceCounter(&t1);
            double elapsed = (double)(t1.QuadPart - t0.QuadPart) / freq.QuadPart;
            fprintf(stderr, "[stream] %d frames in %.2fs = %.1f fps (method=%s)\n",
                frames, elapsed, frames/elapsed, method);
        }
        Sleep(1);
    }

    wgc_cap.shutdown();
    fprintf(stderr,"[stream] exit: %d frames, %d skipped\n",frames,skipped);
    return 0;
}
