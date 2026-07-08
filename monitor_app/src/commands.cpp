/**
 * commands.cpp — Backend command dispatch (replaces Rust main.rs commands).
 *
 * WebMessage JSON → dispatch_command → FFI/lib calls → JSON response.
 */
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include "commands.h"
#include "json_helper.h"
#include "mjpeg_server.h"
#include "../../logger/logger.h"
#include "../../capture/include/capture_methods.h"
#include "../../capture/include/capture_wgc_ffi.h"
#include <shobjidl.h>  // IVirtualDesktopManager
#include <shellapi.h>  // ShellExecuteA
#include <windows.h>
#include <tlhelp32.h>
#include <dwmapi.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using Microsoft::WRL::ComPtr;

// Shared by main.cpp — pushed from stream thread
extern void shared_buffer_push_frame(const uint8_t* bgra, int w, int h);

static constexpr int MAX_PX = 3840 * 2160 * 4;

// ── base64 ─────────────────────────────────────────────────
static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string base64_encode(const uint8_t* data, size_t len) {
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t n = (uint32_t)data[i] << 16;
        if (i + 1 < len) n |= (uint32_t)data[i + 1] << 8;
        if (i + 2 < len) n |= data[i + 2];
        out.push_back(B64[(n >> 18) & 63]);
        out.push_back(B64[(n >> 12) & 63]);
        out.push_back(i + 1 < len ? B64[(n >> 6) & 63] : '=');
        out.push_back(i + 2 < len ? B64[n & 63] : '=');
    }
    return out;
}

// ── WIC helpers ────────────────────────────────────────────
static ComPtr<IWICImagingFactory> g_wic;

static bool init_wic() {
    if (g_wic) return true;
    return SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_wic)));
}

// BGRA pixels → PNG bytes
static bool bgra_to_png(const uint8_t* bgra, int w, int h, std::vector<uint8_t>& out) {
    if (!init_wic()) return false;

    ComPtr<IWICBitmap> bitmap;
    if (FAILED(g_wic->CreateBitmapFromMemory((UINT)w, (UINT)h,
        GUID_WICPixelFormat32bppBGRA, (UINT)(w * 4), (UINT)(w * h * 4),
        (BYTE*)bgra, &bitmap))) return false;

    ComPtr<IStream> stream;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream))) return false;

    ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(g_wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder))) return false;
    encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);

    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    encoder->CreateNewFrame(&frame, &props);
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

// ── JSON escaping ──────────────────────────────────────────
static std::string json_escape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (char c : s) {
        if (c == '"') o += "\\\"";
        else if (c == '\\') o += "\\\\";
        else if (c == '\n') o += "\\n";
        else if (c == '\r') o += "\\r";
        else if (c == '\t') o += "\\t";
        else o.push_back(c);
    }
    return o;
}

// ── list_windows ──────────────────────────────────────────
struct WindowInfo { std::string title, category; uint64_t hwnd; };
static std::vector<WindowInfo> g_winlist;
static std::mutex g_winlist_mutex;

struct EnumContext {
    std::vector<WindowInfo>* list;
    IVirtualDesktopManager* vdm;
    std::vector<GUID>* desktop_ids;  // accumulate unique desktop GUIDs
};

