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
#include "paths.h"
#include "version.h"
#include "sha256_util.h"
#include "../../logger/logger.h"
#include "../../capture/include/capture_methods.h"
#include "../../capture/include/capture_wgc_ffi.h"
#include "../dep/WebView2.h"  // IID_PPV_ARGS for ICoreWebView2Environment12/17
#include <shobjidl.h>  // IVirtualDesktopManager
#include <shlobj.h>    // SHGetFolderPathW, CSIDL_LOCAL_APPDATA
#include "virtual_desktop.h"  // vd_list_desktops, vd_switch_desktop
#include <shellapi.h>  // ShellExecuteA
#include <windows.h>
#include <tlhelp32.h>
#include <dwmapi.h>
#include <wincodec.h>
#include <winhttp.h>
#include <wrl/client.h>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <functional>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>

#pragma comment(lib, "winhttp.lib")

using Microsoft::WRL::ComPtr;

// Shared by main.cpp — pushed from stream thread
extern void PostJsonToWebView(const std::string& json);
// Shared by main.cpp — reveal the (initially hidden) main window on frontend ready
extern void app_post_show_window();

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
    if (!init_wic()) { LOG("cmd", "bgra_to_png: init_wic FAILED"); return false; }

    ComPtr<IWICBitmap> bitmap;
    HRESULT hr = g_wic->CreateBitmapFromMemory((UINT)w, (UINT)h,
        GUID_WICPixelFormat32bppBGRA, (UINT)(w * 4), (UINT)(w * h * 4),
        (BYTE*)bgra, &bitmap);
    if (FAILED(hr)) { LOG("cmd", "bgra_to_png: CreateBitmapFromMemory FAILED hr=0x%x", (unsigned)hr); return false; }

    ComPtr<IStream> stream;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream))) { LOG("cmd", "bgra_to_png: CreateStreamOnHGlobal FAILED"); return false; }

    ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(g_wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder))) { LOG("cmd", "bgra_to_png: CreateEncoder FAILED"); return false; }
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

// ── Logger → TS push callback ──────────────────────────────
// Logger builds JSON internally (owns wire format); we just post it.
static void on_log_notify(const char* json) {
    PostJsonToWebView(json);
}

// ── list_windows ──────────────────────────────────────────
struct WindowInfo { std::string title, category; uint64_t hwnd; int desktop; };
static std::vector<WindowInfo> g_winlist;
static std::mutex g_winlist_mutex;

struct EnumContext {
    std::vector<WindowInfo>* list;
    IVirtualDesktopManager* vdm;
    std::vector<GUID>* absolute_order;  // registry Task View order (D1=leftmost, D2=second...)
    std::vector<GUID>* seen_guids;      // accumulate all seen desktop GUIDs
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

    // Absolute desktop numbering from registry Task View order (D1=leftmost)
    // Fall back to relative order if registry unavailable
    int desktop_num = 0;
    if (ctx->absolute_order && !IsEqualGUID(desktop_id, GUID_NULL)) {
        // Look up GUID in absolute order list (registry Task View order)
        int found_at = -1;
        for (size_t i = 0; i < ctx->absolute_order->size(); i++) {
            if (IsEqualGUID((*ctx->absolute_order)[i], desktop_id)) {
                found_at = (int)i; break;
            }
        }
        if (found_at < 0) {
            // GUID not in registry list yet — track in seen_guids for "Entire Desktop" count
            ctx->absolute_order->push_back(desktop_id);
            found_at = (int)ctx->absolute_order->size() - 1;
        }
        desktop_num = found_at + 1; // D1, D2, D3...
    } else if (ctx->seen_guids) {
        // Fallback: relative numbering if registry unavailable
        int found_at = -1;
        for (size_t i = 0; i < ctx->seen_guids->size(); i++) {
            if (IsEqualGUID((*ctx->seen_guids)[i], desktop_id)) {
                found_at = (int)i; break;
            }
        }
        if (found_at < 0) {
            ctx->seen_guids->push_back(desktop_id);
            found_at = (int)ctx->seen_guids->size() - 1;
        }
        desktop_num = found_at + 1;
    }

    list->push_back({title, "window", (uint64_t)(uintptr_t)hwnd, desktop_num});
    return TRUE;
}

static std::string cmd_list_windows() {
    std::vector<WindowInfo> list;

    // Create VirtualDesktopManager for cross-desktop window enumeration
    IVirtualDesktopManager* vdm = nullptr;
    CoCreateInstance(CLSID_VirtualDesktopManager, nullptr, CLSCTX_INPROC_SERVER,
                     IID_PPV_ARGS(&vdm));

    // Read absolute desktop order from registry (Task View left-to-right = D1, D2, D3...)
    std::vector<GUID> absolute_order = vd_get_registry_desktop_order();
    std::vector<GUID> seen_guids; // fallback if registry empty

    EnumContext ctx = {&list, vdm,
        absolute_order.empty() ? nullptr : &absolute_order,
        absolute_order.empty() ? &seen_guids : nullptr};
    EnumWindows(enum_callback, (LPARAM)&ctx);

    if (vdm) vdm->Release();

    // Determine total desktop count and numbering
    int total_desktops = 0;
    if (!absolute_order.empty()) {
        total_desktops = (int)absolute_order.size();
    } else {
        total_desktops = (int)seen_guids.size();
        if (total_desktops == 0) total_desktops = 1;
        // Copy seen_guids to absolute_order for consistent "Entire Desktop" numbering
        absolute_order = seen_guids;
    }

    // Per-desktop "Entire Desktop" entries (D1, D2, D3... = Task View order)
    for (int d = total_desktops; d >= 1; d--) {
        std::string title = " Entire Desktop";
        if (total_desktops > 1) {
            title += " (D" + std::to_string(d) + ")";
        }
        list.insert(list.begin(), {title, "desktop", 0, d});
    }

    LOG("cmd", "list_windows: %zu entries, %d desktops (abs=%d)",
        list.size(), total_desktops, (int)!absolute_order.empty());

    std::string json = "[";
    for (size_t i = 0; i < list.size(); i++) {
        if (i > 0) json += ",";
        char buf[512];
        snprintf(buf, sizeof(buf), R"({"title":"%s","category":"%s","hwnd":%llu,"desktop":%d})",
                 json_escape(list[i].title).c_str(), list[i].category.c_str(),
                 (unsigned long long)list[i].hwnd, list[i].desktop);
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
void dump_frame_if_enabled(const uint8_t* bgra, int w, int h, bool is_stream);

static CaptureResult call_capture(uint64_t hwnd, const std::string& method) {
    std::vector<uint8_t> buf(MAX_PX);
    int w = 0, h = 0, size = 0;
    HWND hw = (HWND)(uintptr_t)hwnd;
    std::string used = method;

    LOG("cmd", "call_capture: hwnd=%llu method=%s", (unsigned long long)hwnd, method.c_str());

    if (method == "WGC" || method == "wgc") {
        // WGC window capture — needs valid HWND. Desktop (hwnd=0) rejected.
        // Frontend should use 'desktopblt' or 'wgc-monitor' for desktop.
        if (!hw) {
            LOG("cmd", "call_capture: wgc requires valid hwnd, got 0 — use 'desktopblt' or 'wgc-monitor'");
            used = "wgc(bad_hwnd)";
            return {{}, 0, 0, used};
        }
        LOG("cmd", "call_capture: spawning MTA thread for WGC single-frame hwnd=%llu", (unsigned long long)hwnd);
        std::atomic<bool> done{false};
        std::thread t([&]() {
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            size = wgc_capture_single(hw, buf.data(), MAX_PX, &w, &h, nullptr);
            LOG("cmd", "call_capture: wgc_capture_single returned size=%d w=%d h=%d", size, w, h);
            CoUninitialize();
            done = true;
        });
        t.join();
    } else if (method == "wgc-monitor") {
        // WGC monitor capture — frontend explicitly asked for monitor-based capture
        HMONITOR hmon = MonitorFromWindow(nullptr, MONITOR_DEFAULTTOPRIMARY);
        LOG("cmd", "call_capture: wgc-monitor hmon=%p", (void*)hmon);
        std::atomic<bool> done{false};
        std::thread t([&]() {
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            size = wgc_capture_single_monitor(hmon, buf.data(), MAX_PX, &w, &h, nullptr);
            LOG("cmd", "call_capture: wgc_capture_single_monitor returned size=%d w=%d h=%d", size, w, h);
            CoUninitialize();
            done = true;
        });
        t.join();
    } else if (method == "dxgi" || method == "desktopblt") {
        // Desktop BitBlt (GDI) — fast, reliable desktop single-frame.
        // Named 'dxgi' for backward compat; 'desktopblt' is the canonical name.
        size = capture_desktop_bitblt(buf.data(), MAX_PX, &w, &h);
        used = "DesktopBlt";
    } else if (method == "GDI(GetWindowDC)") {
        size = capture_gdi_getwindowdc(hw, buf.data(), MAX_PX, &w, &h);
    } else if (method == "PrintWindow") {
        size = capture_printwindow(hw, buf.data(), MAX_PX, &w, &h);
    } else if (method == "ScreenBitBlt") {
        size = capture_screen_bitblt(hw, buf.data(), MAX_PX, &w, &h);
    } else if (method == "DesktopBlt") {
        size = capture_desktop_bitblt(buf.data(), MAX_PX, &w, &h);
    } else {
        // Unknown method — fail, don't guess
        LOG("cmd", "call_capture: unknown method '%s'", method.c_str());
        used = "unknown_method";
        return {{}, 0, 0, used};
    }

    if (size > 0 && w > 0 && h > 0) {
        buf.resize((size_t)size);
        return {buf, w, h, used};
    }
    return {{}, 0, 0, "ALL_FAILED"};
}

// ── Frame dump (developer mode) ─────────────────────────
// Globals: defined here so all functions can reference them
static bool g_dump_capture_frames = false;
static bool g_dump_stream_frames = false;
static std::string g_dump_dir;

void dump_frame_if_enabled(const uint8_t* bgra, int w, int h, bool is_stream);

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
    LOG("cmd", "cmd_capture_window: hwnd=%llu method=%s", (unsigned long long)hwnd, method.c_str());
    auto r = call_capture(hwnd, method.empty() ? "auto" : method);
    if (r.w <= 0 || r.h <= 0) {
        LOG("cmd", "capture_window: FAILED hwnd=%llu", (unsigned long long)hwnd);
        return "{}";
    }

    // Push frame via SharedBuffer (zero-copy) — no more base64 PNG.
    // Frontend receives the frame via 'sharedbufferreceived' event.
    LOG("cmd", "capture_window: pushing %dx%d (%zu bytes) via SharedBuffer",
        r.w, r.h, r.pixels.size());
    shared_buffer_push_frame(r.pixels.data(), r.w, r.h);

    // Developer mode: dump frame to disk
    LOG("cmd", "capture_window: calling dump_frame_if_enabled (snapshot)");
    dump_frame_if_enabled(r.pixels.data(), r.w, r.h, false);

    std::string json = "{\"ok\":true,\"w\":" + std::to_string(r.w) +
        ",\"h\":" + std::to_string(r.h) +
        ",\"method\":\"" + r.method + "\"}";
    LOG("cmd", "capture_window: %dx%d method=%s via SharedBuffer", r.w, r.h, r.method.c_str());
    return json;
}

// ── Stream management ─────────────────────────────────────
static std::atomic<bool> g_streaming{false};
static std::thread g_stream_thread;
static WgcStreamHandle* g_stream_handle = nullptr;
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

// ── Self-test client (connects to test_target 127.0.0.1:9998, JSON-lines) ──
// Reads reports from test_target and forwards each to the frontend tagged
// type:"selftest". Each line is already a JSON object → nested directly.
static SOCKET            g_st_sock = INVALID_SOCKET;
static std::thread       g_st_thread;
static std::atomic<bool> g_st_running{false};

static void st_forward(const std::string& jsonObj) {
    PostJsonToWebView("{\"type\":\"selftest\",\"data\":" + jsonObj + "}");
}

static void st_cleanup() {
    g_st_running = false;
    if (g_st_sock != INVALID_SOCKET) { closesocket(g_st_sock); g_st_sock = INVALID_SOCKET; }
    if (g_st_thread.joinable()) g_st_thread.join();
}

static void st_reader_loop() {
    std::string buf;
    char tmp[1024];
    while (g_st_running) {
        int n = recv(g_st_sock, tmp, sizeof(tmp), 0);
        if (n <= 0) break;
        buf.append(tmp, n);
        size_t nl;
        while ((nl = buf.find('\n')) != std::string::npos) {
            std::string line = buf.substr(0, nl);
            buf.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) st_forward(line);
        }
    }
    g_st_running = false;
    st_forward(R"({"type":"disconnected"})");   // notify frontend link dropped
    LOG("cmd", "selftest reader exited");
}

