/**
 * mjpeg_server.cpp — MJPEG HTTP server (Winsock2 + WIC JPEG encode).
 *
 * Port 9998. Multipart/x-mixed-replace streaming to multiple clients.
 */
#include "mjpeg_server.h"
#include <winsock2.h>
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <cstdio>

using Microsoft::WRL::ComPtr;

// ── WIC factory (one-time init) ────────────────────────────
static ComPtr<IWICImagingFactory> g_wic;

// ── Client list ────────────────────────────────────────────
struct Client {
    SOCKET sock;
    std::thread thread;
    std::atomic<bool> active{true};
};
static std::vector<Client*> g_clients;
static std::mutex g_clients_mutex;
static std::atomic<bool> g_running{false};
static SOCKET g_listen = INVALID_SOCKET;
static std::thread g_accept_thread;

// ── JPEG encode (WIC) ──────────────────────────────────────
static std::vector<uint8_t> g_last_jpeg;
static std::mutex g_frame_mutex;
static int g_last_w = 0, g_last_h = 0;

static bool bgra_to_jpeg(const uint8_t* bgra, int w, int h,
                         std::vector<uint8_t>& out, float quality = 0.70f) {
    if (!g_wic) return false;

    ComPtr<IWICBitmap> bitmap;
    HRESULT hr = g_wic->CreateBitmapFromMemory(
        (UINT)w, (UINT)h, GUID_WICPixelFormat32bppBGRA,
        (UINT)(w * 4), (UINT)(w * h * 4), (BYTE*)bgra, &bitmap);
    if (FAILED(hr)) return false;

    ComPtr<IStream> stream;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream))) return false;

    ComPtr<IWICBitmapEncoder> encoder;
    hr = g_wic->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder);
    if (FAILED(hr)) return false;
    encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);

    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    encoder->CreateNewFrame(&frame, &props);

    if (props) {
        PROPBAG2 opt = {};
        opt.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
        VARIANT v; VariantInit(&v);
        v.vt = VT_R4; v.fltVal = quality;
        props->Write(1, &opt, &v);
    }

    frame->Initialize(props.Get());
    frame->SetSize((UINT)w, (UINT)h);
    frame->WriteSource(bitmap.Get(), nullptr);
    frame->Commit();
    encoder->Commit();

    STATSTG stat;
    stream->Stat(&stat, STATFLAG_NONAME);
    ULONG size = stat.cbSize.LowPart;
    out.resize(size);

    LARGE_INTEGER li = {};
    stream->Seek(li, STREAM_SEEK_SET, nullptr);
    stream->Read(out.data(), size, nullptr);
    return true;
}

// ── Client handler ─────────────────────────────────────────
static void client_handler(Client* c) {
    // Send HTTP header
    const char* header =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n"
        "Pragma: no-cache\r\n"
        "Access-Control-Allow-Origin: *\r\n\r\n";
    send(c->sock, header, (int)strlen(header), 0);

    while (c->active && g_running) {
        std::vector<uint8_t> jpeg;
        int w, h;
        {
            std::lock_guard<std::mutex> lk(g_frame_mutex);
            jpeg = g_last_jpeg;
            w = g_last_w; h = g_last_h;
        }
        if (jpeg.empty()) {
            Sleep(16); // ~60fps wait
            continue;
        }

        char boundary[256];
        int blen = snprintf(boundary, sizeof(boundary),
            "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %zu\r\n\r\n",
            jpeg.size());
        if (send(c->sock, boundary, blen, 0) == SOCKET_ERROR) break;
        if (send(c->sock, (const char*)jpeg.data(), (int)jpeg.size(), 0) == SOCKET_ERROR) break;
        if (send(c->sock, "\r\n", 2, 0) == SOCKET_ERROR) break;

        Sleep(16);
    }

    closesocket(c->sock);
    c->sock = INVALID_SOCKET;
}

// ── Accept thread ──────────────────────────────────────────
static void accept_loop() {
    while (g_running) {
        SOCKET client = accept(g_listen, nullptr, nullptr);
        if (client == INVALID_SOCKET) {
            if (g_running) { Sleep(100); continue; }
            break;
        }

        auto* c = new Client{client};
        {
            std::lock_guard<std::mutex> lk(g_clients_mutex);
            g_clients.push_back(c);
        }

        c->thread = std::thread(client_handler, c);
        c->thread.detach();
    }

    // Cleanup disconnected clients
    std::lock_guard<std::mutex> lk(g_clients_mutex);
    for (auto it = g_clients.begin(); it != g_clients.end(); ) {
        if (!(*it)->active) { delete *it; it = g_clients.erase(it); }
        else ++it;
    }
}

// ── Public API ─────────────────────────────────────────────
bool mjpeg_server_start() {
    // Init WIC
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                     IID_PPV_ARGS(&g_wic));
    if (!g_wic) {
        fprintf(stderr, "[mjpeg] WIC factory init failed\n");
        return false;
    }

    // Init Winsock
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;

    g_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_listen == INVALID_SOCKET) { WSACleanup(); return false; }

    int reuse = 1;
    setsockopt(g_listen, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(9998);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(g_listen, (sockaddr*)&addr, sizeof(addr)) != 0) {
        closesocket(g_listen);
        WSACleanup();
        return false;
    }
    listen(g_listen, SOMAXCONN);

    g_running = true;
    g_accept_thread = std::thread(accept_loop);
    fprintf(stderr, "[mjpeg] server started on port 9998\n");
    return true;
}

void mjpeg_server_stop() {
    g_running = false;

    // Wait for clients to finish before releasing resources
    {
        std::lock_guard<std::mutex> lk(g_clients_mutex);
        for (auto* c : g_clients) {
            c->active = false;
            if (c->sock != INVALID_SOCKET) closesocket(c->sock);
        }
    }
    Sleep(50); // brief wait for client threads to notice g_running=false

    // Wake accept() by connecting to ourselves
    SOCKET poke = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(9998);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (poke != INVALID_SOCKET) {
        connect(poke, (sockaddr*)&addr, sizeof(addr));
        closesocket(poke);
    }

    if (g_listen != INVALID_SOCKET) {
        closesocket(g_listen);
        g_listen = INVALID_SOCKET;
    }

    if (g_accept_thread.joinable()) g_accept_thread.join();

    std::lock_guard<std::mutex> lk(g_clients_mutex);
    for (auto* c : g_clients) {
        c->active = false;
        if (c->sock != INVALID_SOCKET) closesocket(c->sock);
    }
    g_clients.clear();

    g_wic = nullptr;
    WSACleanup();
    fprintf(stderr, "[mjpeg] server stopped\n");
}

void mjpeg_server_push_frame(const uint8_t* pixels, int w, int h) {
    std::vector<uint8_t> jpeg;
    if (!bgra_to_jpeg(pixels, w, h, jpeg, 0.70f)) return;

    std::lock_guard<std::mutex> lk(g_frame_mutex);
    g_last_jpeg = std::move(jpeg);
    g_last_w = w;
    g_last_h = h;
}

bool mjpeg_server_has_clients() {
    std::lock_guard<std::mutex> lk(g_clients_mutex);
    return !g_clients.empty();
}