static BOOL CALLBACK enum_callback(HWND hwnd, LPARAM lparam) {
    auto* ctx = reinterpret_cast<EnumContext*>(lparam);
    auto* list = ctx->list;

    LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (!(style & WS_CAPTION)) return TRUE;
    if (ex & WS_EX_TOOLWINDOW) return TRUE;

    // Check virtual desktop — allow windows on any desktop
    BOOL on_current = TRUE;
    GUID desktop_id = {};
    if (ctx->vdm) {
        HRESULT hr = ctx->vdm->IsWindowOnCurrentVirtualDesktop(hwnd, &on_current);
        if (FAILED(hr)) on_current = TRUE; // assume current if API fails
        ctx->vdm->GetWindowDesktopId(hwnd, &desktop_id);
    }

    // Visibility: only filter if on CURRENT desktop (windows on other desktops
    // appear invisible/cloaked, but we still want to list them)
    if (on_current) {
        if (!IsWindowVisible(hwnd)) return TRUE;

        // Cloaked check (only for current desktop)
        BOOL cloaked = FALSE;
        DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
        if (cloaked) return TRUE;
    }

    RECT r;
    if (!GetWindowRect(hwnd, &r) || r.right <= r.left || r.bottom <= r.top) return TRUE;

    // No owner
    if (GetWindow(hwnd, GW_OWNER)) return TRUE;

    wchar_t buf[256];
    int len = GetWindowTextW(hwnd, buf, 256);
    if (len == 0) return TRUE;
    std::wstring ws(buf, len);
    // trim
    while (!ws.empty() && ws.back() == L' ') ws.pop_back();
    while (!ws.empty() && ws.front() == L' ') ws.erase(0, 1);
    if (ws.empty() || ws == L"Program Manager") return TRUE;

    int ulen = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), nullptr, 0, nullptr, nullptr);
    std::string title(ulen, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), &title[0], ulen, nullptr, nullptr);

    // Label windows on non-current desktops
    if (!on_current && ctx->desktop_ids) {
        // Find or assign desktop index
        int desktop_idx = -1;
        for (size_t i = 0; i < ctx->desktop_ids->size(); i++) {
            if (IsEqualGUID((*ctx->desktop_ids)[i], desktop_id)) {
                desktop_idx = (int)i + 1; break;
            }
        }
        if (desktop_idx < 0) {
            ctx->desktop_ids->push_back(desktop_id);
            desktop_idx = (int)ctx->desktop_ids->size();
        }
        if (desktop_idx > 1) { // desktop 1 = current, no label needed
            title += " (Desktop " + std::to_string(desktop_idx) + ")";
        }
    }

    list->push_back({title, "window", (uint64_t)(uintptr_t)hwnd});
    return TRUE;
}

static std::string cmd_list_windows() {
    std::vector<WindowInfo> list;
    list.push_back({" Entire Desktop", "desktop", 0});

    // Create VirtualDesktopManager for cross-desktop window enumeration
    IVirtualDesktopManager* vdm = nullptr;
    CoCreateInstance(CLSID_VirtualDesktopManager, nullptr, CLSCTX_INPROC_SERVER,
                     IID_PPV_ARGS(&vdm));

    std::vector<GUID> desktop_ids;
    EnumContext ctx = {&list, vdm, &desktop_ids};
    EnumWindows(enum_callback, (LPARAM)&ctx);

    if (vdm) vdm->Release();

    LOG("cmd", "list_windows: %zu entries", list.size());

    std::string json = "[";
    for (size_t i = 0; i < list.size(); i++) {
        if (i > 0) json += ",";
        char buf[512];
        snprintf(buf, sizeof(buf), R"({"title":"%s","category":"%s","hwnd":%llu})",
                 json_escape(list[i].title).c_str(), list[i].category.c_str(), (unsigned long long)list[i].hwnd);
        json += buf;
    }
    json += "]";
    return json;
}

// ── list_processes ────────────────────────────────────────
static std::string cmd_list_processes() {
    std::string json = "[";
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe = {sizeof(PROCESSENTRY32W)};
        bool first = true;
        if (Process32FirstW(snap, &pe)) {
            do {
                std::wstring ws(pe.szExeFile);
                ws.resize(wcslen(pe.szExeFile));
                if (ws.empty()) continue;
                int ulen = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), nullptr, 0, nullptr, nullptr);
                std::string name(ulen, '\0');
                WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), &name[0], ulen, nullptr, nullptr);

                if (!first) json += ","; first = false;
                char buf[384];
                snprintf(buf, sizeof(buf), R"({"title":"%s","category":"process","hwnd":%lu})",
                         name.c_str(), pe.th32ProcessID);
                json += buf;
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
    }
    json += "]";
    LOG("cmd", "list_processes: done");
    return json;
}

// ── Capture dispatch ──────────────────────────────────────
struct CaptureResult { std::vector<uint8_t> pixels; int w, h; std::string method; };

