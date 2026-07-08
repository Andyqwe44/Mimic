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

using Microsoft::WRL::ComPtr;

// Shared by main.cpp — pushed from stream thread
extern void shared_buffer_push_frame(const uint8_t* bgra, int w, int h);
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

static CaptureResult call_capture(uint64_t hwnd, const std::string& method) {
    std::vector<uint8_t> buf(MAX_PX);
    int w = 0, h = 0, size = 0;
    HWND hw = (HWND)(uintptr_t)hwnd;
    std::string used = method;

    if (method == "WGC" || method == "wgc") {
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

    // Push frame via SharedBuffer (zero-copy) — no more base64 PNG.
    // Frontend receives the frame via 'sharedbufferreceived' event.
    shared_buffer_push_frame(r.pixels.data(), r.w, r.h);

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
                // Push via SharedBuffer (zero-copy) + TCP (agents)
                shared_buffer_push_frame(buf.data(), w, h);
                tcp_broadcast_frame(buf.data(), w, h);
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

// ── Debug dump ────────────────────────────────────────────
static bool g_dump_frames = false;
static std::string cmd_debug_dump_frames(bool enable) {
    g_dump_frames = enable;
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
    else if (cmd == "debug_dump_frames") {
        result = cmd_debug_dump_frames(json_get_int(args, "enable") != 0);
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
