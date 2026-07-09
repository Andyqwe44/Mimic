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
#include "version.h"
#include "../../logger/logger.h"
#include "../../capture/include/capture_methods.h"
#include "../../capture/include/capture_wgc_ffi.h"
#include "../dep/WebView2.h"  // IID_PPV_ARGS for ICoreWebView2Environment12/17
#include <shobjidl.h>  // IVirtualDesktopManager
#include "virtual_desktop.h"  // vd_list_desktops, vd_switch_desktop
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
#include <cerrno>

using Microsoft::WRL::ComPtr;

// Shared by main.cpp — pushed from stream thread
extern void PostJsonToWebView(const std::string& json);

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
static void on_log_notify(const char* ts, const char* tag, const char* msg) {
    std::string json = "{\"type\":\"log\",\"ts\":\"" + json_escape(ts)
                     + "\",\"tag\":\"" + json_escape(tag)
                     + "\",\"msg\":\"" + json_escape(msg) + "\"}";
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

static std::string cmd_capture_stream_start(uint64_t hwnd, const std::string& method, const std::string& transport) {
    // Stop any existing stream first
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
    LOG("cmd", "dump_frame_if_enabled: is_stream=%d enabled=%d dir='%s' capture_en=%d stream_en=%d",
        (int)is_stream, (int)enabled, g_dump_dir.c_str(),
        (int)g_dump_capture_frames, (int)g_dump_stream_frames);
    if (!enabled) return;
    dump_frame_to_disk(bgra, w, h, is_stream ? "stream" : "snap");
}


// ── Input mapping ──────────────────────────────────────────

// ── Input simulation helpers ────────────────────────────────

// Map key name to virtual key code (for named keys; single chars use ASCII)
static WORD vk_from_name(const std::string& name) {
    if (name.empty()) return 0;
    if (name.length() == 1) {
        char c = (char)toupper((unsigned char)name[0]);
        if (c >= 'A' && c <= 'Z') return (WORD)c;
        if (c >= '0' && c <= '9') return (WORD)c;
    }
    if (name == "Enter" || name == "Return") return VK_RETURN;
    if (name == "Tab") return VK_TAB;
    if (name == "Escape" || name == "Esc") return VK_ESCAPE;
    if (name == "Backspace" || name == "Back") return VK_BACK;
    if (name == "Delete" || name == "Del") return VK_DELETE;
    if (name == "Insert" || name == "Ins") return VK_INSERT;
    if (name == "Home") return VK_HOME;
    if (name == "End") return VK_END;
    if (name == "PageUp") return VK_PRIOR;
    if (name == "PageDown") return VK_NEXT;
    if (name == "Up" || name == "ArrowUp") return VK_UP;
    if (name == "Down" || name == "ArrowDown") return VK_DOWN;
    if (name == "Left" || name == "ArrowLeft") return VK_LEFT;
    if (name == "Right" || name == "ArrowRight") return VK_RIGHT;
    if (name == "Space" || name == " ") return VK_SPACE;
    if (name == "Ctrl" || name == "Control") return VK_CONTROL;
    if (name == "Shift") return VK_SHIFT;
    if (name == "Alt" || name == "Menu") return VK_MENU;
    if (name == "Win" || name == "Meta" || name == "LWin") return VK_LWIN;
    if (name == "RWin") return VK_RWIN;
    if (name == "F1") return VK_F1;   if (name == "F2") return VK_F2;
    if (name == "F3") return VK_F3;   if (name == "F4") return VK_F4;
    if (name == "F5") return VK_F5;   if (name == "F6") return VK_F6;
    if (name == "F7") return VK_F7;   if (name == "F8") return VK_F8;
    if (name == "F9") return VK_F9;   if (name == "F10") return VK_F10;
    if (name == "F11") return VK_F11; if (name == "F12") return VK_F12;
    if (name == "CapsLock") return VK_CAPITAL;
    if (name == "NumLock") return VK_NUMLOCK;
    if (name == "PrintScreen" || name == "PrtSc") return VK_SNAPSHOT;
    if (name == "ScrollLock") return VK_SCROLL;
    if (name == "Pause" || name == "Break") return VK_PAUSE;
    return 0;
}

static WORD scan_from_vk(WORD vk) {
    return (WORD)MapVirtualKeyA(vk, MAPVK_VK_TO_VSC);
}

static bool is_extended_key(WORD vk) {
    WORD scan = scan_from_vk(vk);
    return (scan & 0xE000) != 0; // E0/E1 scan prefix = extended keyboard key
}

// Normalized coords (0-1) → screen absolute coords (0-65535)
// Returns false if GetClientRect fails (window invalid/destroyed)
static bool norm_to_screen(HWND hWnd, double nx, double ny, DWORD& absX, DWORD& absY) {
    RECT cr;
    if (!GetClientRect(hWnd, &cr)) {
        LOG("cmd", "norm_to_screen: GetClientRect FAILED for hwnd=0x%llx", (unsigned long long)(uintptr_t)hWnd);
        absX = 0; absY = 0;
        return false;
    }
    POINT pt = { cr.left, cr.top }; ClientToScreen(hWnd, &pt);
    int sx = pt.x + (int)(nx * (cr.right - cr.left));
    int sy = pt.y + (int)(ny * (cr.bottom - cr.top));
    int vsX = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vsY = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vsW = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vsH = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    absX = (DWORD)(((double)(sx - vsX) / (double)vsW) * 65535.0);
    absY = (DWORD)(((double)(sy - vsY) / (double)vsH) * 65535.0);
    return true;
}

static bool norm_to_client(HWND hWnd, double nx, double ny, int& cx, int& cy) {
    RECT cr;
    if (!GetClientRect(hWnd, &cr)) {
        LOG("cmd", "norm_to_client: GetClientRect FAILED for hwnd=0x%llx", (unsigned long long)(uintptr_t)hWnd);
        cx = 0; cy = 0;
        return false;
    }
    cx = (int)(nx * (cr.right - cr.left));
    cy = (int)(ny * (cr.bottom - cr.top));
    return true;
}

// Parse drag path: "path":[{"x":0.5,"y":0.5},...]
static std::vector<std::pair<double,double>> parse_drag_path(const std::string& json) {
    std::vector<std::pair<double,double>> pts;
    std::string s = "\"path\":[";
    size_t p = json.find(s);
    if (p == std::string::npos) return pts;
    p += s.length();
    while (p < json.size() && json[p] != ']') {
        size_t obj = json.find('{', p);
        if (obj == std::string::npos) break;
        size_t end = json.find('}', obj);
        if (end == std::string::npos) break;
        // only parse objects before closing ]
        size_t close = json.find(']', p);
        if (end > close && close != std::string::npos) break;
        double x = 0, y = 0;
        size_t xp = json.find("\"x\":", obj);
        if (xp != std::string::npos && xp < end) x = strtod(json.c_str() + xp + 4, nullptr);
        size_t yp = json.find("\"y\":", obj);
        if (yp != std::string::npos && yp < end) y = strtod(json.c_str() + yp + 4, nullptr);
        pts.push_back({x, y});
        p = end + 1;
        while (p < json.size() && (json[p] == ' ' || json[p] == ',')) p++;
    }
    return pts;
}

// ── cmd_send_input ──────────────────────────────────────────
// args: full JSON args object string (parsed internally for type-specific fields)
static std::string cmd_send_input(const std::string& args) {
    uint64_t hwnd = json_get_uint64(args, "hwnd");
    std::string type = json_get_str(args, "type");
    std::string method = json_get_str(args, "method");
    std::string button = json_get_str(args, "button");
    double x_norm = json_get_double(args, "x_norm");
    double y_norm = json_get_double(args, "y_norm");

    if (hwnd == 0) {
        return "{\"ok\":false,\"error\":\"cannot send input to desktop\"}";
    }
    HWND hWnd = (HWND)(uintptr_t)hwnd;
    if (!IsWindow(hWnd)) {
        return "{\"ok\":false,\"error\":\"invalid window handle\"}";
    }

    if (method == "driver") {
        return "{\"ok\":false,\"error\":\"driver-level input not implemented (requires kernel driver)\"}";
    }

    // Pre-compute coordinate mappings for mouse events
    DWORD absX = 0, absY = 0;
    int clientX = 0, clientY = 0;
    bool coordsOk = true;
    if (!norm_to_screen(hWnd, x_norm, y_norm, absX, absY)) coordsOk = false;
    if (!norm_to_client(hWnd, x_norm, y_norm, clientX, clientY)) coordsOk = false;
    // For non-mouse types, coordinate failure is non-fatal
    bool isMouseType = (type == "click" || type == "dblclick" || type == "move" || type == "drag" || type == "wheel");
    if (!coordsOk && isMouseType) {
        return "{\"ok\":false,\"error\":\"failed to get target window client rect (window may be destroyed)\"}";
    }

    if (method == "sendinput") {
        // ═══ SendInput (application-level synthesized input) ═══

        if (type == "click" || type == "dblclick") {
            DWORD downFlag = (button == "right") ? MOUSEEVENTF_RIGHTDOWN :
                            (button == "middle") ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_LEFTDOWN;
            DWORD upFlag   = (button == "right") ? MOUSEEVENTF_RIGHTUP :
                            (button == "middle") ? MOUSEEVENTF_MIDDLEUP : MOUSEEVENTF_LEFTUP;

            auto doClick = [&]() -> bool {
                INPUT inputs[2] = {};
                inputs[0].type = INPUT_MOUSE;
                inputs[0].mi.dx = absX; inputs[0].mi.dy = absY;
                inputs[0].mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_MOVE | downFlag;
                inputs[1].type = INPUT_MOUSE;
                inputs[1].mi.dx = absX; inputs[1].mi.dy = absY;
                inputs[1].mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_MOVE | upFlag;
                UINT sent = SendInput(2, inputs, sizeof(INPUT));
                if (sent != 2) {
                    LOG("cmd", "send_input: SendInput click sent=%u (expected 2) — UIPI may have blocked", sent);
                    return false;
                }
                return true;
            };

            if (!doClick()) {
                return "{\"ok\":false,\"error\":\"SendInput failed (UIPI may block cross-privilege input)\"}";
            }
            if (type == "dblclick") {
                Sleep(GetDoubleClickTime() / 2);
                if (!doClick()) {
                    return "{\"ok\":false,\"error\":\"SendInput dblclick second click failed\"}";
                }
            }
            LOG("cmd", "send_input: %s at norm(%.3f,%.3f) → screen(%u,%u) method=%s",
                type.c_str(), x_norm, y_norm, absX, absY, method.c_str());
        }
        else if (type == "move") {
            INPUT input = {};
            input.type = INPUT_MOUSE;
            input.mi.dx = absX; input.mi.dy = absY;
            input.mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_MOVE;
            SendInput(1, &input, sizeof(INPUT));
            LOG("cmd", "send_input: move to norm(%.3f,%.3f) method=%s",
                x_norm, y_norm, method.c_str());
        }
        else if (type == "drag") {
            auto path = parse_drag_path(args);
            if (path.empty()) {
                return "{\"ok\":false,\"error\":\"drag requires path array with at least one point\"}";
            }
            DWORD downFlag = (button == "right") ? MOUSEEVENTF_RIGHTDOWN :
                            (button == "middle") ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_LEFTDOWN;
            DWORD upFlag   = (button == "right") ? MOUSEEVENTF_RIGHTUP :
                            (button == "middle") ? MOUSEEVENTF_MIDDLEUP : MOUSEEVENTF_LEFTUP;

            // Move to first point + press button
            {
                DWORD ax, ay;
                norm_to_screen(hWnd, path[0].first, path[0].second, ax, ay);
                INPUT inputs[2] = {};
                inputs[0].type = INPUT_MOUSE;
                inputs[0].mi.dx = ax; inputs[0].mi.dy = ay;
                inputs[0].mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_MOVE;
                inputs[1].type = INPUT_MOUSE;
                inputs[1].mi.dx = ax; inputs[1].mi.dy = ay;
                inputs[1].mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_MOVE | downFlag;
                SendInput(2, inputs, sizeof(INPUT));
            }

            // Move through remaining points
            for (size_t i = 1; i < path.size(); i++) {
                DWORD ax, ay;
                norm_to_screen(hWnd, path[i].first, path[i].second, ax, ay);
                INPUT input = {};
                input.type = INPUT_MOUSE;
                input.mi.dx = ax; input.mi.dy = ay;
                input.mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_MOVE;
                SendInput(1, &input, sizeof(INPUT));
                Sleep(5);
            }

            // Release button at last point
            {
                DWORD ax, ay;
                norm_to_screen(hWnd, path.back().first, path.back().second, ax, ay);
                INPUT input = {};
                input.type = INPUT_MOUSE;
                input.mi.dx = ax; input.mi.dy = ay;
                input.mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_MOVE | upFlag;
                SendInput(1, &input, sizeof(INPUT));
            }
            LOG("cmd", "send_input: drag %zu points method=%s", path.size(), method.c_str());
        }
        else if (type == "wheel") {
            int delta = (int)json_get_double(args, "delta");
            if (delta == 0) {
                // Only fall back to raw parse if field exists but double cast failed
                size_t dp = args.find("\"delta\":");
                if (dp != std::string::npos) {
                    delta = (int)strtol(args.c_str() + dp + 8, nullptr, 10);
                }
            }
            delta = -delta; // browser +down → Win32 +up
            INPUT input = {};
            input.type = INPUT_MOUSE;
            input.mi.dx = absX; input.mi.dy = absY;
            input.mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_MOVE | MOUSEEVENTF_WHEEL;
            input.mi.mouseData = (DWORD)delta;
            SendInput(1, &input, sizeof(INPUT));
            LOG("cmd", "send_input: wheel delta=%d at norm(%.3f,%.3f) method=%s",
                delta, x_norm, y_norm, method.c_str());
        }
        else if (type == "keydown" || type == "keyup" || type == "keypress") {
            int vk = json_get_int(args, "vk");
            std::string keyName = json_get_str(args, "key");
            if (vk == 0 && !keyName.empty()) vk = vk_from_name(keyName);
            if (vk == 0) {
                return "{\"ok\":false,\"error\":\"key requires valid vk or key name\"}";
            }
            WORD scan = scan_from_vk((WORD)vk);

            DWORD xtra = is_extended_key((WORD)vk) ? KEYEVENTF_EXTENDEDKEY : 0;

            INPUT input = {};
            input.type = INPUT_KEYBOARD;
            input.ki.wVk = (WORD)vk;
            input.ki.wScan = scan;
            if (type == "keyup") input.ki.dwFlags = KEYEVENTF_KEYUP | xtra;
            else input.ki.dwFlags = xtra;
            // keydown: flags = extended if applicable
            // keypress: send both down and up

            if (type == "keypress") {
                INPUT inputs[2] = {};
                inputs[0].type = INPUT_KEYBOARD;
                inputs[0].ki.wVk = (WORD)vk; inputs[0].ki.wScan = scan;
                inputs[0].ki.dwFlags = xtra; // down
                inputs[1].type = INPUT_KEYBOARD;
                inputs[1].ki.wVk = (WORD)vk; inputs[1].ki.wScan = scan;
                inputs[1].ki.dwFlags = KEYEVENTF_KEYUP | xtra;
                SendInput(2, inputs, sizeof(INPUT));
                Sleep(5); // ensure nonzero key duration
            } else {
                SendInput(1, &input, sizeof(INPUT));
            }
            LOG("cmd", "send_input: %s key=%s vk=%d scan=%d method=%s",
                type.c_str(), keyName.c_str(), vk, scan, method.c_str());
        }
        else if (type == "combo") {
            // modifiers (ctrl/shift/alt/meta) + main key
            bool ctrl  = json_get_bool(args, "ctrlKey");
            bool shift = json_get_bool(args, "shiftKey");
            bool alt   = json_get_bool(args, "altKey");
            bool meta  = json_get_bool(args, "metaKey");
            int vk = json_get_int(args, "vk");
            std::string keyName = json_get_str(args, "key");
            if (vk == 0 && !keyName.empty()) vk = vk_from_name(keyName);
            if (vk == 0) {
                return "{\"ok\":false,\"error\":\"combo requires valid vk or key name\"}";
            }

            // Build sequence: modifier keys down → main key press → modifier keys up (reverse)
            struct { bool active; WORD vk; } mods[] = {
                {ctrl, VK_CONTROL}, {shift, VK_SHIFT}, {alt, VK_MENU}, {meta, VK_LWIN}
            };

            std::vector<INPUT> batch;
            // Modifiers down
            for (auto& m : mods) {
                if (!m.active) continue;
                INPUT in = {}; in.type = INPUT_KEYBOARD;
                in.ki.wVk = m.vk; in.ki.wScan = scan_from_vk(m.vk);
                in.ki.dwFlags = is_extended_key(m.vk) ? KEYEVENTF_EXTENDEDKEY : 0;
                batch.push_back(in);
            }
            // Main key down
            DWORD mainXtra = is_extended_key((WORD)vk) ? KEYEVENTF_EXTENDEDKEY : 0;
            INPUT keyDown = {}; keyDown.type = INPUT_KEYBOARD;
            keyDown.ki.wVk = (WORD)vk; keyDown.ki.wScan = scan_from_vk((WORD)vk);
            keyDown.ki.dwFlags = mainXtra;
            batch.push_back(keyDown);
            // Main key up (after a gap for nonzero duration)
            {
                INPUT keyUp = {}; keyUp.type = INPUT_KEYBOARD;
                keyUp.ki.wVk = (WORD)vk; keyUp.ki.wScan = scan_from_vk((WORD)vk);
                keyUp.ki.dwFlags = KEYEVENTF_KEYUP | mainXtra;
                batch.push_back(keyUp);
            }
            // Modifiers up (reverse order)
            for (int i = 3; i >= 0; i--) {
                if (!mods[i].active) continue;
                INPUT in = {}; in.type = INPUT_KEYBOARD;
                in.ki.wVk = mods[i].vk; in.ki.wScan = scan_from_vk(mods[i].vk);
                in.ki.dwFlags = KEYEVENTF_KEYUP | (is_extended_key(mods[i].vk) ? KEYEVENTF_EXTENDEDKEY : 0);
                batch.push_back(in);
            }
            // Send batch, then ensure nonzero main-key duration
            SendInput((UINT)batch.size(), batch.data(), sizeof(INPUT));
            Sleep(5);
            LOG("cmd", "send_input: combo ctrl=%d shift=%d alt=%d meta=%d + key=%s vk=%d method=%s",
                ctrl, shift, alt, meta, keyName.c_str(), vk, method.c_str());
        }
        else if (type == "text") {
            std::string text = json_get_str(args, "text");
            if (text.empty()) {
                return "{\"ok\":false,\"error\":\"text type requires 'text' field\"}";
            }
            // Convert UTF-8 to UTF-16, send each char via KEYEVENTF_UNICODE
            int wlen = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0);
            if (wlen <= 0) {
                return "{\"ok\":false,\"error\":\"failed to convert text to UTF-16\"}";
            }
            std::vector<wchar_t> wbuf(wlen + 1);
            MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), wbuf.data(), wlen);
            for (int i = 0; i < wlen; i++) {
                INPUT inputs[2] = {};
                inputs[0].type = INPUT_KEYBOARD;
                inputs[0].ki.wScan = wbuf[i];
                inputs[0].ki.dwFlags = KEYEVENTF_UNICODE;
                inputs[1].type = INPUT_KEYBOARD;
                inputs[1].ki.wScan = wbuf[i];
                inputs[1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
                SendInput(2, inputs, sizeof(INPUT));
                Sleep(5);
            }
            LOG("cmd", "send_input: text \"%s\" (%d chars) method=%s",
                text.c_str(), wlen, method.c_str());
        }
        else {
            return "{\"ok\":false,\"error\":\"unknown input type: " + type + "\"}";
        }
    }
    else if (method == "postmessage") {
        // ═══ PostMessage (window message layer) ═══

        if (type == "click" || type == "dblclick") {
            UINT downMsg = (button == "right") ? WM_RBUTTONDOWN :
                          (button == "middle") ? WM_MBUTTONDOWN : WM_LBUTTONDOWN;
            UINT upMsg   = (button == "right") ? WM_RBUTTONUP :
                          (button == "middle") ? WM_MBUTTONUP : WM_LBUTTONUP;
            WPARAM wDown = (button == "right") ? MK_RBUTTON :
                          (button == "middle") ? MK_MBUTTON : MK_LBUTTON;
            LPARAM lp = MAKELPARAM(clientX, clientY);

            auto doClick = [&]() -> bool {
                if (!PostMessageW(hWnd, downMsg, wDown, lp)) {
                    LOG("cmd", "send_input: PostMessage down failed (hwnd=0x%llx)", (unsigned long long)hwnd);
                    return false;
                }
                Sleep(10);
                if (!PostMessageW(hWnd, upMsg, 0, lp)) {
                    LOG("cmd", "send_input: PostMessage up failed (hwnd=0x%llx)", (unsigned long long)hwnd);
                    return false;
                }
                return true;
            };
            if (!doClick()) {
                return "{\"ok\":false,\"error\":\"PostMessage failed (target window may be destroyed)\"}";
            }
            if (type == "dblclick") {
                Sleep(GetDoubleClickTime() / 2);
                if (!doClick()) {
                    return "{\"ok\":false,\"error\":\"PostMessage dblclick second click failed\"}";
                }
            }
        }
        else if (type == "move") {
            PostMessageW(hWnd, WM_MOUSEMOVE, 0, MAKELPARAM(clientX, clientY));
        }
        else if (type == "drag") {
            auto path = parse_drag_path(args);
            if (path.empty()) {
                return "{\"ok\":false,\"error\":\"drag requires path array with at least one point\"}";
            }
            UINT downMsg = (button == "right") ? WM_RBUTTONDOWN :
                          (button == "middle") ? WM_MBUTTONDOWN : WM_LBUTTONDOWN;
            UINT upMsg   = (button == "right") ? WM_RBUTTONUP :
                          (button == "middle") ? WM_MBUTTONUP : WM_LBUTTONUP;
            WPARAM wDown = (button == "right") ? MK_RBUTTON :
                          (button == "middle") ? MK_MBUTTON : MK_LBUTTON;

            // Move to start + press
            {
                int cx, cy;
                norm_to_client(hWnd, path[0].first, path[0].second, cx, cy);
                PostMessageW(hWnd, WM_MOUSEMOVE, 0, MAKELPARAM(cx, cy));
                PostMessageW(hWnd, downMsg, wDown, MAKELPARAM(cx, cy));
            }
            // Move through path
            for (size_t i = 1; i < path.size(); i++) {
                int cx, cy;
                norm_to_client(hWnd, path[i].first, path[i].second, cx, cy);
                PostMessageW(hWnd, WM_MOUSEMOVE, wDown, MAKELPARAM(cx, cy));
                Sleep(5);
            }
            // Release
            {
                int cx, cy;
                norm_to_client(hWnd, path.back().first, path.back().second, cx, cy);
                PostMessageW(hWnd, upMsg, 0, MAKELPARAM(cx, cy));
            }
        }
        else if (type == "wheel") {
            int delta = (int)json_get_double(args, "delta");
            if (delta == 0) {
                size_t dp = args.find("\"delta\":");
                if (dp != std::string::npos) {
                    delta = (int)strtol(args.c_str() + dp + 8, nullptr, 10);
                }
            }
            delta = -delta; // browser +down → Win32 +up
            PostMessageW(hWnd, WM_MOUSEWHEEL,
                MAKEWPARAM(0, (short)delta), MAKELPARAM(clientX, clientY));
        }
        else if (type == "keydown" || type == "keyup" || type == "keypress") {
            int vk = json_get_int(args, "vk");
            std::string keyName = json_get_str(args, "key");
            if (vk == 0 && !keyName.empty()) vk = vk_from_name(keyName);
            if (vk == 0) {
                return "{\"ok\":false,\"error\":\"key requires valid vk or key name\"}";
            }
            WORD scan = scan_from_vk((WORD)vk);

            auto doKey = [&](bool up) {
                UINT msg = up ? WM_KEYUP : WM_KEYDOWN;
                LPARAM lParam = MAKELPARAM(1, scan);
                if (up) lParam |= (1 << 31) | (1 << 30); // previous down + transition
                PostMessageW(hWnd, msg, (WPARAM)vk, lParam);
            };

            if (type == "keypress") {
                doKey(false);
                Sleep(5);
                doKey(true);
            } else {
                doKey(type == "keyup");
            }
        }
        else if (type == "combo") {
            bool ctrl  = json_get_bool(args, "ctrlKey");
            bool shift = json_get_bool(args, "shiftKey");
            bool alt   = json_get_bool(args, "altKey");
            bool meta  = json_get_bool(args, "metaKey");
            int vk = json_get_int(args, "vk");
            std::string keyName = json_get_str(args, "key");
            if (vk == 0 && !keyName.empty()) vk = vk_from_name(keyName);
            if (vk == 0) {
                return "{\"ok\":false,\"error\":\"combo requires valid vk or key name\"}";
            }

            struct { bool active; WORD vk; } mods[] = {
                {ctrl, VK_CONTROL}, {shift, VK_SHIFT}, {alt, VK_MENU}, {meta, VK_LWIN}
            };

            auto doKey = [&](WORD keyVk, bool up) {
                UINT msg = up ? WM_KEYUP : WM_KEYDOWN;
                LPARAM lParam = MAKELPARAM(1, scan_from_vk(keyVk));
                if (up) lParam |= (1 << 31) | (1 << 30);
                PostMessageW(hWnd, msg, (WPARAM)keyVk, lParam);
            };

            // Modifiers down
            for (auto& m : mods) { if (m.active) doKey(m.vk, false); }
            // Main key press
            doKey((WORD)vk, false); Sleep(5); doKey((WORD)vk, true);
            // Modifiers up (reverse)
            for (int i = 3; i >= 0; i--) { if (mods[i].active) doKey(mods[i].vk, true); }
        }
        else if (type == "text") {
            std::string text = json_get_str(args, "text");
            if (text.empty()) {
                return "{\"ok\":false,\"error\":\"text type requires 'text' field\"}";
            }
            int wlen = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0);
            if (wlen <= 0) {
                return "{\"ok\":false,\"error\":\"failed to convert text to UTF-16\"}";
            }
            std::vector<wchar_t> wbuf(wlen + 1);
            MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), wbuf.data(), wlen);
            for (int i = 0; i < wlen; i++) {
                PostMessageW(hWnd, WM_CHAR, wbuf[i], MAKELPARAM(1, 1));
                Sleep(5);
            }
        }
        else {
            return "{\"ok\":false,\"error\":\"unknown input type: " + type + "\"}";
        }

        LOG("cmd", "send_input: %s method=postmessage hwnd=0x%llx",
            type.c_str(), (unsigned long long)hwnd);
    }
    else if (method == "winapi") {
        // ═══ WinAPI (OS-level: AttachThreadInput + SetForegroundWindow + SendMessage) ═══
        // Attach to target thread so SendMessage properly updates input state
        DWORD targetTid = GetWindowThreadProcessId(hWnd, nullptr);
        DWORD myTid = GetCurrentThreadId();
        BOOL attached = (targetTid != myTid) ? AttachThreadInput(myTid, targetTid, TRUE) : FALSE;
        if (targetTid != myTid && !attached) {
            LOG("cmd", "send_input: AttachThreadInput FAILED (targetTid=%lu, myTid=%lu) — input state may not sync",
                (unsigned long)targetTid, (unsigned long)myTid);
        }

        // Bring window to foreground and set focus for mouse operations
        if (type == "click" || type == "dblclick" || type == "drag" || type == "move" || type == "wheel") {
            SetForegroundWindow(hWnd);
            SetFocus(hWnd);
            Sleep(30); // let window activation settle
        }

        if (type == "click" || type == "dblclick") {
            UINT downMsg = (button == "right") ? WM_RBUTTONDOWN :
                          (button == "middle") ? WM_MBUTTONDOWN : WM_LBUTTONDOWN;
            UINT upMsg   = (button == "right") ? WM_RBUTTONUP :
                          (button == "middle") ? WM_MBUTTONUP : WM_LBUTTONUP;
            WPARAM wDown = (button == "right") ? MK_RBUTTON :
                          (button == "middle") ? MK_MBUTTON : MK_LBUTTON;
            LPARAM lp = MAKELPARAM(clientX, clientY);

            auto doClick = [&]() {
                SendMessageW(hWnd, downMsg, wDown, lp);
                SendMessageW(hWnd, upMsg, 0, lp);
            };
            doClick();
            if (type == "dblclick") {
                Sleep(GetDoubleClickTime() / 2);
                doClick();
            }
        }
        else if (type == "move") {
            SendMessageW(hWnd, WM_MOUSEMOVE, 0, MAKELPARAM(clientX, clientY));
        }
        else if (type == "drag") {
            auto path = parse_drag_path(args);
            if (path.empty()) {
                if (attached) AttachThreadInput(myTid, targetTid, FALSE);
                return "{\"ok\":false,\"error\":\"drag requires path array with at least one point\"}";
            }
            UINT downMsg = (button == "right") ? WM_RBUTTONDOWN :
                          (button == "middle") ? WM_MBUTTONDOWN : WM_LBUTTONDOWN;
            UINT upMsg   = (button == "right") ? WM_RBUTTONUP :
                          (button == "middle") ? WM_MBUTTONUP : WM_LBUTTONUP;
            WPARAM wDown = (button == "right") ? MK_RBUTTON :
                          (button == "middle") ? MK_MBUTTON : MK_LBUTTON;

            // Move to start + press
            {
                int cx, cy;
                norm_to_client(hWnd, path[0].first, path[0].second, cx, cy);
                SendMessageW(hWnd, WM_MOUSEMOVE, 0, MAKELPARAM(cx, cy));
                SendMessageW(hWnd, downMsg, wDown, MAKELPARAM(cx, cy));
            }
            // Move through path
            for (size_t i = 1; i < path.size(); i++) {
                int cx, cy;
                norm_to_client(hWnd, path[i].first, path[i].second, cx, cy);
                SendMessageW(hWnd, WM_MOUSEMOVE, wDown, MAKELPARAM(cx, cy));
                Sleep(5);
            }
            // Release
            {
                int cx, cy;
                norm_to_client(hWnd, path.back().first, path.back().second, cx, cy);
                SendMessageW(hWnd, upMsg, 0, MAKELPARAM(cx, cy));
            }
        }
        else if (type == "wheel") {
            int delta = (int)json_get_double(args, "delta");
            if (delta == 0) {
                size_t dp = args.find("\"delta\":");
                if (dp != std::string::npos) {
                    delta = (int)strtol(args.c_str() + dp + 8, nullptr, 10);
                }
            }
            delta = -delta; // browser +down → Win32 +up
            SendMessageW(hWnd, WM_MOUSEWHEEL,
                MAKEWPARAM(0, (short)delta), MAKELPARAM(clientX, clientY));
        }
        else if (type == "keydown" || type == "keyup" || type == "keypress") {
            int vk = json_get_int(args, "vk");
            std::string keyName = json_get_str(args, "key");
            if (vk == 0 && !keyName.empty()) vk = vk_from_name(keyName);
            if (vk == 0) {
                if (attached) AttachThreadInput(myTid, targetTid, FALSE);
                return "{\"ok\":false,\"error\":\"key requires valid vk or key name\"}";
            }
            WORD scan = scan_from_vk((WORD)vk);

            auto doKey = [&](bool up) {
                UINT msg = up ? WM_KEYUP : WM_KEYDOWN;
                LPARAM lParam = MAKELPARAM(1, scan);
                if (up) lParam |= (1 << 31) | (1 << 30);
                SendMessageW(hWnd, msg, (WPARAM)vk, lParam);
            };

            if (type == "keypress") {
                doKey(false);
                Sleep(5);
                doKey(true);
            } else {
                doKey(type == "keyup");
            }
        }
        else if (type == "combo") {
            bool ctrl  = json_get_bool(args, "ctrlKey");
            bool shift = json_get_bool(args, "shiftKey");
            bool alt   = json_get_bool(args, "altKey");
            bool meta  = json_get_bool(args, "metaKey");
            int vk = json_get_int(args, "vk");
            std::string keyName = json_get_str(args, "key");
            if (vk == 0 && !keyName.empty()) vk = vk_from_name(keyName);
            if (vk == 0) {
                if (attached) AttachThreadInput(myTid, targetTid, FALSE);
                return "{\"ok\":false,\"error\":\"combo requires valid vk or key name\"}";
            }

            struct { bool active; WORD vk; } mods[] = {
                {ctrl, VK_CONTROL}, {shift, VK_SHIFT}, {alt, VK_MENU}, {meta, VK_LWIN}
            };

            auto doKey = [&](WORD keyVk, bool up) {
                UINT msg = up ? WM_KEYUP : WM_KEYDOWN;
                LPARAM lParam = MAKELPARAM(1, scan_from_vk(keyVk));
                if (up) lParam |= (1 << 31) | (1 << 30);
                SendMessageW(hWnd, msg, (WPARAM)keyVk, lParam);
            };

            // Modifiers down
            for (auto& m : mods) { if (m.active) doKey(m.vk, false); }
            // Main key press (with nonzero duration)
            doKey((WORD)vk, false); Sleep(5); doKey((WORD)vk, true);
            // Modifiers up (reverse)
            for (int i = 3; i >= 0; i--) { if (mods[i].active) doKey(mods[i].vk, true); }
        }
        else if (type == "text") {
            std::string text = json_get_str(args, "text");
            if (text.empty()) {
                if (attached) AttachThreadInput(myTid, targetTid, FALSE);
                return "{\"ok\":false,\"error\":\"text type requires 'text' field\"}";
            }
            int wlen = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0);
            if (wlen <= 0) {
                if (attached) AttachThreadInput(myTid, targetTid, FALSE);
                return "{\"ok\":false,\"error\":\"failed to convert text to UTF-16\"}";
            }
            std::vector<wchar_t> wbuf(wlen + 1);
            MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), wbuf.data(), wlen);
            for (int i = 0; i < wlen; i++) {
                SendMessageW(hWnd, WM_CHAR, wbuf[i], MAKELPARAM(1, 1));
                Sleep(5);
            }
        }
        else {
            if (attached) AttachThreadInput(myTid, targetTid, FALSE);
            return "{\"ok\":false,\"error\":\"unknown input type: " + type + "\"}";
        }

        // Detach thread input
        if (attached) {
            AttachThreadInput(myTid, targetTid, FALSE);
        }

        LOG("cmd", "send_input: %s method=winapi hwnd=0x%llx attached=%d",
            type.c_str(), (unsigned long long)hwnd, attached);
    }
    else {
        return "{\"ok\":false,\"error\":\"unknown input method: " + method + "\"}";
    }

    return "{\"ok\":true}";
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
    else if (cmd == "list_desktops") {
        result = vd_list_desktops();
    }
    else if (cmd == "switch_desktop") {
        result = vd_switch_desktop(json_get_int(args, "index"));
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

void backend_init() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); // STA for WebView2/WIC
    // Compute absolute log path from exe directory (not CWD — avoid scattered logs)
    char exe_dir[MAX_PATH];
    GetModuleFileNameA(nullptr, exe_dir, MAX_PATH);
    char* last_slash = strrchr(exe_dir, '\\');
    if (last_slash) *last_slash = '\0';
    std::string log_dir = std::string(exe_dir) + "\\log";
    capture_log_init("agent", APP_VERSION, log_dir.c_str(), 5, 5000);
    capture_log_set_notify(on_log_notify);  // C++ LOG() → push to TS in real-time
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
    capture_log_flush();
    capture_log_shutdown();
    g_wic = nullptr;
}