static CaptureResult call_capture(uint64_t hwnd, const std::string& method) {
    std::vector<uint8_t> buf(MAX_PX);
    int w = 0, h = 0, size = 0;
    HWND hw = (HWND)(uintptr_t)hwnd;
    std::string used = method;

    if (method == "WGC") {
        size = wgc_capture_single(hw, buf.data(), MAX_PX, &w, &h, nullptr);
    } else if (method == "GDI(GetWindowDC)") {
        size = capture_gdi_getwindowdc(hw, buf.data(), MAX_PX, &w, &h);
    } else if (method == "PrintWindow") {
        size = capture_printwindow(hw, buf.data(), MAX_PX, &w, &h);
    } else if (method == "ScreenBitBlt") {
        size = capture_screen_bitblt(hw, buf.data(), MAX_PX, &w, &h);
    } else if (method == "DesktopBlt") {
        size = capture_desktop_bitblt(buf.data(), MAX_PX, &w, &h); // hwnd unused
    } else if (method == "dxgi") {
        // DXGI Desktop Duplication — map to DesktopBlt for single frame
        // (DXGI streaming uses dedicated backend in capture_dxgi.cpp)
        size = capture_desktop_bitblt(buf.data(), MAX_PX, &w, &h);
    } else {
        // fallback chain
        const char* chain[] = {"DesktopBlt", "GDI(GetWindowDC)", "PrintWindow", "ScreenBitBlt"};
        for (auto* m : chain) {
            auto r = call_capture(hwnd, m);
            if (r.w > 0 && r.h > 0) return r;
        }
        used = "ALL_FAILED";
    }

    if (size > 0 && w > 0 && h > 0) {
        buf.resize((size_t)size);
        return {buf, w, h, used};
    }
    return {{}, 0, 0, "ALL_FAILED"};
}

// ── BGRA→RGBA→scale→PNG→base64 for single frame ──────────
static std::string frame_to_json(const CaptureResult& r, int x, int y, int sw, int sh, double total_ms) {
    // Scale to max 640px wide
    float scale = std::min(640.0f / r.w, 1.0f);
    int sw2 = (int)(r.w * scale), sh2 = (int)(r.h * scale);
    std::vector<uint8_t> rgba(sw2 * sh2 * 4);

    for (int py = 0; py < sh2; py++) {
        int sy = (int)(py / scale);
        for (int px = 0; px < sw2; px++) {
            int sx = (int)(px / scale);
            int di = (py * sw2 + px) * 4;
            int si = (sy * r.w + sx) * 4;
            rgba[di]   = r.pixels[si + 2]; // B→R
            rgba[di+1] = r.pixels[si + 1]; // G
            rgba[di+2] = r.pixels[si];     // R→B
            rgba[di+3] = 255;
        }
    }

    std::vector<uint8_t> png;
    if (!bgra_to_png(rgba.data(), sw2, sh2, png)) return "{}";

    std::string b64 = base64_encode(png.data(), png.size());

    std::string json = "{\"image\":\"" + b64 + "\",\"w\":" + std::to_string(r.w) +
        ",\"h\":" + std::to_string(r.h) + ",\"x\":" + std::to_string(x) +
        ",\"y\":" + std::to_string(y) + ",\"screen_w\":" + std::to_string(sw) +
        ",\"screen_h\":" + std::to_string(sh) + ",\"method\":\"" + r.method + "\"}";
    return json;
}

static std::string cmd_capture_window(uint64_t hwnd, const std::string& method) {
    auto r = call_capture(hwnd, method.empty() ? "auto" : method);
    if (r.w <= 0 || r.h <= 0) {
        LOG("cmd", "capture_window: FAILED hwnd=%llu", (unsigned long long)hwnd);
        return "{}";
    }

    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    int wx = 0, wy = 0;
    if (hwnd != 0) {
        RECT wr;
        if (GetWindowRect((HWND)(uintptr_t)hwnd, &wr)) { wx = wr.left; wy = wr.top; }
    }

    auto json = frame_to_json(r, wx, wy, sw, sh, 0);
    LOG("cmd", "capture_window: %dx%d method=%s → %zub", r.w, r.h, r.method.c_str(), json.size());
    return json;
}

// ── Stream management ─────────────────────────────────────
static std::atomic<bool> g_streaming{false};
static std::thread g_stream_thread;
static WgcStreamHandle* g_stream_handle = nullptr;
static std::mutex g_stream_frame_mutex;
static std::vector<uint8_t> g_stream_frame_pixels;
static int g_stream_frame_w = 0, g_stream_frame_h = 0;

// ── TCP broadcast server (port 9999, wire protocol) ──────
static std::mutex g_tcp_mutex;
static std::vector<SOCKET> g_tcp_clients;
static SOCKET g_tcp_listen = INVALID_SOCKET;
static std::thread g_tcp_accept_thread;
static std::atomic<bool> g_tcp_running{false};