static std::string cmd_selftest_connect(int port) {
    st_cleanup();                       // idempotent — drop any stale connection
    if (port <= 0) port = 9998;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return R"({"ok":false,"error":"socket failed"})";
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons((u_short)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, (sockaddr*)&a, sizeof(a)) != 0) {
        int err = WSAGetLastError();
        closesocket(s);
        char e[96];
        snprintf(e, sizeof(e), "{\"ok\":false,\"error\":\"connect failed (%d)\"}", err);
        return e;
    }
    g_st_sock = s;
    g_st_running = true;
    g_st_thread = std::thread(st_reader_loop);
    LOG("cmd", "selftest connected to 127.0.0.1:%d", port);
    return R"({"ok":true})";
}

static std::string cmd_selftest_disconnect() {
    st_cleanup();
    LOG("cmd", "selftest disconnected");
    return R"({"ok":true})";
}

static std::string cmd_capture_stream_start(uint64_t hwnd, const std::string& method, const std::string& transport) {
    // NOTE: TS now handles conflict resolution (auto-stop before start).
    // This auto-stop is commented out to ensure TS-side bugs are surfaced, not silently fixed.
    // cmd_capture_stream_stop();

    HWND h = (HWND)(uintptr_t)hwnd;

    // Frontend decides method; C++ only executes.
    if (method == "wgc" || method == "WGC") {
        if (h == nullptr) {
            g_stream_handle = wgc_stream_start_monitor(
                MonitorFromWindow(nullptr, MONITOR_DEFAULTTOPRIMARY), 1280);
        } else {
            g_stream_handle = wgc_stream_start(h, 1280);
        }
    } else if (method == "dxgi" || method == "DXGI") {
        // DXGI Desktop Duplication stream not yet implemented.
        // Frontend should use 'wgc' for streaming until DXGI stream is ready.
        LOG("cmd", "stream_start: DXGI stream not implemented, use 'wgc'");
        return R"({"ok":false,"error":"DXGI stream not implemented; use 'wgc' for streaming"})";
    } else {
        LOG("cmd", "stream_start: unknown method '%s'", method.c_str());
        return R"({"ok":false,"error":"unknown stream method"})";
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
                // SharedBuffer via bridge: stream thread (MTA) → PostMessage → main STA thread
                stream_bridge_push_frame(buf.data(), w, h);
                tcp_broadcast_frame(buf.data(), w, h);
                // Developer mode: dump stream frame to disk
                dump_frame_if_enabled(buf.data(), w, h, true);
            } else {
                Sleep(1);
            }
        }
        CoUninitialize();
    });

    LOG("cmd", "stream_start: hwnd=%llu method=%s transport=%s dump_cap=%d dump_str=%d dump_dir='%s'",
        (unsigned long long)hwnd, method.c_str(), transport.c_str(),
        (int)g_dump_capture_frames, (int)g_dump_stream_frames, g_dump_dir.c_str());
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

    LOG("cmd", "read_logs: max_files=%d -> %s", max_files, files.c_str());
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
    LOG("cmd", "read_log_file: %s -> %zub", filename.c_str(), result.size());
    return "{\"filename\":\"" + json_escape(filename) + "\",\"content\":\"" + json_escape(result) + "\"}";
}

static std::string cmd_open_log_dir() {
    const char* log_path = capture_log_get_dir();
    if (log_path && log_path[0]) {
        ShellExecuteA(nullptr, "open", log_path, nullptr, nullptr, SW_SHOW);
        LOG("cmd", "open_log_dir: %s", log_path);
    }
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
    capture_log_init("agent", APP_VERSION, capture_log_get_dir(), 5, 5000);
    LOG("cmd", "log cleared -- previous session archived, new session started");
    return R"({"ok":true})";
}

static std::string cmd_log_ui_event(const std::string& event, const std::string& detail) {
    if (detail.empty()) {
        capture_log_write_ui(event.c_str());
    } else {
        std::string combined = event + " | " + detail;
        capture_log_write_ui(combined.c_str());
    }
    return R"({"ok":true})";
}

static std::string cmd_read_live_log() {
    char* mem = capture_log_read_memory();
    std::string content = mem ? mem : "";
    capture_log_free(mem);
    return "{\"lines\":\"" + json_escape(content) + "\"}";
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

// ── Frame dump commands ─────────────────────────────────
static std::string cmd_set_frame_dump(bool capture, bool stream, const std::string& dir) {
    g_dump_capture_frames = capture;
    g_dump_stream_frames = stream;
    if (!dir.empty()) g_dump_dir = dir;
    LOG("cmd", "set_frame_dump: capture=%d stream=%d dir=%s",
        (int)capture, (int)stream, g_dump_dir.c_str());
    return R"({"ok":true})";
}

static std::string cmd_pick_dir() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool needs_uninit = SUCCEEDED(hr);
    std::string result = "{}";
    IFileDialog* dlg = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&dlg))) && dlg) {
        dlg->SetOptions(FOS_PICKFOLDERS | FOS_PATHMUSTEXIST);
        if (SUCCEEDED(dlg->Show(nullptr))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dlg->GetResult(&item)) && item) {
                wchar_t* path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                    int len = WideCharToMultiByte(CP_UTF8, 0, path, -1, nullptr, 0, nullptr, nullptr);
                    std::string dir(len - 1, '\0');
                    WideCharToMultiByte(CP_UTF8, 0, path, -1, &dir[0], len, nullptr, nullptr);
                    result = "{\"dir\":\"" + json_escape(dir) + "\"}";
                    CoTaskMemFree(path);
                }
                item->Release();
            }
        }
        dlg->Release();
    }
    if (needs_uninit) CoUninitialize();
    return result;
}

