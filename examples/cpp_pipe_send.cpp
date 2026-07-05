/**
 * 示例: C++ 捕获桌面 → stdout pipe 发送 → Rust 接收
 *
 * 编译:
 *   cl.exe /EHsc /std:c++17 /I ../common/include cpp_pipe_send.cpp
 *         d3d11.lib dxgi.lib user32.lib gdi32.lib
 *
 * 运行 (bash):
 *   ./cpp_pipe_send.exe | cargo run --example rust_pipe_recv
 *   或
 *   ./cpp_pipe_send.exe > pipe.dat
 *   cat pipe.dat | ./rust_pipe_recv
 */
#include <windows.h>
#include <cstdio>
#include <vector>
#include "cpp_sender.hpp"

// GDI 截图: 获取整个桌面的 BGRA 像素
std::vector<uint8_t> capture_desktop(int& w, int& h) {
    HDC dc = GetDC(nullptr);
    w = GetSystemMetrics(SM_CXSCREEN);
    h = GetSystemMetrics(SM_CYSCREEN);
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, w, h);
    HBITMAP old = (HBITMAP)SelectObject(mem, bmp);
    BitBlt(mem, 0, 0, w, h, dc, 0, 0, SRCCOPY);

    BITMAPINFOHEADER bi = {};
    bi.biSize = sizeof(bi);
    bi.biWidth = w; bi.biHeight = -h; // top-down
    bi.biPlanes = 1; bi.biBitCount = 32; bi.biCompression = BI_RGB;

    std::vector<uint8_t> pixels(w * h * 4);
    GetDIBits(mem, bmp, 0, h, pixels.data(), (BITMAPINFO*)&bi, DIB_RGB_COLORS);

    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, dc);
    return pixels;
}

int main() {
    PipeFrameSender sender;

    // 缩放到 640px
    auto scale_bgra = [](const std::vector<uint8_t>& src, int sw, int sh,
                          std::vector<uint8_t>& dst, int& dw, int& dh) {
        float s = 640.0f / sw;
        if (s >= 1.0f) { dw = sw; dh = sh; dst = src; return; }
        dw = (int)(sw * s); dh = (int)(sh * s);
        dst.resize(dw * dh * 4);
        for (int y = 0; y < dh; y++) {
            int sy = (int)(y / s);
            for (int x = 0; x < dw; x++) {
                int sx = (int)(x / s);
                memcpy(dst.data() + (y*dw+x)*4, src.data() + (sy*sw+sx)*4, 4);
            }
        }
    };

    printf("[cpp_pipe_send] capturing desktop at 5fps... (Ctrl+C to stop)\n");

    int frames = 0;
    while (frames < 100) {  // 发100帧后退出
        int w, h;
        auto full = capture_desktop(w, h);

        std::vector<uint8_t> scaled;
        int sw, sh;
        scale_bgra(full, w, h, scaled, sw, sh);

        if (!sender.send_frame(scaled, sw, sh)) {
            fprintf(stderr, "pipe write failed\n");
            break;
        }

        fprintf(stderr, "[cpp] frame %d: %dx%d\n", ++frames, sw, sh);
        Sleep(200);  // 5fps
    }

    sender.send_unchanged(); // 发送结束信号
    printf("[cpp_pipe_send] done\n");
    return 0;
}