static void tcp_accept_loop() {
    while (g_tcp_running) {
        SOCKET c = accept(g_tcp_listen, nullptr, nullptr);
        if (c == INVALID_SOCKET) {
            if (g_tcp_running) { Sleep(100); continue; }
            else break;
        }
        int flag = 1;
        setsockopt(c, IPPROTO_TCP, TCP_NODELAY, (const char*)&flag, sizeof(flag));
        std::lock_guard<std::mutex> lk(g_tcp_mutex);
        g_tcp_clients.push_back(c);
    }
}

static bool tcp_server_start() {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    g_tcp_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_tcp_listen == INVALID_SOCKET) { WSACleanup(); g_tcp_listen = INVALID_SOCKET; return false; }
    int reuse = 1;
    setsockopt(g_tcp_listen, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(9999);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(g_tcp_listen, (sockaddr*)&addr, sizeof(addr)) != 0) {
        closesocket(g_tcp_listen); g_tcp_listen = INVALID_SOCKET; WSACleanup(); return false;
    }
    listen(g_tcp_listen, SOMAXCONN);
    g_tcp_running = true;
    g_tcp_accept_thread = std::thread(tcp_accept_loop);
    LOG("cmd", "TCP server started on port 9999");
    return true;
}

static void tcp_server_stop() {
    g_tcp_running = false;
    if (g_tcp_listen != INVALID_SOCKET) { closesocket(g_tcp_listen); g_tcp_listen = INVALID_SOCKET; }
    if (g_tcp_accept_thread.joinable()) g_tcp_accept_thread.join();
    std::lock_guard<std::mutex> lk(g_tcp_mutex);
    for (auto s : g_tcp_clients) closesocket(s);
    g_tcp_clients.clear();
    WSACleanup();
}

static void tcp_broadcast_frame(const uint8_t* bgra, int w, int h) {
    // Wire protocol: magic(4) + body_size(4 LE) + type_tag(4 LE) + body
    // type_tag 1 = BGRA: w(4)+h(4)+ch(4)+reserved(4)+pixels(w*h*ch)
    uint32_t magic = 0x4D415246; // "FRAM"
    uint32_t body_size = 16 + (uint32_t)(w * h * 4); // 12 header + pixels
    uint32_t type_tag = 1;
    uint32_t zero = 0;

    char hdr[12];
    memcpy(hdr, &magic, 4);
    memcpy(hdr + 4, &body_size, 4);
    memcpy(hdr + 8, &type_tag, 4);

    uint32_t frame_hdr[4] = {(uint32_t)w, (uint32_t)h, 4u, 0u};

    std::lock_guard<std::mutex> lk(g_tcp_mutex);
    for (auto it = g_tcp_clients.begin(); it != g_tcp_clients.end(); ) {
        if (send(*it, hdr, 12, 0) == SOCKET_ERROR ||
            send(*it, (const char*)frame_hdr, 16, 0) == SOCKET_ERROR ||
            send(*it, (const char*)bgra, w * h * 4, 0) == SOCKET_ERROR) {
            closesocket(*it);
            it = g_tcp_clients.erase(it);
        } else { ++it; }
    }
}

static std::string cmd_capture_stream_stop(); // fwd decl for cmd_capture_stream_start