static std::string cmd_open_dir(const std::string& dir) {
    if (!dir.empty()) {
        ShellExecuteA(nullptr, "open", dir.c_str(), nullptr, nullptr, SW_SHOW);
    }
    return R"({"ok":true})";
}

// Save a single BGRA frame as PNG to dump dir
static void dump_frame_to_disk(const uint8_t* bgra, int w, int h, const char* prefix) {
    if (g_dump_dir.empty()) {
        LOG("cmd", "dump_frame_to_disk: SKIP — g_dump_dir is empty");
        return;
    }
    // Generate filename: prefix_YYYYMMDD_HHMMSS_ms.png
    SYSTEMTIME st;
    GetLocalTime(&st);
    char fname[256];
    snprintf(fname, sizeof(fname), "%s_%04d%02d%02d_%02d%02d%02d_%03d.png",
             prefix, st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    std::string full = g_dump_dir + "\\" + fname;
    LOG("cmd", "dump_frame_to_disk: target=%s %dx%d", full.c_str(), w, h);

    // Convert BGRA → RGBA
    std::vector<uint8_t> rgba(w * h * 4);
    for (int i = 0; i < w * h; i++) {
        rgba[i * 4 + 0] = bgra[i * 4 + 2];
        rgba[i * 4 + 1] = bgra[i * 4 + 1];
        rgba[i * 4 + 2] = bgra[i * 4 + 0];
        rgba[i * 4 + 3] = 255;
    }

    std::vector<uint8_t> png;
    if (bgra_to_png(rgba.data(), w, h, png)) {
        FILE* f = fopen(full.c_str(), "wb");
        if (f) {
            size_t written = fwrite(png.data(), 1, png.size(), f);
            fclose(f);
            LOG("cmd", "frame dump OK: %s (%dx%d png=%zu written=%zu)", fname, w, h, png.size(), written);
        } else {
            LOG("cmd", "frame dump FAIL: fopen '%s' err=%d", full.c_str(), errno);
        }
    } else {
        LOG("cmd", "frame dump FAIL: bgra_to_png returned false");
    }
}

void dump_frame_if_enabled(const uint8_t* bgra, int w, int h, bool is_stream) {
    bool enabled = is_stream ? g_dump_stream_frames : g_dump_capture_frames;
    if (!enabled) return;  // skip LOG when disabled — avoids per-frame spam that breaks collapse
    LOG("cmd", "dump_frame_if_enabled: is_stream=%d dir='%s'",
        (int)is_stream, g_dump_dir.c_str());
    dump_frame_to_disk(bgra, w, h, is_stream ? "stream" : "snap");
}


// ── Cursor overlay: transparent circular window shown on real screen ──
// Pre-rendered 32x32 BGRA bitmap with per-pixel alpha → UpdateLayeredWindow.
static HWND g_overlay_hwnd = nullptr;
static HBITMAP g_overlay_bmp = nullptr;
static int g_overlay_size = 32;
static int g_overlay_half = 16;

static void cursor_overlay_init() {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = L"GAM_CursorOverlay";
    wc.lpfnWndProc = DefWindowProcW;
    RegisterClassExW(&wc);

    int SZ = g_overlay_size, H = g_overlay_half;

    g_overlay_hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED,
        L"GAM_CursorOverlay", L"",
        WS_POPUP,
        0, 0, SZ, SZ,
        nullptr, nullptr, GetModuleHandle(nullptr), nullptr);

    // Pre-render 32×32 BGRA bitmap with anti-aliased circle
    // Channels: B G R A (little-endian BGRA as DWORD)
    BITMAPV5HEADER bi = {};
    bi.bV5Size = sizeof(BITMAPV5HEADER);
    bi.bV5Width = SZ;
    bi.bV5Height = -SZ; // top-down
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_RGB;

    DWORD* pixels = nullptr;
    HDC hdcScreen = GetDC(nullptr);
    g_overlay_bmp = CreateDIBSection(hdcScreen, (BITMAPINFO*)&bi, DIB_RGB_COLORS,
                                      (void**)&pixels, nullptr, 0);
    ReleaseDC(nullptr, hdcScreen);

    if (!pixels || !g_overlay_bmp) return;

    float cx = (float)H, cy = (float)H;
    float outerR = (float)H - 2.0f;   // outer ring radius
    float innerR = (float)(H - 4);     // inner hole radius (ring thickness)
    float dotR = (float)H * 0.38f;     // center dot radius

    for (int y = 0; y < SZ; y++) {
        for (int x = 0; x < SZ; x++) {
            float dx = (float)x - cx + 0.5f;
            float dy = (float)y - cy + 0.5f;
            float dist = sqrtf(dx * dx + dy * dy);

            // Anti-aliased ring
            float outerAlpha = fmaxf(0.0f, fminf(1.0f, outerR - dist + 0.5f));
            float innerAlpha = fmaxf(0.0f, fminf(1.0f, dist - innerR + 0.5f));
            float ringAlpha = outerAlpha * innerAlpha;

            // Anti-aliased center dot
            float dotAlpha = fmaxf(0.0f, fminf(1.0f, dotR - dist + 0.5f));

            float alpha = fmaxf(ringAlpha, dotAlpha);
            BYTE a = (BYTE)(alpha * 220.0f);  // overall opacity

            // Accent blue #3B82F6 → RGB(59, 130, 246)
            BYTE b = 59, g = 130, r = 246;

            // Premultiplied alpha for UpdateLayeredWindow
            pixels[y * SZ + x] = ((DWORD)a << 24) | ((DWORD)(r * a / 255) << 16) |
                                 ((DWORD)(g * a / 255) << 8) | (DWORD)(b * a / 255);
        }
    }

    ShowWindow(g_overlay_hwnd, SW_HIDE);
}

static void cursor_overlay_show(int screenX, int screenY) {
    if (!g_overlay_hwnd) cursor_overlay_init();
    if (!g_overlay_hwnd || !g_overlay_bmp) return;

    HDC hdcScreen = GetDC(nullptr);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    HBITMAP oldBmp = (HBITMAP)SelectObject(hdcMem, g_overlay_bmp);

    POINT ptDst = { screenX - g_overlay_half, screenY - g_overlay_half };
    POINT ptSrc = { 0, 0 };
    SIZE sz = { g_overlay_size, g_overlay_size };
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };

    UpdateLayeredWindow(g_overlay_hwnd, hdcScreen, &ptDst, &sz, hdcMem, &ptSrc, 0, &bf, ULW_ALPHA);

    SelectObject(hdcMem, oldBmp);
    DeleteDC(hdcMem);
    ReleaseDC(nullptr, hdcScreen);

    SetWindowPos(g_overlay_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
}

static void cursor_overlay_hide() {
    if (g_overlay_hwnd) ShowWindow(g_overlay_hwnd, SW_HIDE);
}

// ── Input forwarding (delegated to per-method libs) ──────────
#include "../../input/include/input_methods.h"

static std::string cmd_send_input(const std::string& args) {
    InputArgs a = parse_input_args(args);

    // Desktop (hwnd=0): only sendinput works (uses virtual screen coords).
    // winapi/postmessage need a real window handle for thread attach / message routing.
    if (a.hwnd == 0 && a.method != "sendinput")
        return "{\"ok\":false,\"error\":\"desktop input only supports sendinput method\"}";
    HWND hWnd = (HWND)(uintptr_t)a.hwnd;
    if (hWnd != nullptr && !IsWindow(hWnd))
        return "{\"ok\":false,\"error\":\"invalid window handle\"}";

    if (a.method == "sendinput")    return input_sendinput(hWnd, a);
    if (a.method == "winapi")       return input_winapi(hWnd, a);
    if (a.method == "postmessage")  return input_postmessage(hWnd, a);
    if (a.method == "driver")       return input_driver(hWnd, a);

    return "{\"ok\":false,\"error\":\"unknown input method: " + a.method + "\"}";
}

// ── Auto-update (WinHTTP) ──────────────────────────────────

// Minimal JSON value extractor — handles nested keys by searching
// from a given offset. Returns empty if not found.
static std::string json_val(const std::string& json, const std::string& key, size_t from = 0) {
    std::string s = "\"" + key + "\"";
    size_t p = json.find(s, from);
    if (p == std::string::npos) return "";
    p += s.length();
    while (p < json.size() && (json[p] == ':' || json[p] == ' ')) p++;
    if (p >= json.size()) return "";
    if (json[p] == '"') {
        p++;
        size_t e = p;
        while (e < json.size() && !(json[e] == '"' && json[e-1] != '\\')) e++;
        return json.substr(p, e - p);
    }
    // Number/bool/null — read until , } ]
    size_t e = p;
    while (e < json.size() && json[e] != ',' && json[e] != '}' && json[e] != ']') e++;
    return json.substr(p, e - p);
}

// Progress callback for downloads: (bytesDownloaded, totalBytes | 0 if unknown).
using ProgressCb = std::function<void(unsigned long long, unsigned long long)>;

