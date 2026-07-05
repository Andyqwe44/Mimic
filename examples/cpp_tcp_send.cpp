/**
 * 示例: C++ 捕获桌面 → TCP :9999 广播 → Python 接收
 *
 * 编译:
 *   cl.exe /EHsc /std:c++17 /I ../common/include cpp_tcp_send.cpp
 *         d3d11.lib dxgi.lib user32.lib gdi32.lib ws2_32.lib
 *
 * 运行:
 *   终端1: ./cpp_tcp_send.exe
 *   终端2: python examples/python_tcp_recv.py
 */
#include <windows.h>
#include <cstdio>
#include <vector>
#include "cpp_sender.hpp"

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
    bi.biWidth = w; bi.biHeight = -h;
    bi.biPlanes = 1; bi.biBitCount = 32; bi.biCompression = BI_RGB;

    std::vector<uint8_t> pixels(w * h * 4);
    GetDIBits(mem, bmp, 0, h, pixels.data(), (BITMAPINFO*)&bi, DIB_RGB_COLORS);

    SelectObject(mem, old); DeleteObject(bmp); DeleteDC(mem);
    ReleaseDC(nullptr, dc);
    return pixels;
}

int main() {
    TcpFrameSender sender;
    if (!sender.listen(stream_protocol::DEFAULT_TCP_PORT)) {
        fprintf(stderr, "listen failed\n");
        return 1;
    }

    printf("[cpp_tcp_send] broadcasting desktop at 5fps on :%d...\n",
           stream_protocol::DEFAULT_TCP_PORT);
    printf("              run: python examples/python_tcp_recv.py\n");

    int frames = 0;
    while (true) {
        // Accept new clients (non-blocking)
        sender.accept_clients();

        // Capture + scale
        int w, h;
        auto full = capture_desktop(w, h);

        float s = 640.0f / w;
        int sw = w, sh = h;
        std::vector<uint8_t> scaled;
        if (s >= 1.0f) { scaled = full; }
        else {
            sw = (int)(w * s); sh = (int)(h * s);
            scaled.resize(sw * sh * 4);
            for (int y = 0; y < sh; y++) {
                int sy = (int)(y / s);
                for (int x = 0; x < sw; x++) {
                    int sx = (int)(x / s);
                    memcpy(scaled.data() + (y*sw+x)*4, full.data() + (sy*w+sx)*4, 4);
                }
            }
        }

        // Broadcast to all connected clients
        sender.broadcast_frame(scaled, sw, sh);

        fprintf(stderr, "[cpp] frame %d: %dx%d\n", ++frames, sw, sh);
        Sleep(200); // 5fps
    }

    return 0;
}