static std::string cmd_capture_stream_start(uint64_t hwnd, const std::string& method, const std::string& transport) {
    // Stop any existing stream first
    cmd_capture_stream_stop();

    HWND h = (HWND)(uintptr_t)hwnd;
    if (h == nullptr) {
        // Desktop capture: use monitor-based WGC
        g_stream_handle = wgc_stream_start_monitor(
            MonitorFromWindow(nullptr, MONITOR_DEFAULTTOPRIMARY), 1280);
    } else {
        g_stream_handle = wgc_stream_start(h, 1280);
    }
    if (!g_stream_handle) {
        LOG("cmd", "stream_start: FAILED");
        return R"({"ok":false,"error":"wgc_stream_start failed"})";
    }
    g_streaming = true;

    g_stream_thread = std::thread([transport]() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        std::vector<uint8_t> buf(MAX_PX);
        while (g_streaming) {
            int w, h, ch;
            int size = wgc_stream_read(g_stream_handle, buf.data(), MAX_PX, &w, &h, &ch);
            if (size > 0 && w > 0 && h > 0 && w <= 3840 && h <= 2160) {
                // Push via SharedBuffer (zero-copy), MJPEG (fallback), TCP (agents)
                shared_buffer_push_frame(buf.data(), w, h);
                mjpeg_server_push_frame(buf.data(), w, h);
                tcp_broadcast_frame(buf.data(), w, h);
                // Store frame for stream_poll (Canvas fallback)
                std::lock_guard<std::mutex> lk(g_stream_frame_mutex);
                g_stream_frame_pixels.assign(buf.data(), buf.data() + (size_t)size);
                g_stream_frame_w = w;
                g_stream_frame_h = h;
            } else {
                // No new frame — yield CPU, avoid busy-wait
                Sleep(1);
            }
        }
        CoUninitialize();
    });

    LOG("cmd", "stream_start: hwnd=%llu method=%s transport=%s", (unsigned long long)hwnd, method.c_str(), transport.c_str());
    return R"({"ok":true})";
}

static std::string cmd_capture_stream_stop() {
    g_streaming = false;
    if (g_stream_handle) {
        wgc_stream_signal_stop(g_stream_handle);
        if (g_stream_thread.joinable()) g_stream_thread.join();
        wgc_stream_stop(g_stream_handle);
        g_stream_handle = nullptr;
    }
    LOG("cmd", "stream_stop");
    return R"({"ok":true})";
}

// ── Log commands ──────────────────────────────────────────
static std::string cmd_read_logs(int max_files) {
    // Only return file list — live content is managed by frontend LogManager.
    // (Including the ring buffer in this response causes recursive growth:
    //  each LOG() in this function expands the ring buffer, making the
    //  next response even larger until PostWebMessageAsJson drops it.)
    char* fjson = capture_log_list_files(max_files);
    std::string files = fjson ? fjson : "[]";
    capture_log_free(fjson);

    LOG("cmd", "read_logs: max_files=%d → %s", max_files, files.c_str());
    return "{\"files\":" + files + "}";
}

static std::string cmd_read_log_file(const std::string& filename) {
    // Sanity check: reject paths with separators
    if (filename.find('/') != std::string::npos ||
        filename.find('\\') != std::string::npos ||
        filename.find("..") != std::string::npos) {
        return R"({"error":"invalid filename"})";
    }
    char* content = capture_log_read_file(filename.c_str());
    std::string result = content ? content : "";
    capture_log_free(content);
    LOG("cmd", "read_log_file: %s → %zub", filename.c_str(), result.size());
    return "{\"filename\":\"" + json_escape(filename) + "\",\"content\":\"" + json_escape(result) + "\"}";
}

static std::string cmd_open_log_dir() {
    // Get absolute path to log directory
    char cwd[MAX_PATH];
    GetCurrentDirectoryA(MAX_PATH, cwd);
    std::string log_path = std::string(cwd) + "\\log";
    // Open in Explorer
    ShellExecuteA(nullptr, "open", log_path.c_str(), nullptr, nullptr, SW_SHOW);
    LOG("cmd", "open_log_dir: %s", log_path.c_str());
    return R"({"ok":true})";
}

static std::string cmd_clear_log() {
    // Stop any running stream first — prevents use-after-free in concurrent LOG() calls
    if (g_streaming) {
        g_streaming = false;
        if (g_stream_handle) wgc_stream_signal_stop(g_stream_handle);
        if (g_stream_thread.joinable()) g_stream_thread.join();
        if (g_stream_handle) { wgc_stream_stop(g_stream_handle); g_stream_handle = nullptr; }
    }
    capture_log_shutdown();
    capture_log_init("agent", "0.2.0", "log/", 5, 5000);
    LOG("cmd", "log cleared");
    return R"({"ok":true})";
}

static std::string cmd_log_ui_event(const std::string& event, const std::string& detail) {
    LOG("ui", "event=%s detail=%s", event.c_str(), detail.c_str());
    return R"({"ok":true})";
}