// WinHTTP GET — returns response body (empty string on failure).
// Logs tag for diagnostics. on_progress (optional) fires after each read chunk.
static std::string winhttp_get(const wchar_t* url, const char* tag, ProgressCb on_progress = nullptr) {
    // Crack URL into components
    URL_COMPONENTS uc = {};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {}, path[1024] = {};
    uc.lpszHostName = host;  uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;   uc.dwUrlPathLength = 1024;
    if (!WinHttpCrackUrl(url, 0, 0, &uc)) {
        LOG(tag, "WinHttpCrackUrl failed url=%S", url);
        return "";
    }
    bool https = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    WORD port = uc.nPort ? uc.nPort : (https ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT);

    HINTERNET hSess = WinHttpOpen(L"GAM/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSess) { LOG(tag, "WinHttpOpen failed"); return ""; }

    HINTERNET hConn = WinHttpConnect(hSess, host, port, 0);
    if (!hConn) { WinHttpCloseHandle(hSess); LOG(tag, "WinHttpConnect failed"); return ""; }

    HINTERNET hReq = WinHttpOpenRequest(hConn, L"GET", path, nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, https ? WINHTTP_FLAG_SECURE : 0);
    if (!hReq) {
        WinHttpCloseHandle(hConn); WinHttpCloseHandle(hSess);
        LOG(tag, "WinHttpOpenRequest failed");
        return "";
    }

    // Follow HTTP redirects (Gitee raw URLs 302 -> raw.giteeusercontent.com)
    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(hReq, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy));

    BOOL ok = WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (!ok) {
        WinHttpCloseHandle(hReq); WinHttpCloseHandle(hConn); WinHttpCloseHandle(hSess);
        LOG(tag, "WinHttpSendRequest failed");
        return "";
    }

    ok = WinHttpReceiveResponse(hReq, nullptr);
    if (!ok) {
        WinHttpCloseHandle(hReq); WinHttpCloseHandle(hConn); WinHttpCloseHandle(hSess);
        LOG(tag, "WinHttpReceiveResponse failed");
        return "";
    }

    // Check HTTP status. Non-2xx is a HARD failure: return "" instead of the
    // body. A 404/403/5xx (or a WAF/redirect stub) response body is NOT data —
    // handing it back would silently poison manifest parsing and masquerade as
    // "no update" (see cmd_check_update). 铁律 5: fail loud, never fake success.
    DWORD status = 0, statusSize = sizeof(status);
    WinHttpQueryHeaders(hReq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX);
    LOG(tag, "HTTP %lu", (unsigned long)status);
    if (status < 200 || status >= 300) {
        LOG_WARN(tag, "non-2xx status %lu for %S — returning empty (fetch failed)",
            (unsigned long)status, url);
        WinHttpCloseHandle(hReq); WinHttpCloseHandle(hConn); WinHttpCloseHandle(hSess);
        return "";
    }

    // Total size for progress (Content-Length header; 0 if absent/chunked).
    unsigned long long total = 0;
    {
        wchar_t clbuf[32] = {}; DWORD clsize = sizeof(clbuf);
        if (WinHttpQueryHeaders(hReq, WINHTTP_QUERY_CONTENT_LENGTH,
                WINHTTP_HEADER_NAME_BY_INDEX, clbuf, &clsize, WINHTTP_NO_HEADER_INDEX))
            total = _wcstoui64(clbuf, nullptr, 10);
    }

    std::string body;
    DWORD bytesRead = 0;
    char buf[4096];
    while (WinHttpReadData(hReq, buf, sizeof(buf), &bytesRead) && bytesRead > 0) {
        body.append(buf, bytesRead);
        if (on_progress) on_progress((unsigned long long)body.size(), total);
    }

    WinHttpCloseHandle(hReq);
    WinHttpCloseHandle(hConn);
    WinHttpCloseHandle(hSess);
    return body;
}

// Also support std::string URL wrapper
static std::string winhttp_get_str(const std::string& urlStr, const char* tag, ProgressCb on_progress = nullptr) {
    int len = MultiByteToWideChar(CP_UTF8, 0, urlStr.c_str(), (int)urlStr.size(), nullptr, 0);
    std::wstring wurl(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, urlStr.c_str(), (int)urlStr.size(), &wurl[0], len);
    return winhttp_get(wurl.c_str(), tag, on_progress);
}

// Forward declarations
static std::string read_file(const char* path);

// ── Auto-update: shared progress state (download thread → WndProc → JS) ──
static UpdateProgress g_up;
static std::mutex     g_up_mtx;

UpdateProgress update_get_progress() {
    std::lock_guard<std::mutex> lk(g_up_mtx);
    return g_up;
}

// Launch updater.exe for the finished (succeeded) download. Main thread (WndProc).
// Guard against duplicate WM_UPDATE_PROGRESS — the download thread posts once per
// chunk AND a terminal "done" post, which can both land in the queue before the
// first is processed, causing two ShellExecuteEx runas calls (two UAC prompts).
bool update_launch_updater() {
    static bool s_launched = false;  // one-shot
    if (s_launched) return false;
    std::string stagingDir;
    {
        std::lock_guard<std::mutex> lk(g_up_mtx);
        if (!g_up.succeeded || g_up.staging_dir.empty()) return false;
        stagingDir = g_up.staging_dir;
    }
    s_launched = true;
    std::string installDir  = paths_get_install_dir();
    std::string updaterPath = installDir + "\\bin\\updater.exe";
    DWORD pid = GetCurrentProcessId();
    // updater.exe is requireAdministrator (it overwrites Program Files). Launching
    // it from this non-elevated process via CreateProcess fails with
    // ERROR_ELEVATION_REQUIRED (740). ShellExecuteEx + "runas" raises the UAC
    // prompt so the updater runs elevated.
    // 铁律 9a: params do NOT quote the staging path (the updater strips quotes defensively).
    std::string params = stagingDir + " " + std::to_string((unsigned long)pid);
    SHELLEXECUTEINFOA sei = {};
    sei.cbSize       = sizeof(sei);
    sei.fMask        = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb       = "runas";
    sei.lpFile       = updaterPath.c_str();
    sei.lpParameters = params.c_str();
    sei.nShow        = SW_HIDE;
    if (!ShellExecuteExA(&sei)) {
        LOG_ERROR("cmd", "update_launch_updater: ShellExecuteEx(runas) failed err=%lu",
            (unsigned long)GetLastError());
        return false;
    }
    if (sei.hProcess) CloseHandle(sei.hProcess);
    LOG("cmd", "update_launch_updater: updater launched (elevated), staging=%s", stagingDir.c_str());
    return true;
}

// ── Run-as permission (Medium=normal / High=admin integrity) ────────────────

extern void app_release_singleton();  // main.cpp — frees the single-instance mutex
extern void app_acquire_singleton();  // main.cpp — re-grabs it if a relaunch aborts

// True if this process runs elevated (High integrity == admin).
static bool process_is_elevated() {
    bool elevated = false;
    HANDLE hTok = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hTok)) {
        TOKEN_ELEVATION te; DWORD cb = 0;
        if (GetTokenInformation(hTok, TokenElevation, &te, sizeof(te), &cb))
            elevated = te.TokenIsElevated != 0;
        CloseHandle(hTok);
    }
    return elevated;
}

// Persist the "always run as admin" preference via the shell compat flag
// (HKCU\...\AppCompatFlags\Layers = "~ RUNASADMIN" for this exe). With it set,
// double-clicking the exe auto-prompts UAC — so "last choice = admin" survives a
// restart WITHOUT the app relaunching itself (no double-flash).
static void set_run_as_admin_flag(bool enable) {
    char exePath[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    LOG("cmd", "set_run_as_admin_flag: enable=%d exe=%s", (int)enable, exePath);
    const char* sub = "Software\\Microsoft\\Windows NT\\CurrentVersion\\AppCompatFlags\\Layers";
    HKEY hKey = nullptr;
    if (enable) {
        if (RegCreateKeyExA(HKEY_CURRENT_USER, sub, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &hKey, nullptr) == ERROR_SUCCESS) {
            const char* val = "~ RUNASADMIN";
            RegSetValueExA(hKey, exePath, 0, REG_SZ, (const BYTE*)val, (DWORD)strlen(val) + 1);
            RegCloseKey(hKey);
            LOG("cmd", "set_run_as_admin_flag: wrote RUNASADMIN");
        } else { LOG_WARN("cmd", "set_run_as_admin_flag: RegCreateKeyExA failed"); }
    } else {
        LONG err = RegOpenKeyExA(HKEY_CURRENT_USER, sub, 0, KEY_SET_VALUE, &hKey);
        if (err == ERROR_SUCCESS) {
            err = RegDeleteValueA(hKey, exePath);
            RegCloseKey(hKey);
            LOG("cmd", "set_run_as_admin_flag: RegDeleteValueA -> %ld", (long)err);
        } else { LOG_WARN("cmd", "set_run_as_admin_flag: RegOpenKeyExA failed err=%ld", (long)err); }
    }
}

// Relaunch a fresh ELEVATED copy of ourselves (UAC prompt).
static bool relaunch_as_admin() {
    char exePath[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    SHELLEXECUTEINFOA sei = {};
    sei.cbSize = sizeof(sei);
    sei.fMask  = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = "runas";
    sei.lpFile = exePath;
    sei.nShow  = SW_SHOWNORMAL;
    bool ok = ShellExecuteExA(&sei) != FALSE;
    if (sei.hProcess) CloseHandle(sei.hProcess);
    return ok;
}

// Relaunch a fresh copy at MEDIUM integrity (normal user). Uses the standard
// Windows technique: the elevated token's TokenLinkedToken is the original
// non-elevated (Medium) token from before UAC. No manual IL manipulation needed.
static bool relaunch_as_medium() {
    char exePath[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    // Enable CreateProcessAsUserW privileges (present in admin tokens but may be disabled).
    HANDLE hAdj = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_ADJUST_PRIVILEGES, &hAdj)) {
        const char* privs[] = { "SeAssignPrimaryTokenPrivilege", "SeIncreaseQuotaPrivilege" };
        for (const char* pn : privs) {
            LUID luid;
            if (LookupPrivilegeValueA(nullptr, pn, &luid)) {
                TOKEN_PRIVILEGES tp = {}; tp.PrivilegeCount = 1;
                tp.Privileges[0].Luid = luid;
                tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
                AdjustTokenPrivileges(hAdj, FALSE, &tp, sizeof(tp), nullptr, nullptr);
            }
        }
        CloseHandle(hAdj);
    }
    HANDLE hSelf = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hSelf))
        return false;
    // The linked token is the pre-UAC-elevation token at Medium IL — exactly
    // what we need for a normal-user process.
    HANDLE hLinked = nullptr;
    TOKEN_LINKED_TOKEN tlt = {};
    DWORD cb = 0;
    GetTokenInformation(hSelf, TokenLinkedToken, &tlt, sizeof(tlt), &cb);
    hLinked = tlt.LinkedToken;
    bool ok = false;
    if (hLinked) {
        wchar_t wexe[MAX_PATH] = {};
        MultiByteToWideChar(CP_UTF8, 0, exePath, -1, wexe, MAX_PATH);
        STARTUPINFOW si = {}; si.cb = sizeof(si);
        PROCESS_INFORMATION pi = {};
        if (CreateProcessAsUserW(hLinked, wexe, nullptr, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
            ok = true;
        } else {
            LOG_ERROR("cmd", "relaunch_as_medium: CreateProcessAsUserW(linked token) failed err=%lu",
                (unsigned long)GetLastError());
        }
        CloseHandle(hLinked);
    } else {
        LOG_ERROR("cmd", "relaunch_as_medium: TokenLinkedToken not found — cannot launch normal process");
    }
    CloseHandle(hSelf);
    return ok;
}

// get_elevation -> {ok, admin}
static std::string cmd_get_elevation() {
    return std::string("{\"ok\":true,\"admin\":") + (process_is_elevated() ? "true" : "false") + "}";
}

// switch_permission {admin}: persist the preference, relaunch at the target
// integrity, then close this instance (posted to the UI thread AFTER the result
// is returned to JS). No-op if already at the target level.
static std::string cmd_switch_permission(bool toAdmin) {
    bool already = process_is_elevated();
    set_run_as_admin_flag(toAdmin);  // write/delete flag BEFORE relaunch (now logs result)
    if (toAdmin == already) return R"({"ok":true,"changed":false})";
    LOG("cmd", "switch_permission: toAdmin=%d already=%d", (int)toAdmin, (int)already);
    // Free the single-instance lock BEFORE relaunching, else the new copy hits the
    // guard and exits (code 2) since this instance hasn't fully quit yet.
    app_release_singleton();
    bool ok = toAdmin ? relaunch_as_admin() : relaunch_as_medium();
    if (!ok) {
        app_acquire_singleton();  // relaunch aborted (e.g. UAC denied) → restore the lock
        LOG_ERROR("cmd", "switch_permission: relaunch (%s) failed err=%lu",
            toAdmin ? "admin" : "normal", (unsigned long)GetLastError());
        return R"({"ok":false,"error":"relaunch failed"})";
    }
    HWND hwnd = (HWND)get_main_hwnd();
    if (hwnd) PostMessageW(hwnd, WM_CLOSE, 0, 0);   // quit this instance after reply
    return R"({"ok":true,"changed":true})";
}

// Compare dotted numeric versions: version_lt("0.3.7","0.3.10") == true.
// Missing trailing segments count as 0. Non-digits within a segment are ignored.
static bool version_lt(const std::string& a, const std::string& b) {
    size_t ia = 0, ib = 0;
    while (ia < a.size() || ib < b.size()) {
        long na = 0, nb = 0;
        while (ia < a.size() && a[ia] != '.') { if (a[ia] >= '0' && a[ia] <= '9') na = na * 10 + (a[ia] - '0'); ia++; }
        while (ib < b.size() && b[ib] != '.') { if (b[ib] >= '0' && b[ib] <= '9') nb = nb * 10 + (b[ib] - '0'); ib++; }
        if (na != nb) return na < nb;
        if (ia < a.size()) ia++;  // skip '.'
        if (ib < b.size()) ib++;
    }
    return false;
}

// Highest manifest schema this client understands. A remote manifest with a
// higher "schema" means the update needs a newer bootstrap → tell the user to
// install the full package rather than guessing at fields we don't know.
static const int KNOWN_SCHEMA = 2;