// ── Benchmark ─────────────────────────────────────────────
static std::string cmd_benchmark_methods(uint64_t hwnd, const std::string& method_hint) {
    const char* methods[] = {"WGC", "DesktopBlt", "GDI(GetWindowDC)", "PrintWindow", "ScreenBitBlt"};
    std::string json = R"({"results":[)";
    bool first = true;
    for (auto* m : methods) {
        if (!first) json += ","; first = false;
        auto t0 = GetTickCount64();
        auto r = call_capture(hwnd, m);
        auto ms = GetTickCount64() - t0;
        char buf[256];
        snprintf(buf, sizeof(buf), R"({"method":"%s","time_ms":%llu,"size":%zu,"ok":%s})",
                 m, (unsigned long long)ms, r.pixels.size(), r.w > 0 ? "true" : "false");
        json += buf;
    }
    json += "]}";
    return json;
}

// ── Debug dump ────────────────────────────────────────────
static bool g_dump_frames = false;
static std::string cmd_debug_dump_frames(bool enable) {
    g_dump_frames = enable;
    return R"({"ok":true})";
}

// ── highlight_window (yellow overlay) ─────────────────────
static HWND g_overlay_bars[4] = {};
static std::mutex g_overlay_mutex;
static constexpr int BORDER_W = 3;

static void destroy_overlay() {
    std::lock_guard<std::mutex> lk(g_overlay_mutex);
    for (auto& hw : g_overlay_bars) {
        if (hw) { DestroyWindow(hw); hw = nullptr; }
    }
}

static std::string cmd_highlight_window(uint64_t hwnd_u64) {
    destroy_overlay();
    std::lock_guard<std::mutex> lk(g_overlay_mutex);
    HWND target = (HWND)(uintptr_t)hwnd_u64;
    if (!target || !IsWindow(target)) return R"({"ok":false})";

    RECT r;
    if (!GetWindowRect(target, &r)) return R"({"ok":false})";
    int bw = BORDER_W;
    int iw = r.right - r.left;
    int ih = r.bottom - r.top;

    HINSTANCE hi = GetModuleHandle(nullptr);
    struct { int x, y, w, h; } pos[4] = {
        {r.left - bw, r.top - bw, iw + bw*2, bw},         // top
        {r.left - bw, r.bottom,      iw + bw*2, bw},       // bottom
        {r.left - bw, r.top,          bw,        ih},       // left
        {r.right,     r.top,          bw,        ih}        // right
    };

    for (int i = 0; i < 4; i++) {
        g_overlay_bars[i] = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW,
            L"STATIC", nullptr,
            WS_POPUP | WS_VISIBLE,
            pos[i].x, pos[i].y, pos[i].w, pos[i].h,
            nullptr, nullptr, hi, nullptr);
        if (g_overlay_bars[i]) {
            SetLayeredWindowAttributes(g_overlay_bars[i], RGB(255,255,0), 180, LWA_ALPHA);
            HDC dc = GetDC(g_overlay_bars[i]);
            if (dc) {
                HBRUSH br = CreateSolidBrush(RGB(255,255,0));
                RECT fr = {0, 0, pos[i].w, pos[i].h};
                FillRect(dc, &fr, br);
                DeleteObject(br);
                ReleaseDC(g_overlay_bars[i], dc);
            }
            ShowWindow(g_overlay_bars[i], SW_SHOWNOACTIVATE);
        }
    }
    return R"({"ok":true})";
}