// check_update — query Gitee for latest release; per-file diff by sha256.
// forceFull (or remote "full_update":true) → include every file (full package).
static std::string cmd_check_update(bool forceFull) {
    // Dev/test override: GAM_UPDATE_TAG points the check at an explicit tag and
    // skips the releases API, so the update chain can be exercised against any
    // pushed tag (even a non-latest "test" tag) without publishing a release.
    std::string tag, name, changelog;
    char tagEnv[128] = {};
    DWORD tagEnvLen = GetEnvironmentVariableA("GAM_UPDATE_TAG", tagEnv, sizeof(tagEnv));
    if (tagEnvLen > 0 && tagEnvLen < sizeof(tagEnv)) {
        tag = tagEnv;
        LOG_WARN("cmd", "check_update: GAM_UPDATE_TAG override -> %s (skipping releases API)", tag.c_str());
    } else {
        LOG("cmd", "check_update: querying Gitee API...");
        std::string body = winhttp_get(
            L"https://gitee.com/api/v5/repos/Andyqwe44/tictactoe/releases/latest",
            "update");
        if (body.empty()) {
            LOG("cmd", "check_update: HTTP request failed");
            return R"({"ok":false,"error":"network request failed"})";
        }
        tag       = json_val(body, "tag_name");
        name      = json_val(body, "name");
        changelog = json_val(body, "body");
    }
    // Strip leading 'v' for comparison
    std::string latest = tag;
    if (!latest.empty() && (latest[0] == 'v' || latest[0] == 'V'))
        latest = latest.substr(1);
    std::string current = APP_VERSION;

    bool hasUpdate = !latest.empty() && latest != current;
    std::string diffJson = "[]";
    bool useFull = forceFull;
    // Server-controllable policy (parsed from the manifest below); defaults are
    // the safe no-op so an old (schema-1) manifest behaves exactly as before.
    std::string message, downloadBase;
    bool mandatory = false;

    if (hasUpdate) {
        // Fetch remote version.json from the release tag, with retries. The raw
        // CDN can lag right after a release is published (302 propagation / tag
        // raw not yet live). A transient miss must NEVER be reported as "no
        // update" — that was the 0.3.x update-stuck bug.
        std::string manifestUrl = "https://gitee.com/Andyqwe44/tictactoe/raw/";
        manifestUrl += tag;
        manifestUrl += "/release/GameAgentMonitor/version.json";
        std::string remoteManifest;
        for (int attempt = 1; attempt <= 3; attempt++) {
            remoteManifest = winhttp_get_str(manifestUrl, "update");
            if (!remoteManifest.empty() && remoteManifest.find("\"files\"") != std::string::npos)
                break;
            LOG_WARN("cmd", "check_update: manifest attempt %d empty/invalid (len=%zu), retrying",
                attempt, remoteManifest.size());
            Sleep(500);
        }

        // Hard-fail on a missing/invalid manifest. Return ok:false (NOT
        // has_update + empty diff) so the UI shows a real error instead of a
        // misleading "already latest / nothing to update". 铁律 5.
        if (remoteManifest.empty() || remoteManifest.find("\"files\"") == std::string::npos) {
            LOG_ERROR("cmd", "check_update: manifest fetch FAILED after retries (len=%zu) url=%s",
                remoteManifest.size(), manifestUrl.c_str());
            return "{\"ok\":false,\"error\":\"manifest fetch failed (network/CDN) - please retry\""
                   ",\"current\":\"" + json_escape(current) + "\""
                   ",\"latest\":\"" + json_escape(latest) + "\"}";
        }

        // ── Manifest schema v2 policy (all fields optional / forward-compatible) ──
        // schema: reject a manifest newer than we understand rather than guessing.
        std::string schemaStr = json_val(remoteManifest, "schema");
        int remoteSchema = schemaStr.empty() ? 1 : atoi(schemaStr.c_str());
        if (remoteSchema > KNOWN_SCHEMA) {
            LOG_ERROR("cmd", "check_update: manifest schema %d > known %d - client too old for incremental",
                remoteSchema, KNOWN_SCHEMA);
            std::string relUrl = "https://gitee.com/Andyqwe44/tictactoe/releases/tag/" + tag;
            return "{\"ok\":false,\"needs_full_installer\":true"
                   ",\"error\":\"this update needs a newer installer - please download the full package\""
                   ",\"current\":\"" + json_escape(current) + "\""
                   ",\"latest\":\"" + json_escape(latest) + "\""
                   ",\"download_url\":\"" + json_escape(relUrl) + "\"}";
        }
        // download_base: build every file URL from this (server can move host/CDN
        // without a client rebuild). message/mandatory: server steers the UI.
        downloadBase = json_val(remoteManifest, "download_base");
        message      = json_val(remoteManifest, "message");
        mandatory    = json_val(remoteManifest, "mandatory") == "true";
        // min_version: a client older than this cannot incrementally cross a
        // breaking change → force a full download of every file.
        std::string minVer = json_val(remoteManifest, "min_version");
        if (!minVer.empty() && version_lt(current, minVer)) {
            useFull = true;
            LOG_WARN("cmd", "check_update: current %s < min_version %s - forcing full update",
                current.c_str(), minVer.c_str());
        }

        // Read local version.json (installed manifest). Missing -> all changed.
        std::string installDir = paths_get_install_dir();
        std::string localPath = installDir + "\\version.json";
        std::string localManifest = read_file(localPath.c_str());
        if (localManifest.empty())
            LOG_WARN("cmd", "check_update: local manifest missing (%s) - all files count as changed",
                localPath.c_str());

        // Remote may force a full update (updater/protocol changed).
        if (json_val(remoteManifest, "full_update") == "true") useFull = true;

        // Compare manifests: include files whose sha256 differs (or all, if full).
        // remoteManifest is guaranteed non-empty and to contain "files" here.
        {
            diffJson = "[";
            bool first = true;
            // Parse remote files object — extract each "path": {"v":..., ...}
            size_t filesPos = remoteManifest.find("\"files\"");
            if (filesPos != std::string::npos) {
                // Walk through each file entry
                size_t pos = remoteManifest.find("{", filesPos);
                if (pos != std::string::npos) {
                    int depth = 0;
                    for (size_t i = pos; i < remoteManifest.size(); i++) {
                        if (remoteManifest[i] == '{') depth++;
                        else if (remoteManifest[i] == '}') { depth--; if (depth == 0) break; }
                        else if (depth == 1 && remoteManifest[i] == '"' && (i == pos+1 || remoteManifest[i-1] != '\\')) {
                            // Start of a key (file path)
                            size_t keyEnd = remoteManifest.find("\"", i+1);
                            if (keyEnd == std::string::npos) break;
                            std::string filePath = remoteManifest.substr(i+1, keyEnd - i - 1);
                            std::string remoteSha = json_val(remoteManifest, "sha256", keyEnd);
                            std::string localSha  = json_val(localManifest, "sha256",
                                localManifest.find("\"" + filePath + "\""));
                            std::string remoteVer = json_val(remoteManifest, "v", keyEnd);
                            std::string sz = json_val(remoteManifest, "size", keyEnd);
                            // Incremental: content changed (sha differs). Full: everything.
                            bool changed = useFull || (!remoteSha.empty() && remoteSha != localSha);
                            if (changed) {
                                if (!first) diffJson += ","; first = false;
                                // Download URL from server-provided base (falls
                                // back to the tag path for a schema-1 manifest).
                                std::string dlUrl = !downloadBase.empty()
                                    ? downloadBase + filePath
                                    : ("https://gitee.com/Andyqwe44/tictactoe/raw/"
                                        + tag + "/release/GameAgentMonitor/" + filePath);
                                diffJson += "{\"path\":\"" + filePath + "\"";
                                diffJson += ",\"v\":\"" + remoteVer + "\"";
                                diffJson += ",\"sha256\":\"" + remoteSha + "\"";
                                diffJson += ",\"size\":" + (sz.empty() ? "0" : sz);
                                diffJson += ",\"url\":\"" + json_escape(dlUrl) + "\"}";
                            }
                            i = keyEnd;
                        }
                    }
                }
            }
            diffJson += "]";
        }
    }

    size_t nDiff = (size_t)std::count(diffJson.begin(), diffJson.end(), '{');
    if (hasUpdate && nDiff == 0)
        LOG_WARN("cmd", "check_update: hasUpdate but 0 files differ - local already matches remote content");
    LOG("cmd", "check_update: current=%s latest=%s hasUpdate=%d full=%d diff_files=%zu",
        current.c_str(), latest.c_str(), (int)hasUpdate, (int)useFull, nDiff);

    return "{\"ok\":true"
        ",\"current\":\"" + json_escape(current) + "\""
        ",\"latest\":\"" + json_escape(latest) + "\""
        ",\"name\":\"" + json_escape(name.empty() ? tag : name) + "\""
        ",\"body\":\"" + json_escape(changelog) + "\""
        ",\"has_update\":" + (hasUpdate ? "true" : "false")
        + ",\"mode\":\"" + (useFull ? "full" : "incremental") + "\""
        + ",\"mandatory\":" + (mandatory ? "true" : "false")
        + ",\"message\":\"" + json_escape(message) + "\""
        + ",\"diff\":" + diffJson + "}";
}

// Background download thread: fetch each diff file to staging, verify sha256,
// update g_up + throttled-post WM_UPDATE_PROGRESS. On success sets `succeeded`
// so WndProc launches updater; on any failure sets `failed` and stops.
static void download_thread_func(std::string diffJsonStr, std::string stagingDir) {
    HWND hwnd = (HWND)get_main_hwnd();
    auto post = [&]() { if (hwnd) PostMessageW(hwnd, WM_UPDATE_PROGRESS, 0, 0); };

    unsigned long long baseBytes = 0;  // bytes fully written for prior files
    int index = 0;
    ULONGLONG lastPost = 0;
    bool ok = true;

    size_t pos = 0;
    while ((pos = diffJsonStr.find("\"path\"", pos)) != std::string::npos) {
        std::string filePath = json_val(diffJsonStr, "path", pos);
        std::string wantSha  = json_val(diffJsonStr, "sha256", pos);
        std::string dlUrl    = json_val(diffJsonStr, "url", pos);
        pos++;
        if (filePath.empty() || dlUrl.empty()) continue;
        index++;

        {
            std::lock_guard<std::mutex> lk(g_up_mtx);
            g_up.current_file = index;
            g_up.file_path = filePath;
        }
        post();

        LOG("cmd", "download_update: getting %s", filePath.c_str());
        std::string data = winhttp_get_str(dlUrl, "update",
            [&](unsigned long long done, unsigned long long /*total*/) {
                {
                    std::lock_guard<std::mutex> lk(g_up_mtx);
                    g_up.done_bytes = baseBytes + done;
                }
                ULONGLONG now = GetTickCount64();
                if (now - lastPost >= 50) { lastPost = now; post(); }
            });

        if (data.empty()) {
            LOG("cmd", "download_update: FAILED download %s", filePath.c_str());
            ok = false;
            std::lock_guard<std::mutex> lk(g_up_mtx);
            g_up.failed = true; g_up.error_file = filePath;
            break;
        }

        // Integrity: downloaded bytes must match the manifest sha256.
        if (!wantSha.empty()) {
            std::string gotSha = sha256_hex(data.data(), data.size());
            if (gotSha != wantSha) {
                LOG("cmd", "download_update: SHA256 mismatch %s (want %s got %s)",
                    filePath.c_str(), wantSha.c_str(), gotSha.c_str());
                ok = false;
                std::lock_guard<std::mutex> lk(g_up_mtx);
                g_up.failed = true; g_up.error_file = filePath;
                break;
            }
        }

        // Write to staging, creating parent dirs.
        std::string outPath = stagingDir + "\\" + filePath;
        for (size_t i = stagingDir.size() + 1; i < outPath.size(); i++)
            if (outPath[i] == '\\' || outPath[i] == '/')
                CreateDirectoryA(outPath.substr(0, i).c_str(), nullptr);
        FILE* f = fopen(outPath.c_str(), "wb");
        if (!f) {
            LOG("cmd", "download_update: cannot write %s", outPath.c_str());
            ok = false;
            std::lock_guard<std::mutex> lk(g_up_mtx);
            g_up.failed = true; g_up.error_file = filePath;
            break;
        }
        fwrite(data.data(), 1, data.size(), f);
        fclose(f);
        baseBytes += data.size();
        {
            std::lock_guard<std::mutex> lk(g_up_mtx);
            g_up.done_bytes = baseBytes;
        }
        LOG("cmd", "download_update: wrote %s (%zu bytes)", filePath.c_str(), data.size());
        post();
    }

    {
        std::lock_guard<std::mutex> lk(g_up_mtx);
        g_up.active = false;
        g_up.succeeded = ok && index > 0;
        if (g_up.succeeded) g_up.done_bytes = g_up.total_bytes;
    }
    post();  // terminal push → WndProc sees done/error; on done it launches updater
    LOG("cmd", "download_update: thread finished ok=%d files=%d", (int)ok, index);
}

// download_update — spawn a background download thread and return immediately.
// Real-time progress + completion are delivered via WM_UPDATE_PROGRESS pushes.
static std::string cmd_download_update(const std::string& diffJsonStr) {
    LOG("cmd", "download_update: diff=%s", diffJsonStr.c_str());

    {
        std::lock_guard<std::mutex> lk(g_up_mtx);
        if (g_up.active) return R"({"ok":false,"error":"already_downloading"})";
    }

    // Resolve + create staging dir.
    wchar_t localAppData[MAX_PATH];
    std::string stagingDir;
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, localAppData))) {
        int len = WideCharToMultiByte(CP_UTF8, 0, localAppData, -1, nullptr, 0, nullptr, nullptr);
        std::string base(len - 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, localAppData, -1, &base[0], len, nullptr, nullptr);
        stagingDir = base + "\\GameAgentMonitor\\staging";
        CreateDirectoryA((base + "\\GameAgentMonitor").c_str(), nullptr);
        CreateDirectoryA(stagingDir.c_str(), nullptr);
    } else {
        return R"({"ok":false,"error":"cannot resolve appdata"})";
    }

    // Count files + total bytes for the progress bar.
    int totalFiles = 0;
    unsigned long long totalBytes = 0;
    {
        size_t p = 0;
        while ((p = diffJsonStr.find("\"path\"", p)) != std::string::npos) {
            totalFiles++;
            std::string sz = json_val(diffJsonStr, "size", p);
            if (!sz.empty()) totalBytes += _strtoui64(sz.c_str(), nullptr, 10);
            p++;
        }
    }
    if (totalFiles == 0) return R"({"ok":false,"error":"no files to download"})";

    {
        std::lock_guard<std::mutex> lk(g_up_mtx);
        g_up = UpdateProgress{};
        g_up.active = true;
        g_up.total_files = totalFiles;
        g_up.total_bytes = totalBytes;
        g_up.staging_dir = stagingDir;
    }

    std::thread(download_thread_func, diffJsonStr, stagingDir).detach();

    return "{\"ok\":true,\"started\":true,\"total_files\":" + std::to_string(totalFiles)
        + ",\"total_bytes\":" + std::to_string(totalBytes) + "}";
}

// ── Settings persistence ──────────────────────────────────
// Forward declaration of paths functions (defined in main.cpp via paths.cpp)
// We duplicate the path logic here to keep commands.cpp self-contained.
static std::string g_settings_path;

static const char* get_settings_path() {
    if (!g_settings_path.empty()) return g_settings_path.c_str();

    // Use %LOCALAPPDATA%\GameAgentMonitor\config\settings.json
    wchar_t localAppData[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, localAppData))) {
        int len = WideCharToMultiByte(CP_UTF8, 0, localAppData, -1, nullptr, 0, nullptr, nullptr);
        std::string base(len - 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, localAppData, -1, &base[0], len, nullptr, nullptr);
        g_settings_path = base + "\\GameAgentMonitor\\config\\settings.json";
    } else {
        // Fallback: next to EXE
        char exeDir[MAX_PATH];
        GetModuleFileNameA(nullptr, exeDir, MAX_PATH);
        char* slash = strrchr(exeDir, '\\');
        if (slash) *slash = '\0';
        g_settings_path = std::string(exeDir) + "\\config\\settings.json";
    }

    // Ensure parent directory exists
    std::string dir = g_settings_path;
    size_t lastSlash = dir.rfind('\\');
    if (lastSlash != std::string::npos) {
        std::string parent = dir.substr(0, lastSlash);
        CreateDirectoryA(parent.c_str(), nullptr);
        // Also create parent's parent in case config/ doesn't exist yet
        size_t prevSlash = parent.rfind('\\');
        if (prevSlash != std::string::npos)
            CreateDirectoryA(parent.substr(0, prevSlash).c_str(), nullptr);
    }

    return g_settings_path.c_str();
}

static std::string read_file(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return "";
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return "{}"; }
    std::string content(sz, '\0');
    fread(&content[0], 1, sz, f);
    fclose(f);
    return content;
}

static std::string cmd_get_settings() {
    const char* path = get_settings_path();
    std::string json = read_file(path);
    if (json.empty() || json.size() < 3) json = "{}";
    LOG("cmd", "get_settings: %s -> %zub", path, json.size());
    return "{\"ok\":true,\"settings\":" + json + "}";
}