// ── Main dispatch ─────────────────────────────────────────
std::string dispatch_command(const std::string& json) {
    std::string cmd = json_get_str(json, "cmd");
    int id = json_get_int(json, "id");
    std::string args = json_get_obj(json, "args");

    std::string result;
    if (cmd == "list_windows") result = cmd_list_windows();
    else if (cmd == "list_processes") result = cmd_list_processes();
    else if (cmd == "capture_window") {
        result = cmd_capture_window(json_get_uint64(args, "hwnd"), json_get_str(args, "method"));
    }
    else if (cmd == "capture_stream_start") {
        result = cmd_capture_stream_start(json_get_uint64(args, "hwnd"),
            json_get_str(args, "method"), json_get_str(args, "transport"));
    }
    else if (cmd == "capture_stream_stop") result = cmd_capture_stream_stop();
    else if (cmd == "read_logs") result = cmd_read_logs(json_get_int(args, "max_files"));
    else if (cmd == "read_log_file") result = cmd_read_log_file(json_get_str(args, "filename"));
    else if (cmd == "open_log_dir") result = cmd_open_log_dir();
    else if (cmd == "clear_log") result = cmd_clear_log();
    else if (cmd == "log_ui_event") {
        result = cmd_log_ui_event(json_get_str(args, "event"), json_get_str(args, "detail"));
    }
    else if (cmd == "benchmark_methods") {
        result = cmd_benchmark_methods(json_get_uint64(args, "hwnd"), json_get_str(args, "method"));
    }
    else if (cmd == "debug_dump_frames") {
        result = cmd_debug_dump_frames(json_get_int(args, "enable") != 0);
    }
    else if (cmd == "highlight_window") {
        result = cmd_highlight_window(json_get_uint64(args, "hwnd"));
    }
    else if (cmd == "screen_info") {
        int sw = GetSystemMetrics(SM_CXSCREEN);
        int sh = GetSystemMetrics(SM_CYSCREEN);
        result = "{\"w\":" + std::to_string(sw) + ",\"h\":" + std::to_string(sh) + "}";
    }
    else if (cmd == "window_state") {
        auto* hw = (HWND)(uintptr_t)json_get_uint64(args, "hwnd");
        const char* state = capture_query_window_state(hw);
        result = "\"" + std::string(state ? state : "unknown") + "\"";
        if (state) capture_free_string(state);
    }
    else if (cmd == "stream_poll") {
        // Return latest RGBA frame as base64 for Canvas fallback
        std::lock_guard<std::mutex> lk(g_stream_frame_mutex);
        if (g_stream_frame_pixels.empty()) { result = "{}"; }
        else {
            std::string b64 = base64_encode(g_stream_frame_pixels.data(), g_stream_frame_pixels.size());
            result = "{\"p\":\"" + b64 + "\",\"w\":" + std::to_string(g_stream_frame_w) +
                     ",\"h\":" + std::to_string(g_stream_frame_h) + ",\"m\":\"WGC\"}";
        }
    }

    if (id <= 0) return result; // fire-and-forget (no id field)
    if (result.empty()) return "{\"id\":" + std::to_string(id) + ",\"error\":\"unknown command\"}";
    return "{\"id\":" + std::to_string(id) + ",\"result\":" + result + "}";
}

// ── Init / Shutdown ───────────────────────────────────────
// ── MTA daemon thread (WGC needs MTA, main thread is STA for WebView2/WIC) ──
static std::thread g_mta_thread;
static std::atomic<bool> g_mta_running{false};
static std::mutex g_mta_init_mtx;
static std::condition_variable g_mta_init_cv;
static bool g_mta_init_done = false;

static void mta_daemon() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    wgc_init_apartment();
    {
        std::lock_guard<std::mutex> lk(g_mta_init_mtx);
        g_mta_init_done = true;
    }
    g_mta_init_cv.notify_one();
    LOG("cmd", "MTA daemon running");
    while (g_mta_running) Sleep(500);
    wgc_deinit_apartment();
    CoUninitialize();
    LOG("cmd", "MTA daemon stopped");
}

void backend_init() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); // STA for WebView2/WIC
    capture_log_init("agent", "0.2.0", "log/", 5, 5000);
    init_wic();
    tcp_server_start();

    // MTA daemon for WGC (separate thread avoids STA vs MTA conflict)
    g_mta_running = true;
    g_mta_thread = std::thread(mta_daemon);
    // Wait for MTA init with proper sync (not Sleep race)
    {
        std::unique_lock<std::mutex> lk(g_mta_init_mtx);
        if (!g_mta_init_cv.wait_for(lk, std::chrono::seconds(10), [] { return g_mta_init_done; })) {
            LOG("cmd", "WARNING: MTA daemon init timeout after 10s");
        }
    }

    LOG("cmd", "backend init OK");
}

void backend_shutdown() {
    if (g_streaming) {
        g_streaming = false;
        if (g_stream_handle) wgc_stream_signal_stop(g_stream_handle);
        if (g_stream_thread.joinable()) g_stream_thread.join();
        if (g_stream_handle) { wgc_stream_stop(g_stream_handle); g_stream_handle = nullptr; }
    }
    LOG("cmd", "backend shutdown");
    g_mta_running = false;
    if (g_mta_thread.joinable()) g_mta_thread.join();
    tcp_server_stop();
    destroy_overlay();
    capture_log_flush();
    capture_log_shutdown();
    g_wic = nullptr;
}