static std::string cmd_set_setting(const std::string& key, const std::string& value) {
    if (key.empty()) return R"({"ok":false,"error":"key required"})";

    const char* path = get_settings_path();
    std::string json = read_file(path);
    if (json.empty() || json.size() < 3) json = "{}";

    // Simple JSON key-value update: add/overwrite key in the object
    // Strip outer braces
    std::string inner = json;
    if (!inner.empty() && inner.front() == '{') inner = inner.substr(1);
    if (!inner.empty() && inner.back() == '}') inner.pop_back();

    // Remove whitespace from ends
    while (!inner.empty() && (inner.front() == ' ' || inner.front() == '\n' || inner.front() == '\r')) inner = inner.substr(1);
    while (!inner.empty() && (inner.back() == ' ' || inner.back() == '\n' || inner.back() == '\r')) inner.pop_back();

    // Check if key already exists — remove old value
    std::string search = "\"" + key + "\":";
    size_t pos = inner.find(search);
    if (pos != std::string::npos) {
        // Find end of value (comma or end of string)
        size_t end = pos + search.length();
        int depth = 0;
        bool inStr = false;
        while (end < inner.size()) {
            char c = inner[end];
            if (inStr) {
                if (c == '"' && inner[end-1] != '\\') inStr = false;
            } else {
                if (c == '"') inStr = true;
                else if (c == '{' || c == '[') depth++;
                else if (c == '}' || c == ']') { if (depth > 0) depth--; else break; }
                else if (c == ',' && depth == 0) { end++; break; } // include comma
            }
            end++;
        }
        inner.erase(pos, end - pos);
        // Remove trailing comma at end
        while (!inner.empty() && inner.back() == ',') inner.pop_back();
    }

    // Append new key-value
    if (!inner.empty() && inner.back() != ',' && inner.front() != '\0') inner += ",";
    // JSON-escape the value string
    std::string escaped;
    for (char c : value) {
        if (c == '"') escaped += "\\\"";
        else if (c == '\\') escaped += "\\\\";
        else if (c == '\n') escaped += "\\n";
        else escaped += c;
    }
    inner += "\"" + key + "\":" + escaped;

    json = "{" + inner + "}";

    // Write back
    const char* dir = get_settings_path();
    std::string dirStr(dir);
    size_t lastSlash = dirStr.rfind('\\');
    if (lastSlash != std::string::npos)
        CreateDirectoryA(dirStr.substr(0, lastSlash).c_str(), nullptr);

    FILE* f = fopen(dir, "wb");
    if (!f) return R"({"ok":false,"error":"write failed"})";
    fwrite(json.data(), 1, json.size(), f);
    fclose(f);

    LOG("cmd", "set_setting: %s=%s", key.c_str(), value.c_str());
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
    else if (cmd == "read_live_log") result = cmd_read_live_log();
    else if (cmd == "benchmark_methods") {
        result = cmd_benchmark_methods(json_get_uint64(args, "hwnd"), json_get_str(args, "method"));
    }
    else if (cmd == "set_frame_dump") {
        result = cmd_set_frame_dump(
            json_get_int(args, "capture") != 0,
            json_get_int(args, "stream") != 0,
            json_get_str(args, "dir"));
    }
    else if (cmd == "pick_dir") result = cmd_pick_dir();
    else if (cmd == "open_dir") result = cmd_open_dir(json_get_str(args, "dir"));

    else if (cmd == "send_input") {
        result = cmd_send_input(args);
    }
    else if (cmd == "get_version") {
        result = "\"" APP_VERSION "\"";
    }
    else if (cmd == "show_window") {
        // Frontend's first frame is painted — reveal the window (kept hidden
        // through WebView2 startup to avoid a white flash). Idempotent host-side.
        app_post_show_window();
        result = R"({"ok":true})";
    }
    else if (cmd == "get_log_dir") {
        const char* log_dir = capture_log_get_dir();
        result = "{\"dir\":\"" + json_escape(log_dir ? log_dir : "") + "\"}";
    }
    else if (cmd == "pick_log_dir") {
        // Windows folder picker via IFileDialog (Vista+)
        HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        bool alreadyCOM = (hr == S_FALSE || hr == RPC_E_CHANGED_MODE);
        if (FAILED(hr) && !alreadyCOM) { result = "{\"dir\":\"\"}"; }
        else {
            IFileDialog* pfd = nullptr;
            hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&pfd));
            if (SUCCEEDED(hr) && pfd) {
                DWORD opts;
                pfd->GetOptions(&opts);
                pfd->SetOptions(opts | FOS_PICKFOLDERS);
                hr = pfd->Show(nullptr);
                if (SUCCEEDED(hr)) {
                    IShellItem* psi;
                    hr = pfd->GetResult(&psi);
                    if (SUCCEEDED(hr) && psi) {
                        PWSTR pszPath = nullptr;
                        hr = psi->GetDisplayName(SIGDN_FILESYSPATH, &pszPath);
                        if (SUCCEEDED(hr) && pszPath) {
                            int len = WideCharToMultiByte(CP_UTF8, 0, pszPath, -1, nullptr, 0, nullptr, nullptr);
                            std::string path(len, '\0');
                            WideCharToMultiByte(CP_UTF8, 0, pszPath, -1, &path[0], len, nullptr, nullptr);
                            while (!path.empty() && path.back() == '\0') path.pop_back();
                            result = "{\"dir\":\"" + json_escape(path) + "\"}";
                            CoTaskMemFree(pszPath);
                        }
                        psi->Release();
                    }
                }
                pfd->Release();
            }
            if (result.empty()) result = "{\"dir\":\"\"}";
            if (!alreadyCOM) CoUninitialize();
        }
    }
    else if (cmd == "launch_test_target") {
        // Toggle: if test_target window already exists, close it.
        // Otherwise launch a new instance.
        HWND hTest = FindWindowW(L"GAMTestTarget", L"GAM Test Target");
        if (hTest) {
            PostMessageW(hTest, WM_CLOSE, 0, 0);
            LOG("cmd", "launch_test_target: close existing window hwnd=0x%llx", (unsigned long long)(uintptr_t)hTest);
            result = R"({"ok":true,"action":"closed"})";
        } else {
            char exeDir[MAX_PATH];
            GetModuleFileNameA(nullptr, exeDir, MAX_PATH);
            char* lastSlash = strrchr(exeDir, '\\');
            if (lastSlash) *lastSlash = '\0';
            char* p = strrchr(exeDir, '\\'); if (p) *p = '\0';
            p = strrchr(exeDir, '\\'); if (p) *p = '\0';
            std::string path = std::string(exeDir) + "\\test_target\\test_target.exe";
            LOG("cmd", "launch_test_target: %s", path.c_str());
            HINSTANCE h = ShellExecuteA(nullptr, "open", path.c_str(), nullptr, nullptr, SW_SHOW);
            if ((INT_PTR)h > 32) {
                result = R"({"ok":true,"action":"launched"})";
            } else {
                result = "{\"ok\":false,\"error\":\"failed to launch, code=" +
                         std::to_string((int)(INT_PTR)h) + "\"}";
            }
        }
    }
    else if (cmd == "find_test_target") {
        HWND h = FindWindowW(L"GAMTestTarget", L"GAM Test Target");
        char b[64];
        snprintf(b, sizeof(b), "{\"hwnd\":%llu}", (unsigned long long)(uintptr_t)h);
        result = b;
    }
    else if (cmd == "selftest_connect") {
        result = cmd_selftest_connect(json_get_int(args, "port"));
    }
    else if (cmd == "selftest_disconnect") {
        result = cmd_selftest_disconnect();
    }
    else if (cmd == "get_self_rect") {
        HWND self = (HWND)get_main_hwnd();
        RECT r = {};
        if (self && GetWindowRect(self, &r)) {
            char buf[128];
            snprintf(buf, sizeof(buf), R"({"x":%ld,"y":%ld,"w":%ld,"h":%ld})",
                     r.left, r.top, r.right - r.left, r.bottom - r.top);
            result = buf;
        } else {
            result = R"({"x":0,"y":0,"w":0,"h":0})";
        }
    }
    else if (cmd == "set_exclude_self") {
        bool exclude = json_get_int(args, "exclude") != 0;
        HWND self = (HWND)get_main_hwnd();
        if (self) {
            // WDA_EXCLUDEFROMCAPTURE = 0x11 (Windows 10 2004+)
            DWORD affinity = exclude ? 0x11 : 0;  // 0 = WDA_NONE
            if (SetWindowDisplayAffinity(self, affinity)) {
                LOG("cmd", "set_exclude_self: %d OK", (int)exclude);
                result = R"({"ok":true})";
            } else {
                DWORD err = GetLastError();
                LOG("cmd", "set_exclude_self: FAILED err=%lu", (unsigned long)err);
                result = "{\"ok\":false,\"error\":\"SetWindowDisplayAffinity failed (requires Windows 10 2004+)\"}";
            }
        } else {
            result = R"({"ok":false,"error":"no main window"})";
        }
    }
    else if (cmd == "cursor_overlay") {
        int show = json_get_int(args, "show");
        if (!show) {
            cursor_overlay_hide();
            result = R"({"ok":true})";
        } else {
            uint64_t hwnd = json_get_uint64(args, "hwnd");
            double x_norm = json_get_double(args, "x_norm");
            double y_norm = json_get_double(args, "y_norm");
            HWND h = (HWND)(uintptr_t)hwnd;
            int sx = 0, sy = 0;
            if (h) {
                RECT wr;
                if (GetWindowRect(h, &wr)) {
                    sx = wr.left + (int)(x_norm * (double)(wr.right - wr.left));
                    sy = wr.top  + (int)(y_norm * (double)(wr.bottom - wr.top));
                }
            } else {
                sx = GetSystemMetrics(SM_XVIRTUALSCREEN) + (int)(x_norm * (double)GetSystemMetrics(SM_CXVIRTUALSCREEN));
                sy = GetSystemMetrics(SM_YVIRTUALSCREEN) + (int)(y_norm * (double)GetSystemMetrics(SM_CYVIRTUALSCREEN));
            }
            cursor_overlay_show(sx, sy);
            result = R"({"ok":true})";
        }
    }
    else if (cmd == "screen_info") {
        int sx = GetSystemMetrics(SM_XVIRTUALSCREEN);
        int sy = GetSystemMetrics(SM_YVIRTUALSCREEN);
        int sw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        int sh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        result = "{\"x\":" + std::to_string(sx) + ",\"y\":" + std::to_string(sy) +
                 ",\"w\":" + std::to_string(sw) + ",\"h\":" + std::to_string(sh) + "}";
    }
    else if (cmd == "window_state") {
        auto* hw = (HWND)(uintptr_t)json_get_uint64(args, "hwnd");
        const char* state = capture_query_window_state(hw);
        result = "\"" + std::string(state ? state : "unknown") + "\"";
        if (state) capture_free_string(state);
    }
    else if (cmd == "list_desktops") {
        result = vd_list_desktops();
    }
    else if (cmd == "switch_desktop") {
        result = vd_switch_desktop(json_get_int(args, "index"));
    }
    else if (cmd == "check_update") {
        bool ff = args.find("\"force_full\":true") != std::string::npos;
        result = cmd_check_update(ff);
    }
    else if (cmd == "download_update") {
        // Extract diff array from args JSON directly
        // args looks like: {"diff": "[...]"}
        size_t p = args.find("\"diff\"");
        std::string diffArr = "[]";
        if (p != std::string::npos) {
            p = args.find("[", p);
            if (p != std::string::npos) {
                int depth = 0;
                size_t e = p;
                while (e < args.size()) {
                    if (args[e] == '[') depth++;
                    else if (args[e] == ']') { depth--; if (depth == 0) { e++; break; } }
                    e++;
                }
                diffArr = args.substr(p, e - p);
            }
        }
        // The diff may arrive double-encoded (a JSON string value), so its quotes
        // are backslash-escaped: [{\"path\":...}]. Unescape \" -> " and \\ -> \ so
        // the parser finds "path". No-op for a clean array (diff data has no
        // backslashes). This was the "no files to download" bug on the first real
        // update run (check_update finds 21 files, download parsed 0).
        std::string diffClean;
        diffClean.reserve(diffArr.size());
        for (size_t i = 0; i < diffArr.size(); i++) {
            if (diffArr[i] == '\\' && i + 1 < diffArr.size()) { diffClean += diffArr[i + 1]; i++; }
            else diffClean += diffArr[i];
        }
        result = cmd_download_update(diffClean);
    }
    else if (cmd == "get_elevation") {
        result = cmd_get_elevation();
    }
    else if (cmd == "switch_permission") {
        bool toAdmin = args.find("\"admin\":true") != std::string::npos;
        result = cmd_switch_permission(toAdmin);
    }
    else if (cmd == "get_settings") {
        result = cmd_get_settings();
    }
    else if (cmd == "set_setting") {
        result = cmd_set_setting(json_get_str(args, "key"), json_get_str(args, "value"));
    }

    if (id <= 0) return result; // fire-and-forget (no id field)
    if (result.empty()) return "{\"error\":\"unknown command\"}";
    return result;  // HandleWebMessage in main.cpp wraps with {id, result}
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

extern unsigned long long g_boot_tick;  // perf: set in WinMain (main.cpp) for startup timing

void backend_init() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); // STA for WebView2/WIC
#ifdef DEV_MODE
    // Dev: keep logs next to the exe (self-contained build_dev\bin\, read by devprobe;
    // the dev dir is always writable, no need to reach into appdata).
    char exe_dir[MAX_PATH];
    GetModuleFileNameA(nullptr, exe_dir, MAX_PATH);
    char* last_slash = strrchr(exe_dir, '\\');
    if (last_slash) *last_slash = '\0';
    std::string log_dir = std::string(exe_dir) + "\\log";
#else
    // Prod: logs under %LOCALAPPDATA%\GameAgentMonitor\log — writable regardless of the
    // install drive (Program Files\bin is not user-writable, same root cause as the
    // white-screen bug) and cleaned by the uninstaller. paths_get_appdata_dir() has
    // already ensure_dir'd the log\ subfolder; paths_init() ran before backend_init().
    std::string log_dir = paths_get_appdata_dir() + "\\log";
#endif
    capture_log_init("agent", APP_VERSION, log_dir.c_str(), 5, 5000);
    capture_log_set_notify(on_log_notify);  // C++ LOG() → push to TS in real-time
#ifdef DEV_MODE
    capture_log_set_level(LOG_LEVEL_DEBUG);  // show all levels in dev
#else
    capture_log_set_level(LOG_LEVEL_INFO);   // hide DEBUG in prod
#endif
    LOG("cmd", "perf: backend_init entered t+%llums (blocks msg loop until done)",
        GetTickCount64() - g_boot_tick);
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

    LOG("cmd", "backend init OK t+%llums", GetTickCount64() - g_boot_tick);
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
    st_cleanup();          // drop self-test client link
    tcp_server_stop();
    capture_log_flush();
    capture_log_shutdown();
    g_wic = nullptr;
}
