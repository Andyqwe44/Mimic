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
#include "update_verify.h"
#include "ws_client.h"
#include "peer_session.h"
#include "h264_encoder.h"
#include <d3d11.h>
#include "../../logger/logger.h"
#include "../../capture/include/capture_methods.h"
#include "../../capture/include/capture_wgc_ffi.h"
#include "../dep/WebView2.h"  // IID_PPV_ARGS for ICoreWebView2Environment12/17
#include <shobjidl.h>  // IVirtualDesktopManager
#include <shlobj.h>    // SHGetFolderPathW, CSIDL_LOCAL_APPDATA
#include "virtual_desktop.h"  // vd_list_desktops, vd_switch_desktop
#include <shellapi.h>  // ShellExecuteA
#include <exdisp.h>    // IShellWindows, CLSID_ShellWindows (explorer-launch)
#include <shldisp.h>   // IShellFolderViewDual, IShellDispatch2 (explorer-launch)
#include <servprov.h>  // IServiceProvider (explorer-launch)
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
#include <algorithm>
#include <cmath>

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
        char buf[768];
        const char* kind = (list[i].category == "desktop" || list[i].hwnd == 0) ? "desktop" : "window";
        char idbuf[64];
        if (list[i].hwnd == 0)
            snprintf(idbuf, sizeof(idbuf), "desktop:%d", list[i].desktop);
        else
            snprintf(idbuf, sizeof(idbuf), "hwnd:%llu", (unsigned long long)list[i].hwnd);
        snprintf(buf, sizeof(buf),
                 R"({"title":"%s","category":"%s","hwnd":%llu,"desktop":%d,"id":"%s","platform":"windows","kind":"%s"})",
                 json_escape(list[i].title).c_str(), list[i].category.c_str(),
                 (unsigned long long)list[i].hwnd, list[i].desktop, idbuf, kind);
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
// Active remote-control target (forced onto CONTROL_MSG actions).
static std::atomic<uint64_t> g_control_hwnd{0};
// Dual gates (SSOT): stream out vs apply remote control. Default both closed.
static std::atomic<bool> g_allow_stream{false};
static std::atomic<bool> g_accept_control{false};
static std::atomic<uint64_t> g_stream_session{0};

// Remote config from controller_server (browser settings UI).
static std::mutex g_remote_cfg_mtx;
static std::string g_remote_capture = "wgc";   // wgc | dxgi
static std::string g_remote_codec = "h264";
static std::string g_remote_input = "postmsg"; // seize | postmsg

// Fwd: remote CONTROL_MSG → send_input (defined with input dispatch).
static std::string execute_remote_control_json(const std::string& actionJson);
static void agent_push_status();
static void on_server_text(const std::string& json);

// ── TCP server (port 9999): H.264 NAL out + CONTROL_MSG JSON in ──
#include "../../../shared/protocol/protocol.h"

struct TcpClient {
    SOCKET sock = INVALID_SOCKET;
    std::thread reader;
    std::atomic<bool> alive{true};
};

static std::mutex g_tcp_mutex;
static std::vector<TcpClient*> g_tcp_clients;
static SOCKET g_tcp_listen = INVALID_SOCKET;
static std::thread g_tcp_accept_thread;
static std::atomic<bool> g_tcp_running{false};
static std::atomic<bool> g_h264_need_key{true};

static bool tcp_recv_exact(SOCKET s, char* buf, int n) {
    int got = 0;
    while (got < n) {
        int r = recv(s, buf + got, n - got, 0);
        if (r <= 0) return false;
        got += r;
    }
    return true;
}

static void tcp_client_reader(TcpClient* c) {
    while (g_tcp_running && c->alive) {
        uint8_t hdr[PROTOCOL_FRAME_HEADER];
        if (!tcp_recv_exact(c->sock, (char*)hdr, (int)PROTOCOL_FRAME_HEADER)) break;
        uint32_t payload_size = 0, type_tag = 0;
        if (!protocol_parse_header(hdr, payload_size, type_tag)) {
            LOG_WARN("cmd", "TCP: bad frame header from client");
            break;
        }
        if (payload_size > 1024 * 1024) {
            LOG_WARN("cmd", "TCP: control payload too large (%u)", payload_size);
            break;
        }
        std::vector<char> body(payload_size ? payload_size : 1);
        if (payload_size > 0 && !tcp_recv_exact(c->sock, body.data(), (int)payload_size)) break;

        if (type_tag == PAYLOAD_TYPE_CONTROL_MSG) {
            std::string json(body.data(), body.data() + payload_size);
            if (json.find("\"cmd\":\"ping\"") != std::string::npos)
                continue;
            std::string result = execute_remote_control_json(json);
            if (result.find("\"ok\":false") != std::string::npos)
                LOG_WARN("cmd", "TCP control rejected: %s", result.c_str());
        } else if (type_tag != PAYLOAD_TYPE_NONE) {
            LOG_DEBUG("cmd", "TCP: ignoring inbound type_tag=%u", type_tag);
        }
    }
    c->alive = false;
}

static void tcp_accept_loop() {
    while (g_tcp_running) {
        SOCKET s = accept(g_tcp_listen, nullptr, nullptr);
        if (s == INVALID_SOCKET) {
            if (g_tcp_running) { Sleep(100); continue; }
            else break;
        }
        int flag = 1;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&flag, sizeof(flag));
        auto* c = new TcpClient();
        c->sock = s;
        c->reader = std::thread(tcp_client_reader, c);
        {
            std::lock_guard<std::mutex> lk(g_tcp_mutex);
            g_tcp_clients.push_back(c);
        }
        g_h264_need_key.store(true); // new viewer needs an IDR soon
        LOG("cmd", "TCP controller connected");
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
    addr.sin_port = htons(PROTOCOL_DEFAULT_TCP_PORT);
    // LAN-visible so phone / another PC / Python can reach the agent.
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(g_tcp_listen, (sockaddr*)&addr, sizeof(addr)) != 0) {
        closesocket(g_tcp_listen); g_tcp_listen = INVALID_SOCKET; WSACleanup(); return false;
    }
    listen(g_tcp_listen, SOMAXCONN);
    g_tcp_running = true;
    g_tcp_accept_thread = std::thread(tcp_accept_loop);
    LOG("cmd", "TCP server started on 0.0.0.0:%u (frames out + CONTROL_MSG in)",
        PROTOCOL_DEFAULT_TCP_PORT);
    return true;
}

static void tcp_server_stop() {
    g_tcp_running = false;
    if (g_tcp_listen != INVALID_SOCKET) { closesocket(g_tcp_listen); g_tcp_listen = INVALID_SOCKET; }
    if (g_tcp_accept_thread.joinable()) g_tcp_accept_thread.join();
    std::lock_guard<std::mutex> lk(g_tcp_mutex);
    for (auto* c : g_tcp_clients) {
        c->alive = false;
        if (c->sock != INVALID_SOCKET) closesocket(c->sock);
        if (c->reader.joinable()) c->reader.join();
        delete c;
    }
    g_tcp_clients.clear();
    WSACleanup();
}

static bool tcp_send_all(SOCKET s, const char* data, int n) {
    int sent = 0;
    while (sent < n) {
        int r = send(s, data + sent, n - sent, 0);
        if (r == SOCKET_ERROR) return false;
        sent += r;
    }
    return true;
}

// H.264 body: [w:4][h:4][flags:4][reserved:4][annexb NALs...]
// flags bit0 = keyframe. Prefer this over raw BGRA for remote controllers.
static void tcp_broadcast_h264(const H264Packet& pkt) {
    uint32_t flags = pkt.keyframe ? 1u : 0u;
    uint32_t body_size = 16 + (uint32_t)pkt.annexb.size();
    uint8_t hdr[PROTOCOL_FRAME_HEADER];
    protocol_build_header(hdr, body_size, PAYLOAD_TYPE_H264_STREAM);
    uint32_t meta[4] = { (uint32_t)pkt.w, (uint32_t)pkt.h, flags, 0u };

    std::lock_guard<std::mutex> lk(g_tcp_mutex);
    for (auto it = g_tcp_clients.begin(); it != g_tcp_clients.end(); ) {
        TcpClient* c = *it;
        if (!c->alive ||
            !tcp_send_all(c->sock, (const char*)hdr, PROTOCOL_FRAME_HEADER) ||
            !tcp_send_all(c->sock, (const char*)meta, 16) ||
            (!pkt.annexb.empty() &&
             !tcp_send_all(c->sock, (const char*)pkt.annexb.data(), (int)pkt.annexb.size()))) {
            c->alive = false;
            if (c->sock != INVALID_SOCKET) { closesocket(c->sock); c->sock = INVALID_SOCKET; }
            if (c->reader.joinable()) c->reader.detach();
            delete c;
            it = g_tcp_clients.erase(it);
        } else {
            ++it;
        }
    }
}

static H264Encoder g_h264;
static std::mutex g_h264_mtx;

// HW: encode every WGC frame (no artificial throttle — latency first, ~30fps from WGC).
// Soft: still capped; soft 1080p cannot sustain real-time.
static constexpr int kRemoteEncodeMinIntervalMsHw = 0;
static constexpr int kRemoteEncodeMinIntervalMsSw = 50;   // ~20fps soft
static constexpr int kRemoteEncodeMaxWHw = 1920;
static constexpr int kRemoteEncodeMaxWSw = 1280;

/** Bilinear BGRA scale — softer than nearest-neighbor for soft-encode fallback. */
static void scale_bgra_bilinear(const uint8_t* src, int sw, int sh,
                                uint8_t* dst, int dw, int dh) {
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;
    for (int y = 0; y < dh; ++y) {
        float fy = ((float)y + 0.5f) * (float)sh / (float)dh - 0.5f;
        int y0 = (int)fy;
        if (y0 < 0) y0 = 0;
        if (y0 >= sh - 1) y0 = sh - 2;
        if (y0 < 0) y0 = 0;
        int y1 = y0 + 1;
        if (y1 >= sh) y1 = sh - 1;
        float wy = fy - (float)y0;
        if (wy < 0) wy = 0;
        if (wy > 1) wy = 1;
        const uint8_t* row0 = src + (size_t)y0 * sw * 4;
        const uint8_t* row1 = src + (size_t)y1 * sw * 4;
        uint8_t* drow = dst + (size_t)y * dw * 4;
        for (int x = 0; x < dw; ++x) {
            float fx = ((float)x + 0.5f) * (float)sw / (float)dw - 0.5f;
            int x0 = (int)fx;
            if (x0 < 0) x0 = 0;
            if (x0 >= sw - 1) x0 = sw - 2;
            if (x0 < 0) x0 = 0;
            int x1 = x0 + 1;
            if (x1 >= sw) x1 = sw - 1;
            float wx = fx - (float)x0;
            if (wx < 0) wx = 0;
            if (wx > 1) wx = 1;
            for (int c = 0; c < 4; ++c) {
                float v00 = row0[x0 * 4 + c];
                float v10 = row0[x1 * 4 + c];
                float v01 = row1[x0 * 4 + c];
                float v11 = row1[x1 * 4 + c];
                float v0 = v00 + (v10 - v00) * wx;
                float v1 = v01 + (v11 - v01) * wx;
                drow[x * 4 + c] = (uint8_t)(v0 + (v1 - v0) * wy + 0.5f);
            }
        }
    }
}

static void broadcast_h264_all(const H264Packet& pkt) {
    tcp_broadcast_h264(pkt);
    ws_client_send_h264(pkt);
    peer_send_h264(pkt);
}

static bool remote_has_viewers() {
    if (peer_media_ready() && peer_role() == PeerRole::Controlled) return true;
    if (ws_client_connected()) return true;
    std::lock_guard<std::mutex> lk(g_tcp_mutex);
    return !g_tcp_clients.empty();
}

// WGC worker: GPU texture → H.264 → TCP/WS.
// Soft encode at full 1080p was ~8fps / multi-second lag (see agent log SOFTWARE_FALLBACK).
static void on_wgc_gpu_frame(void* /*ctx*/, void* d3d_device, void* d3d_tex, int w, int h) {
    if (!g_streaming.load() || !g_allow_stream.load()) return;
    if (!d3d_device || !d3d_tex || w < 16 || h < 16) return;
    if (!remote_has_viewers()) return;

    static ULONGLONG s_last_enc_ms = 0;
    static int s_enc_w = 0, s_enc_h = 0;
    static bool s_h264_give_up = false;
    static bool s_prefer_soft_scale = false;
    static uint64_t s_stream_gen = 0;
    static std::vector<uint8_t> s_bgra_full;
    static std::vector<uint8_t> s_bgra_scaled;
    uint64_t gen = g_stream_session.load();
    if (gen != s_stream_gen) {
        s_stream_gen = gen;
        s_h264_give_up = false;
        s_prefer_soft_scale = false;
        s_enc_w = s_enc_h = 0;
        s_last_enc_ms = 0;
    }

    auto* dev = (ID3D11Device*)d3d_device;
    auto* tex = (ID3D11Texture2D*)d3d_tex;

    std::lock_guard<std::mutex> lk(g_h264_mtx);

    // First init: try HW at source size (needs WGC device VIDEO_SUPPORT).
    if (!s_h264_give_up && !g_h264.ready()) {
        int ew = w & ~1, eh = h & ~1;
        if (ew > kRemoteEncodeMaxWHw) {
            ew = kRemoteEncodeMaxWHw & ~1;
            eh = ((int)((int64_t)h * ew / w)) & ~1;
        }
        if (g_h264.init(dev, ew, eh, 30, 6000)) {
            s_enc_w = ew; s_enc_h = eh;
            s_prefer_soft_scale = !g_h264.hardware();
            g_h264_need_key.store(true);
            LOG("cmd", "H.264 %s %dx%d", g_h264.hardware() ? "HARDWARE" : "SOFTWARE_FALLBACK", ew, eh);
            // Soft at 1080p is unusable — re-init private soft encoder at 1280.
            if (s_prefer_soft_scale) {
                g_h264.shutdown();
                int sw = (w > kRemoteEncodeMaxWSw) ? kRemoteEncodeMaxWSw : (w & ~1);
                int sh = ((int)((int64_t)h * sw / w)) & ~1;
                if (sh < 16) sh = 16;
                if (!g_h264.init(sw, sh, 24, 4000)) {
                    s_h264_give_up = true;
                    LOG_WARN("cmd", "H.264 soft re-init %dx%d failed", sw, sh);
                    return;
                }
                s_enc_w = sw; s_enc_h = sh;
                g_h264_need_key.store(true);
                LOG_WARN("cmd", "H.264 SOFTWARE scaled %dx%d (1080p soft too slow)", sw, sh);
                peer_ui_enqueue(
                    "{\"type\":\"h264_encode\",\"path\":\"software\",\"w\":" +
                    std::to_string(sw) + ",\"h\":" + std::to_string(sh) + "}");
            } else {
                peer_ui_enqueue(
                    "{\"type\":\"h264_encode\",\"path\":\"hardware\",\"w\":" +
                    std::to_string(ew) + ",\"h\":" + std::to_string(eh) + "}");
            }
        } else {
            s_h264_give_up = true;
            LOG_WARN("cmd", "H.264 init failed — sticky give-up");
            return;
        }
    }
    if (!g_h264.ready()) return;

    int interval = g_h264.hardware() ? kRemoteEncodeMinIntervalMsHw : kRemoteEncodeMinIntervalMsSw;
    ULONGLONG now = GetTickCount64();
    if (now - s_last_enc_ms < (ULONGLONG)interval) return;
    s_last_enc_ms = now;

    if (g_h264_need_key.exchange(false))
        g_h264.request_keyframe();

    std::vector<H264Packet> pkts;
    if (g_h264.hardware() && !s_prefer_soft_scale) {
        int ew = w & ~1, eh = h & ~1;
        if (ew != s_enc_w || eh != s_enc_h) {
            // Resolution change — re-init next frame.
            g_h264.shutdown();
            s_enc_w = s_enc_h = 0;
            return;
        }
        if (!g_h264.encode_texture(tex, ew, eh, pkts) || pkts.empty()) return;
    } else {
        // Soft path: Map staging → scale → encode_bgra (keeps latency bounded).
        static ComPtr<ID3D11Texture2D> s_staging;
        static int s_stg_w = 0, s_stg_h = 0;
        ComPtr<ID3D11DeviceContext> ctx;
        dev->GetImmediateContext(&ctx);
        D3D11_TEXTURE2D_DESC td = {};
        tex->GetDesc(&td);
        if (!s_staging || s_stg_w != (int)td.Width || s_stg_h != (int)td.Height) {
            td.Usage = D3D11_USAGE_STAGING;
            td.BindFlags = 0;
            td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            td.MiscFlags = 0;
            s_staging.Reset();
            if (FAILED(dev->CreateTexture2D(&td, nullptr, &s_staging))) return;
            s_stg_w = (int)td.Width;
            s_stg_h = (int)td.Height;
        }
        ctx->CopyResource(s_staging.Get(), tex);
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (FAILED(ctx->Map(s_staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return;
        s_bgra_full.resize((size_t)w * h * 4);
        if ((int)mapped.RowPitch == w * 4) {
            memcpy(s_bgra_full.data(), mapped.pData, s_bgra_full.size());
        } else {
            for (int y = 0; y < h; ++y)
                memcpy(s_bgra_full.data() + (size_t)y * w * 4,
                       (uint8_t*)mapped.pData + y * mapped.RowPitch, (size_t)w * 4);
        }
        ctx->Unmap(s_staging.Get(), 0);

        int tw = s_enc_w, th = s_enc_h;
        const uint8_t* px = s_bgra_full.data();
        int pw = w, ph = h;
        if (tw != w || th != h) {
            s_bgra_scaled.resize((size_t)tw * th * 4);
            scale_bgra_bilinear(s_bgra_full.data(), w, h, s_bgra_scaled.data(), tw, th);
            px = s_bgra_scaled.data();
            pw = tw; ph = th;
        }
        if (!g_h264.encode_bgra(px, pw, ph, pkts) || pkts.empty()) return;
    }
    for (const auto& p : pkts) broadcast_h264_all(p);
}

static void tcp_broadcast_bgra_fallback(const uint8_t* bgra, int w, int h) {
    uint32_t body_size = 16 + (uint32_t)(w * h * 4);
    uint8_t hdr[PROTOCOL_FRAME_HEADER];
    protocol_build_header(hdr, body_size, PAYLOAD_TYPE_BGRA_FRAME);
    uint32_t frame_hdr[4] = {(uint32_t)w, (uint32_t)h, 4u, 0u};

    std::lock_guard<std::mutex> lk(g_tcp_mutex);
    for (auto it = g_tcp_clients.begin(); it != g_tcp_clients.end(); ) {
        TcpClient* c = *it;
        if (!c->alive ||
            !tcp_send_all(c->sock, (const char*)hdr, PROTOCOL_FRAME_HEADER) ||
            !tcp_send_all(c->sock, (const char*)frame_hdr, 16) ||
            !tcp_send_all(c->sock, (const char*)bgra, w * h * 4)) {
            c->alive = false;
            if (c->sock != INVALID_SOCKET) { closesocket(c->sock); c->sock = INVALID_SOCKET; }
            if (c->reader.joinable()) c->reader.detach();
            delete c;
            it = g_tcp_clients.erase(it);
        } else {
            ++it;
        }
    }
}

static std::string cmd_capture_stream_stop(); // fwd decl for cmd_capture_stream_start

// ── Self-test client (connects to test_target 127.0.0.1:19998, JSON-lines) ──
// Reads reports from test_target and forwards each to the frontend tagged
// type:"selftest". Log lines ({"type":"log",...}) are written into GAM's
// logger so the Monitor UI owns the log pipeline.
//
// Handshake: the first "hello" line is read SYNCHRONOUSLY in selftest_connect
// and returned in the command result (avoids racing PostWebMessage vs JS
// subscribe). Subsequent lines are queued and drained on the main STA thread
// via WM_SELFTEST_EVENT (WebView2 PostWebMessage is not reliable off-thread).
static SOCKET            g_st_sock = INVALID_SOCKET;
static std::thread       g_st_thread;
static std::atomic<bool> g_st_running{false};
static constexpr int     SELFTEST_DEFAULT_PORT = 19998;
static std::string       g_st_pending;          // bytes left after sync hello read
static std::mutex        g_st_q_mtx;
static std::vector<std::string> g_st_q;         // lines waiting for main-thread drain

static std::string st_extract_json_str(const std::string& obj, const char* key) {
    std::string pat = std::string("\"") + key + "\":\"";
    size_t p = obj.find(pat);
    if (p == std::string::npos) return {};
    p += pat.size();
    std::string out;
    for (size_t i = p; i < obj.size(); ++i) {
        char c = obj[i];
        if (c == '\\' && i + 1 < obj.size()) {
            char n = obj[++i];
            if (n == 'n') out.push_back('\n');
            else if (n == 'r') out.push_back('\r');
            else if (n == 't') out.push_back('\t');
            else out.push_back(n);
            continue;
        }
        if (c == '"') break;
        out.push_back(c);
    }
    return out;
}

static void st_enqueue_event(const std::string& jsonObj) {
    {
        std::lock_guard<std::mutex> lk(g_st_q_mtx);
        g_st_q.push_back(jsonObj);
    }
    HWND hwnd = (HWND)get_main_hwnd();
    if (hwnd) PostMessageW(hwnd, WM_SELFTEST_EVENT, 0, 0);
}

static void st_forward(const std::string& jsonObj) {
    // test_target logs → GAM logger (tag tt). LOG() notifies the UI itself.
    if (jsonObj.find("\"type\":\"log\"") != std::string::npos) {
        std::string msg = st_extract_json_str(jsonObj, "msg");
        std::string level = st_extract_json_str(jsonObj, "level");
        if (msg.empty()) msg = jsonObj;
        if (level == "ERROR")      LOG_ERROR("tt", "%s", msg.c_str());
        else if (level == "WARN")  LOG_WARN("tt", "%s", msg.c_str());
        else if (level == "DEBUG") LOG_DEBUG("tt", "%s", msg.c_str());
        else                       LOG("tt", "%s", msg.c_str());
        return;
    }
    st_enqueue_event(jsonObj);
}

void selftest_drain_to_webview() {
    std::vector<std::string> batch;
    {
        std::lock_guard<std::mutex> lk(g_st_q_mtx);
        batch.swap(g_st_q);
    }
    for (const auto& line : batch) {
        PostJsonToWebView("{\"type\":\"selftest\",\"data\":" + line + "}");
    }
}

// Peer UI queue — WS/LAN reader threads → STA → PostJsonToWebView
static std::mutex g_peer_ui_mtx;
static std::vector<std::string> g_peer_ui_q;

void peer_ui_enqueue(const std::string& json) {
    {
        std::lock_guard<std::mutex> lk(g_peer_ui_mtx);
        g_peer_ui_q.push_back(json);
    }
    HWND hwnd = (HWND)get_main_hwnd();
    if (hwnd) PostMessageW(hwnd, WM_PEER_UI_EVENT, 0, 0);
}

void peer_ui_drain_to_webview() {
    std::vector<std::string> batch;
    {
        std::lock_guard<std::mutex> lk(g_peer_ui_mtx);
        batch.swap(g_peer_ui_q);
    }
    for (const auto& json : batch) {
        PostJsonToWebView(json);
    }
}

static void st_cleanup() {
    g_st_running = false;
    if (g_st_sock != INVALID_SOCKET) { closesocket(g_st_sock); g_st_sock = INVALID_SOCKET; }
    if (g_st_thread.joinable()) g_st_thread.join();
    g_st_pending.clear();
    std::lock_guard<std::mutex> lk(g_st_q_mtx);
    g_st_q.clear();
}

// Read one JSON-line with timeout. Any extra bytes stay in g_st_pending for the reader.
static bool st_recv_line_timeout(SOCKET s, std::string& line, int timeout_ms) {
    DWORD tv = (DWORD)timeout_ms;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
    char tmp[1024];
    for (;;) {
        size_t nl = g_st_pending.find('\n');
        if (nl != std::string::npos) {
            line = g_st_pending.substr(0, nl);
            g_st_pending.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            DWORD infinite = 0;
            setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&infinite, sizeof(infinite));
            return !line.empty();
        }
        int n = recv(s, tmp, sizeof(tmp), 0);
        if (n <= 0) {
            DWORD infinite = 0;
            setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&infinite, sizeof(infinite));
            return false;
        }
        g_st_pending.append(tmp, n);
    }
}

static void st_reader_loop() {
    std::string buf = std::move(g_st_pending);
    g_st_pending.clear();
    char tmp[1024];
    // Flush any leftover lines from the sync hello read first.
    auto flush_lines = [&]() {
        size_t nl;
        while ((nl = buf.find('\n')) != std::string::npos) {
            std::string line = buf.substr(0, nl);
            buf.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) st_forward(line);
        }
    };
    flush_lines();
    while (g_st_running) {
        int n = recv(g_st_sock, tmp, sizeof(tmp), 0);
        if (n <= 0) break;
        buf.append(tmp, n);
        flush_lines();
    }
    g_st_running = false;
    st_forward(R"({"type":"disconnected"})");
    LOG("cmd", "selftest reader exited");
}

static std::string cmd_selftest_connect(int port) {
    st_cleanup();                       // idempotent — drop any stale connection
    if (port <= 0) port = SELFTEST_DEFAULT_PORT;
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

    // Block until test_target greets us — this is the reliable handshake.
    // (Pushing hello only via PostWebMessage raced the JS subscriber / STA.)
    std::string hello;
    if (!st_recv_line_timeout(s, hello, 2500) ||
        hello.find("\"type\":\"hello\"") == std::string::npos) {
        closesocket(s);
        LOG_WARN("cmd", "selftest: no hello from test_target (got '%s')",
                 hello.empty() ? "<empty>" : hello.substr(0, 80).c_str());
        return R"st({"ok":false,"error":"no hello from test_target (is it listening on :19998?)"})st";
    }

    g_st_sock = s;
    g_st_running = true;
    g_st_thread = std::thread(st_reader_loop);
    LOG("cmd", "selftest connected to 127.0.0.1:%d (hello %zub)", port, hello.size());
    // Return hello inline so the frontend never depends on the push path for geometry.
    return std::string("{\"ok\":true,\"hello\":") + hello + "}";
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
                MonitorFromWindow(nullptr, MONITOR_DEFAULTTOPRIMARY), kRemoteEncodeMaxWHw);
        } else {
            g_stream_handle = wgc_stream_start(h, kRemoteEncodeMaxWHw);
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
    // GPU encode before Map — no local SharedBuffer preview.
    wgc_stream_set_gpu_frame_callback(g_stream_handle, on_wgc_gpu_frame, nullptr);
    wgc_stream_set_cpu_readback(g_stream_handle, 0);

    g_control_hwnd.store(hwnd);
    g_allow_stream.store(true);
    g_streaming = true;
    g_stream_session.fetch_add(1);
    g_h264_need_key.store(true);

    // WGC worker owns capture+encode; this thread only waits for stop.
    g_stream_thread = std::thread([]() {
        while (g_streaming) Sleep(50);
        std::lock_guard<std::mutex> lk(g_h264_mtx);
        g_h264.shutdown();
    });

    LOG("cmd", "stream_start: hwnd=%llu method=%s transport=%s gpu_encode=1 (no local preview)",
        (unsigned long long)hwnd, method.c_str(), transport.c_str());
    return R"({"ok":true,"allow_stream":true})";
}

static std::string cmd_capture_stream_stop() {
    g_streaming = false;
    g_allow_stream.store(false);
    if (g_stream_handle) {
        wgc_stream_signal_stop(g_stream_handle);
        if (g_stream_thread.joinable()) g_stream_thread.join();
        wgc_stream_set_gpu_frame_callback(g_stream_handle, nullptr, nullptr);
        wgc_stream_stop(g_stream_handle);
        g_stream_handle = nullptr;
    }
    // Keep g_control_hwnd so accept_control can still target last window.
    {
        std::lock_guard<std::mutex> lk(g_h264_mtx);
        g_h264.shutdown();
    }
    LOG("cmd", "stream_stop allow_stream=0");
    return R"({"ok":true,"allow_stream":false})";
}

static std::string cmd_set_stream_gate(const std::string& args) {
    bool enabled = json_get_bool(args, "enabled");
    if (!enabled) {
        std::string r;
        if (g_streaming) r = cmd_capture_stream_stop();
        else {
            g_allow_stream.store(false);
            r = R"({"ok":true,"allow_stream":false})";
        }
        agent_push_status();
        return r;
    }
    if (!ws_client_connected() && !(peer_role() == PeerRole::Controlled && peer_media_ready())) {
        // Peer controlled may stream before media socket is up — allow if Controlled.
        if (peer_role() != PeerRole::Controlled) {
            return R"({"ok":false,"error":"not connected to controller_server or peer session"})";
        }
    }
    uint64_t hwnd = json_get_uint64(args, "hwnd");
    std::string method = json_get_str(args, "method");
    if (method.empty()) {
        std::lock_guard<std::mutex> lk(g_remote_cfg_mtx);
        method = g_remote_capture;
    }
    if (method.empty()) method = "wgc";
    std::string r;
    if (g_streaming) {
        g_allow_stream.store(true);
        r = R"({"ok":true,"allow_stream":true})";
    } else {
        r = cmd_capture_stream_start(hwnd, method, "h264");
    }
    agent_push_status();
    return r;
}

static std::string cmd_set_control_gate(const std::string& args) {
    bool enabled = json_get_bool(args, "enabled");
    g_accept_control.store(enabled);
    if (args.find("\"hwnd\"") != std::string::npos)
        g_control_hwnd.store(json_get_uint64(args, "hwnd"));
    LOG("cmd", "accept_control=%d hwnd=%llu", (int)enabled,
        (unsigned long long)g_control_hwnd.load());
    agent_push_status();
    return enabled ? R"({"ok":true,"accept_control":true})"
                   : R"({"ok":true,"accept_control":false})";
}

static std::string cmd_get_gates() {
    char buf[128];
    snprintf(buf, sizeof(buf),
             "{\"ok\":true,\"allow_stream\":%s,\"accept_control\":%s,\"hwnd\":%llu}",
             g_allow_stream.load() ? "true" : "false",
             g_accept_control.load() ? "true" : "false",
             (unsigned long long)g_control_hwnd.load());
    return buf;
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


// ── Target-screen feedback overlays (NOT on the Monitor canvas) ──
// Cursor circle / click ripple / drag rect are topmost layered popups on the
// real desktop at the mapped capture point. HTTRANSPARENT so they never steal
// hits. Class names are skipped by background input hit-testing.
static HWND g_cursor_hwnd = nullptr;
static HBITMAP g_cursor_bmp = nullptr;
static constexpr int CURSOR_SZ = 32;
static constexpr int CURSOR_HALF = 16;

static HWND g_ripple_hwnd = nullptr;
static HBITMAP g_ripple_bmp = nullptr;
static constexpr int RIPPLE_SZ = 56;
static constexpr int RIPPLE_HALF = 28;
static constexpr UINT_PTR TIMER_RIPPLE_HIDE = 1;

static HWND g_drag_hwnd = nullptr;
static HBITMAP g_drag_bmp = nullptr;
static int g_drag_bmp_w = 0, g_drag_bmp_h = 0;

static bool overlay_norm_to_screen(HWND h, double nx, double ny, int& sx, int& sy) {
    // Match input/WGC: window capture uses full window rect (incl. chrome).
    if (h) {
        RECT wr{};
        if (!GetWindowRect(h, &wr)) return false;
        sx = wr.left + (int)(nx * (double)(wr.right - wr.left));
        sy = wr.top + (int)(ny * (double)(wr.bottom - wr.top));
        return true;
    }
    sx = GetSystemMetrics(SM_XVIRTUALSCREEN) +
         (int)(nx * (double)GetSystemMetrics(SM_CXVIRTUALSCREEN));
    sy = GetSystemMetrics(SM_YVIRTUALSCREEN) +
         (int)(ny * (double)GetSystemMetrics(SM_CYVIRTUALSCREEN));
    return true;
}

static HWND create_overlay_popup(const wchar_t* className, WNDPROC proc, int w, int h) {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = className;
    wc.lpfnWndProc = proc;
    RegisterClassExW(&wc);
    return CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT,
        className, L"", WS_POPUP, 0, 0, w, h,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
}

static void present_layered(HWND hwnd, HBITMAP bmp, int dstX, int dstY, int w, int h) {
    if (!hwnd || !bmp || w <= 0 || h <= 0) return;
    HDC hdcScreen = GetDC(nullptr);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    HBITMAP oldBmp = (HBITMAP)SelectObject(hdcMem, bmp);
    POINT ptDst = { dstX, dstY };
    POINT ptSrc = { 0, 0 };
    SIZE sz = { w, h };
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(hwnd, hdcScreen, &ptDst, &sz, hdcMem, &ptSrc, 0, &bf, ULW_ALPHA);
    SelectObject(hdcMem, oldBmp);
    DeleteDC(hdcMem);
    ReleaseDC(nullptr, hdcScreen);
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
}

static LRESULT CALLBACK cursor_overlay_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCHITTEST) return HTTRANSPARENT;
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK ripple_overlay_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCHITTEST) return HTTRANSPARENT;
    if (msg == WM_TIMER && wp == TIMER_RIPPLE_HIDE) {
        KillTimer(hwnd, TIMER_RIPPLE_HIDE);
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK drag_overlay_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCHITTEST) return HTTRANSPARENT;
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static HBITMAP make_circle_bitmap(int SZ, int half, bool ringOnly, BYTE accentR, BYTE accentG, BYTE accentB) {
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = SZ;
    bi.bmiHeader.biHeight = -SZ;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    DWORD* pixels = nullptr;
    HDC hdcScreen = GetDC(nullptr);
    HBITMAP bmp = CreateDIBSection(hdcScreen, &bi, DIB_RGB_COLORS, (void**)&pixels, nullptr, 0);
    ReleaseDC(nullptr, hdcScreen);
    if (!pixels || !bmp) return nullptr;

    float cx = (float)half, cy = (float)half;
    float outerR = (float)half - 2.0f;
    float innerR = ringOnly ? (float)half - 6.0f : (float)(half - 4);
    float dotR = ringOnly ? 0.0f : (float)half * 0.38f;

    for (int y = 0; y < SZ; y++) {
        for (int x = 0; x < SZ; x++) {
            float dx = (float)x - cx + 0.5f;
            float dy = (float)y - cy + 0.5f;
            float dist = sqrtf(dx * dx + dy * dy);
            float outerAlpha = fmaxf(0.0f, fminf(1.0f, outerR - dist + 0.5f));
            float innerAlpha = fmaxf(0.0f, fminf(1.0f, dist - innerR + 0.5f));
            float ringAlpha = outerAlpha * innerAlpha;
            float dotAlpha = dotR > 0 ? fmaxf(0.0f, fminf(1.0f, dotR - dist + 0.5f)) : 0.0f;
            float alpha = fmaxf(ringAlpha, dotAlpha);
            BYTE a = (BYTE)(alpha * (ringOnly ? 200.0f : 220.0f));
            pixels[y * SZ + x] = ((DWORD)a << 24) |
                ((DWORD)(accentR * a / 255) << 16) |
                ((DWORD)(accentG * a / 255) << 8) |
                (DWORD)(accentB * a / 255);
        }
    }
    return bmp;
}

static void cursor_overlay_init() {
    if (g_cursor_hwnd) return;
    g_cursor_hwnd = create_overlay_popup(L"GAM_CursorOverlay", cursor_overlay_wndproc, CURSOR_SZ, CURSOR_SZ);
    // Accent blue #3B82F6
    g_cursor_bmp = make_circle_bitmap(CURSOR_SZ, CURSOR_HALF, false, 59, 130, 246);
    if (g_cursor_hwnd) ShowWindow(g_cursor_hwnd, SW_HIDE);
}

static void ripple_overlay_init() {
    if (g_ripple_hwnd) return;
    g_ripple_hwnd = create_overlay_popup(L"GAM_RippleOverlay", ripple_overlay_wndproc, RIPPLE_SZ, RIPPLE_SZ);
    // Amber/green flash for left; we'll tint via separate bitmaps if needed — default accent.
    g_ripple_bmp = make_circle_bitmap(RIPPLE_SZ, RIPPLE_HALF, true, 96, 210, 140);
    if (g_ripple_hwnd) ShowWindow(g_ripple_hwnd, SW_HIDE);
}

static void drag_overlay_init() {
    if (g_drag_hwnd) return;
    g_drag_hwnd = create_overlay_popup(L"GAM_DragOverlay", drag_overlay_wndproc, 8, 8);
    if (g_drag_hwnd) ShowWindow(g_drag_hwnd, SW_HIDE);
}

static void cursor_overlay_show(int screenX, int screenY) {
    cursor_overlay_init();
    if (!g_cursor_hwnd || !g_cursor_bmp) return;
    present_layered(g_cursor_hwnd, g_cursor_bmp,
                    screenX - CURSOR_HALF, screenY - CURSOR_HALF, CURSOR_SZ, CURSOR_SZ);
}

static void cursor_overlay_hide() {
    if (g_cursor_hwnd) ShowWindow(g_cursor_hwnd, SW_HIDE);
}

static void ripple_overlay_show(int screenX, int screenY, bool rightButton) {
    ripple_overlay_init();
    if (!g_ripple_hwnd) return;
    // Rebuild tint for right-click (red-ish) vs left (green-ish).
    if (g_ripple_bmp) { DeleteObject(g_ripple_bmp); g_ripple_bmp = nullptr; }
    g_ripple_bmp = rightButton
        ? make_circle_bitmap(RIPPLE_SZ, RIPPLE_HALF, true, 238, 120, 120)
        : make_circle_bitmap(RIPPLE_SZ, RIPPLE_HALF, true, 96, 210, 140);
    if (!g_ripple_bmp) return;
    present_layered(g_ripple_hwnd, g_ripple_bmp,
                    screenX - RIPPLE_HALF, screenY - RIPPLE_HALF, RIPPLE_SZ, RIPPLE_SZ);
    KillTimer(g_ripple_hwnd, TIMER_RIPPLE_HIDE);
    SetTimer(g_ripple_hwnd, TIMER_RIPPLE_HIDE, 420, nullptr);
}

static void ripple_overlay_hide() {
    if (g_ripple_hwnd) {
        KillTimer(g_ripple_hwnd, TIMER_RIPPLE_HIDE);
        ShowWindow(g_ripple_hwnd, SW_HIDE);
    }
}

static void drag_overlay_hide() {
    if (g_drag_hwnd) ShowWindow(g_drag_hwnd, SW_HIDE);
}

static void drag_overlay_show(int x0, int y0, int x1, int y1) {
    drag_overlay_init();
    if (!g_drag_hwnd) return;
    int left = (std::min)(x0, x1);
    int top = (std::min)(y0, y1);
    int w = (std::max)(2, abs(x1 - x0));
    int h = (std::max)(2, abs(y1 - y0));
    // Cap bitmap size for pathological drags across huge virtual desktops.
    if (w > 4096) w = 4096;
    if (h > 4096) h = 4096;

    if (g_drag_bmp && (g_drag_bmp_w != w || g_drag_bmp_h != h)) {
        DeleteObject(g_drag_bmp);
        g_drag_bmp = nullptr;
    }
    if (!g_drag_bmp) {
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        DWORD* pixels = nullptr;
        HDC hdcScreen = GetDC(nullptr);
        g_drag_bmp = CreateDIBSection(hdcScreen, &bi, DIB_RGB_COLORS, (void**)&pixels, nullptr, 0);
        ReleaseDC(nullptr, hdcScreen);
        if (!pixels || !g_drag_bmp) return;
        g_drag_bmp_w = w;
        g_drag_bmp_h = h;
        // Premultiplied accent fill + brighter 2px border.
        const BYTE br = 59, bg = 130, bb = 246;
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                bool edge = x < 2 || y < 2 || x >= w - 2 || y >= h - 2;
                BYTE a = edge ? 200 : 48;
                pixels[y * w + x] = ((DWORD)a << 24) |
                    ((DWORD)(br * a / 255) << 16) |
                    ((DWORD)(bg * a / 255) << 8) |
                    (DWORD)(bb * a / 255);
            }
        }
    }
    present_layered(g_drag_hwnd, g_drag_bmp, left, top, w, h);
}

static void target_overlays_hide_all() {
    cursor_overlay_hide();
    ripple_overlay_hide();
    drag_overlay_hide();
}

// ── Input forwarding (delegated to per-method libs) ──────────
#include "../../input/include/input_methods.h"
#include "../../input/include/input_common.h"

static std::string cmd_send_input(const std::string& args) {
    InputArgs a = parse_input_args(args);
    a.ignore_hwnd = (uint64_t)(uintptr_t)get_main_hwnd();

    // Desktop PostMessage performs explicit point hit-testing and child-window
    // routing. WinAPI still requires a concrete target thread/window.
    if (a.hwnd == 0 && a.method == "winapi")
        return "{\"ok\":false,\"error\":\"desktop input does not support winapi method\"}";
    HWND hWnd = (HWND)(uintptr_t)a.hwnd;
    if (hWnd != nullptr && !IsWindow(hWnd))
        return "{\"ok\":false,\"error\":\"invalid window handle\"}";

    // Window targets: keep all pointer ops inside the window (model hallucination guard).
    if (input_type_uses_norm_coords(a.type)) {
        std::string err = input_validate_norm_bounds(hWnd, a.x_norm, a.y_norm);
        if (!err.empty())
            return std::string("{\"ok\":false,\"error\":\"") + err + "\"}";
        for (const auto& pt : a.dragPath) {
            err = input_validate_norm_bounds(hWnd, pt.first, pt.second);
            if (!err.empty())
                return std::string("{\"ok\":false,\"error\":\"") + err + "\"}";
        }
    }

    if (a.method == "sendinput")    return input_sendinput(hWnd, a);
    if (a.method == "winapi")       return input_winapi(hWnd, a);
    if (a.method == "postmessage")  return input_postmessage(hWnd, a);
    if (a.method == "sendmessage")  return input_postmessage(hWnd, a);
    if (a.method == "driver")       return input_driver(hWnd, a);

    return "{\"ok\":false,\"error\":\"unknown input method: " + a.method + "\"}";
}

// Apply target-driven policy: desktop=foreground SendInput, window=background SendMessage.
// Forces hwnd to g_control_hwnd so a hallucinating model cannot retarget another window.
static std::string execute_remote_control_json(const std::string& actionJson) {
    if (!g_accept_control.load()) {
        return "{\"ok\":false,\"error\":\"accept_control gate closed\"}";
    }
    uint64_t hwnd = g_control_hwnd.load();
    std::string input_mode;
    {
        std::lock_guard<std::mutex> lk(g_remote_cfg_mtx);
        input_mode = g_remote_input;
    }
    // seize → SendInput (foreground); postmsg → PostMessage (background).
    const char* method = (input_mode == "seize") ? "sendinput" : "postmessage";

    std::string body = actionJson;
    if (body.empty() || body[0] != '{')
        return "{\"ok\":false,\"error\":\"control message must be a JSON object\"}";

    char prefix[128];
    snprintf(prefix, sizeof(prefix),
             "{\"hwnd\":%llu,\"method\":\"%s\",",
             (unsigned long long)hwnd, method);
    std::string merged = std::string(prefix) + body.substr(1);
    return cmd_send_input(merged);
}

static void agent_push_status() {
    if (!ws_client_connected()) return;
    std::string capture, codec, input;
    {
        std::lock_guard<std::mutex> lk(g_remote_cfg_mtx);
        capture = g_remote_capture;
        codec = g_remote_codec;
        input = g_remote_input;
    }
    char buf[384];
    snprintf(buf, sizeof(buf),
             "{\"type\":\"status\",\"allow_stream\":%s,\"accept_control\":%s,"
             "\"capture\":\"%s\",\"codec\":\"%s\",\"input\":\"%s\",\"hwnd\":%llu}",
             g_allow_stream.load() ? "true" : "false",
             g_accept_control.load() ? "true" : "false",
             capture.c_str(), codec.c_str(), input.c_str(),
             (unsigned long long)g_control_hwnd.load());
    ws_client_send_text(buf);
}

static void on_server_text(const std::string& json) {
    if (json.find("\"type\":\"need_key\"") != std::string::npos ||
        json.find("\"type\": \"need_key\"") != std::string::npos) {
        g_h264_need_key.store(true);
        return;
    }
    if (json.find("\"type\":\"config\"") != std::string::npos ||
        json.find("\"type\": \"config\"") != std::string::npos) {
        std::string capture = json_get_str(json, "capture");
        std::string codec = json_get_str(json, "codec");
        std::string input = json_get_str(json, "input");
        {
            std::lock_guard<std::mutex> lk(g_remote_cfg_mtx);
            if (!capture.empty()) g_remote_capture = capture;
            if (!codec.empty()) g_remote_codec = codec;
            if (!input.empty()) g_remote_input = input;
        }
        LOG("cmd", "remote config capture=%s codec=%s input=%s",
            capture.empty() ? "-" : capture.c_str(),
            codec.empty() ? "-" : codec.c_str(),
            input.empty() ? "-" : input.c_str());
        // ACK with applied values
        char ack[256];
        {
            std::lock_guard<std::mutex> lk(g_remote_cfg_mtx);
            snprintf(ack, sizeof(ack),
                     "{\"type\":\"config_ack\",\"ok\":true,\"capture\":\"%s\","
                     "\"codec\":\"%s\",\"input\":\"%s\"}",
                     g_remote_capture.c_str(), g_remote_codec.c_str(),
                     g_remote_input.c_str());
        }
        ws_client_send_text(ack);
        agent_push_status();
        return;
    }
    // Control action from browser (via relay)
    std::string result = execute_remote_control_json(json);
    if (result.find("\"ok\":false") != std::string::npos)
        LOG_WARN("cmd", "control rejected: %s", result.c_str());
}

static std::string cmd_connect_server(const std::string& args) {
    std::string host = json_get_str(args, "host");
    int port = json_get_int(args, "port");
    if (host.empty()) host = "127.0.0.1";
    if (port <= 0 || port > 65535) port = 9997;
    ws_client_set_handlers(on_server_text, []() {
        LOG("cmd", "controller_server link closed");
        agent_push_status(); // no-op if disconnected
    });
    if (!ws_client_connect(host, (uint16_t)port))
        return "{\"ok\":false,\"error\":\"connect failed\"}";
    agent_push_status();
    char buf[160];
    snprintf(buf, sizeof(buf),
             "{\"ok\":true,\"connected\":true,\"host\":\"%s\",\"port\":%d}",
             host.c_str(), port);
    return buf;
}

static std::string cmd_disconnect_server() {
    ws_client_disconnect();
    return R"({"ok":true,"connected":false})";
}

static std::string cmd_get_server_status() {
    char buf[256];
    std::string capture, input;
    {
        std::lock_guard<std::mutex> lk(g_remote_cfg_mtx);
        capture = g_remote_capture;
        input = g_remote_input;
    }
    snprintf(buf, sizeof(buf),
             "{\"ok\":true,\"connected\":%s,\"allow_stream\":%s,\"accept_control\":%s,"
             "\"capture\":\"%s\",\"input\":\"%s\"}",
             ws_client_connected() ? "true" : "false",
             g_allow_stream.load() ? "true" : "false",
             g_accept_control.load() ? "true" : "false",
             capture.c_str(), input.c_str());
    return buf;
}

// Test-harness only: keep GAM above targets while preview-mapping so same-box
// z-order does not thrash. Production agent path never calls this.
static bool g_mapping_controller_on = false;

static void mapping_controller_apply(bool on) {
    HWND hwnd = (HWND)get_main_hwnd();
    if (!hwnd || !IsWindow(hwnd)) return;
    if (on) {
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        LockSetForegroundWindow(LSFW_LOCK);
        LOG("cmd", "mapping_controller: TOPMOST + LSFW_LOCK");
    } else {
        SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        LockSetForegroundWindow(LSFW_UNLOCK);
        LOG("cmd", "mapping_controller: restored (NOTOPMOST + LSFW_UNLOCK)");
    }
    g_mapping_controller_on = on;
}

static std::string cmd_set_mapping_controller(const std::string& args) {
    bool on = json_get_bool(args, "on");
    mapping_controller_apply(on);
    return on ? R"({"ok":true,"on":true})" : R"({"ok":true,"on":false})";
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

// Launch update apply for a finished download. Main thread (WndProc).
// Ownership: MimicClient installs/replaces bin\updater.exe from staging; then the
// (new) updater copies everything else. One UAC via cmd when a staged updater exists.
// Guard against duplicate WM_UPDATE_PROGRESS (two UAC prompts).
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
    std::string installDir = paths_get_install_dir();
    std::string installUpdater = installDir + "\\bin\\updater.exe";
    std::string stagedUpdater  = stagingDir + "\\bin\\updater.exe";
    DWORD pid = GetCurrentProcessId();
    const bool stageHasUpdater =
        GetFileAttributesA(stagedUpdater.c_str()) != INVALID_FILE_ATTRIBUTES;

    // Quote a path for cmd.exe when it contains spaces; updater parse_args strips quotes.
    auto cmd_quote = [](const std::string& p) -> std::string {
        if (p.find(' ') == std::string::npos && p.find('\t') == std::string::npos) return p;
        return "\"" + p + "\"";
    };

    SHELLEXECUTEINFOA sei = {};
    sei.cbSize = sizeof(sei);
    sei.fMask  = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = "runas";
    sei.nShow  = SW_HIDE;

    std::string file;
    std::string params;
    if (stageHasUpdater) {
        // Main installs new updater, then runs it (single elevation).
        file = "cmd.exe";
        params = "/c copy /Y " + cmd_quote(stagedUpdater) + " " + cmd_quote(installUpdater) +
                 " && " + cmd_quote(installUpdater) + " " + cmd_quote(stagingDir) + " " +
                 std::to_string((unsigned long)pid);
        LOG("cmd", "update_launch_updater: main will install staged updater then apply rest");
    } else {
        // Incremental with no updater change — run installed updater as-is.
        file = installUpdater;
        params = cmd_quote(stagingDir) + " " + std::to_string((unsigned long)pid);
        LOG("cmd", "update_launch_updater: no staged updater — apply with installed updater");
    }

    sei.lpFile       = file.c_str();
    sei.lpParameters = params.c_str();
    if (!ShellExecuteExA(&sei)) {
        LOG_ERROR("cmd", "update_launch_updater: ShellExecuteEx(runas) failed err=%lu",
            (unsigned long)GetLastError());
        return false;
    }
    if (sei.hProcess) CloseHandle(sei.hProcess);
    LOG("cmd", "update_launch_updater: elevated apply started, staging=%s", stagingDir.c_str());
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

// <shellapi.h> #defines ShellExecute → ShellExecuteA, which would rewrite the
// IShellDispatch2::ShellExecute method call below into a non-existent member.
// Drop the macro; explicit ShellExecuteA/ShellExecuteExA elsewhere are unaffected.
#ifdef ShellExecute
#undef ShellExecute
#endif

// Ask the ALREADY-RUNNING desktop shell (explorer.exe, always Medium IL) to
// ShellExecute for us. No Win32 primitive spawns a lower-IL child, so we reach
// explorer's automation object through the desktop ShellView's IShellDispatch2
// (Raymond Chen's technique) and its child inherits explorer's Medium IL.
// NOT CoCreateInstance(CLSID_Shell): an in-proc Shell object runs at OUR High
// IL (no de-elevation) and CLSID_Shell has no LOCAL_SERVER reg → 0x80040154.
static bool shell_execute_via_explorer(const std::wstring& file,
                                       const std::wstring& args,
                                       const std::wstring& verb) {
    bool ok = false;
    IShellWindows* psw = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL,
                                  IID_IShellWindows, (void**)&psw);
    if (FAILED(hr) || !psw) {
        LOG_ERROR("cmd", "explorer-launch: CoCreateInstance(ShellWindows) hr=0x%lx", (unsigned long)hr);
        return false;
    }

    VARIANT vEmpty; VariantInit(&vEmpty);
    VARIANT vLoc;   VariantInit(&vLoc); vLoc.vt = VT_I4; vLoc.lVal = CSIDL_DESKTOP;
    long lhwnd = 0;
    IDispatch* pdisp = nullptr;
    hr = psw->FindWindowSW(&vLoc, &vEmpty, SWC_DESKTOP, &lhwnd, SWFO_NEEDDISPATCH, &pdisp);
    if (FAILED(hr) || !pdisp) {
        LOG_ERROR("cmd", "explorer-launch: FindWindowSW hr=0x%lx", (unsigned long)hr);
        psw->Release();
        return false;
    }

    IServiceProvider* psp = nullptr;
    hr = pdisp->QueryInterface(IID_IServiceProvider, (void**)&psp);
    if (SUCCEEDED(hr) && psp) {
        IShellBrowser* psb = nullptr;
        hr = psp->QueryService(SID_STopLevelBrowser, IID_IShellBrowser, (void**)&psb);
        if (SUCCEEDED(hr) && psb) {
            IShellView* psv = nullptr;
            hr = psb->QueryActiveShellView(&psv);
            if (SUCCEEDED(hr) && psv) {
                IDispatch* pvdisp = nullptr;
                hr = psv->GetItemObject(SVGIO_BACKGROUND, IID_IDispatch, (void**)&pvdisp);
                if (SUCCEEDED(hr) && pvdisp) {
                    IShellFolderViewDual* pfvd = nullptr;
                    hr = pvdisp->QueryInterface(IID_IShellFolderViewDual, (void**)&pfvd);
                    if (SUCCEEDED(hr) && pfvd) {
                        IDispatch* pappdisp = nullptr;
                        hr = pfvd->get_Application(&pappdisp);
                        if (SUCCEEDED(hr) && pappdisp) {
                            IShellDispatch2* psd = nullptr;
                            hr = pappdisp->QueryInterface(IID_IShellDispatch2, (void**)&psd);
                            if (SUCCEEDED(hr) && psd) {
                                BSTR bFile = SysAllocString(file.c_str());
                                VARIANT vArgs; VariantInit(&vArgs);
                                if (!args.empty()) { vArgs.vt = VT_BSTR; vArgs.bstrVal = SysAllocString(args.c_str()); }
                                VARIANT vDir;  VariantInit(&vDir);
                                VARIANT vOp;   VariantInit(&vOp);   vOp.vt = VT_BSTR; vOp.bstrVal = SysAllocString(verb.c_str());
                                VARIANT vShow; VariantInit(&vShow); vShow.vt = VT_I4;  vShow.lVal = SW_SHOWNORMAL;
                                hr = psd->ShellExecute(bFile, vArgs, vDir, vOp, vShow);
                                ok = SUCCEEDED(hr);
                                if (!ok) LOG_ERROR("cmd", "explorer-launch: ShellExecute hr=0x%lx", (unsigned long)hr);
                                SysFreeString(bFile);
                                VariantClear(&vArgs); VariantClear(&vOp);
                                psd->Release();
                            } else LOG_ERROR("cmd", "explorer-launch: QI IShellDispatch2 hr=0x%lx", (unsigned long)hr);
                            pappdisp->Release();
                        } else LOG_ERROR("cmd", "explorer-launch: get_Application hr=0x%lx", (unsigned long)hr);
                        pfvd->Release();
                    } else LOG_ERROR("cmd", "explorer-launch: QI IShellFolderViewDual hr=0x%lx", (unsigned long)hr);
                    pvdisp->Release();
                } else LOG_ERROR("cmd", "explorer-launch: GetItemObject hr=0x%lx", (unsigned long)hr);
                psv->Release();
            } else LOG_ERROR("cmd", "explorer-launch: QueryActiveShellView hr=0x%lx", (unsigned long)hr);
            psb->Release();
        } else LOG_ERROR("cmd", "explorer-launch: QueryService(TopLevelBrowser) hr=0x%lx", (unsigned long)hr);
        psp->Release();
    } else LOG_ERROR("cmd", "explorer-launch: QI IServiceProvider hr=0x%lx", (unsigned long)hr);

    pdisp->Release();
    psw->Release();
    return ok;
}

// Relaunch a fresh copy at MEDIUM integrity (normal user) from our elevated
// process, by routing the launch through explorer (see above).
static bool relaunch_as_medium() {
    char exePath[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    int wlen = MultiByteToWideChar(CP_UTF8, 0, exePath, -1, nullptr, 0);
    if (wlen <= 0) return false;
    std::wstring wexe(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, exePath, -1, &wexe[0], wlen);

    // Balance CoUninitialize only on S_OK/S_FALSE (both bump the ref count);
    // RPC_E_CHANGED_MODE means COM was already up on this thread — don't unwind it.
    HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool ok = shell_execute_via_explorer(wexe, L"", L"open");
    if (SUCCEEDED(hrInit)) CoUninitialize();
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

static std::string cmd_set_setting(const std::string& key, const std::string& valueLiteral);
static const char* get_settings_path();
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
static const int KNOWN_SCHEMA = 3;

// Compile-time bootstrap URLs for "latest" manifest discovery (main tip).
// After a successful check, remote "sources" are persisted to settings and
// tried first on subsequent runs — so the server can rotate hosts without a
// client rebuild. These are factory defaults only.
static const char* BOOTSTRAP_MANIFEST_URLS[] = {
    "http://47.107.43.5/mimic/client/version.json",
    nullptr
};

// Extract quoted strings from a JSON string-array value for `key`.
static void parse_json_str_array(const std::string& m, const char* key,
                                 std::vector<std::string>& out) {
    std::string k = "\""; k += key; k += "\"";
    size_t p = m.find(k);
    if (p == std::string::npos) return;
    p = m.find('[', p + k.size());
    if (p == std::string::npos) return;
    int depth = 0;
    for (size_t i = p; i < m.size(); i++) {
        char c = m[i];
        if (c == '[') depth++;
        else if (c == ']') { depth--; if (depth == 0) break; }
        else if (depth == 1 && c == '"') {
            size_t e = i + 1;
            while (e < m.size() && !(m[e] == '"' && m[e - 1] != '\\')) e++;
            if (e >= m.size()) break;
            out.push_back(m.substr(i + 1, e - i - 1));
            i = e;
        }
    }
}

// Build the ordered candidate list: persisted sources first, then bootstrap
// (deduped). GAM_UPDATE_TAG override (if set) replaces the list with a single
// tag-pinned raw URL for isolated update-chain testing.
static void build_manifest_urls(std::vector<std::string>& urls) {
    char tagEnv[128] = {};
    DWORD tagEnvLen = GetEnvironmentVariableA("GAM_UPDATE_TAG", tagEnv, sizeof(tagEnv));
    if (tagEnvLen > 0 && tagEnvLen < sizeof(tagEnv)) {
        // Tag override still hits the live CDN shelf (git no longer hosts binaries).
        urls.push_back("http://47.107.43.5/mimic/client/version.json");
        LOG_WARN("cmd", "check_update: GAM_UPDATE_TAG=%s (CDN version.json)", tagEnv);
        return;
    }

    std::string settings = read_file(get_settings_path());
    std::vector<std::string> persisted;
    if (!settings.empty())
        parse_json_str_array(settings, "updateSources", persisted);
    for (size_t i = 0; i < persisted.size(); i++) {
        if (persisted[i].empty()) continue;
        bool dup = false;
        for (size_t j = 0; j < urls.size(); j++)
            if (urls[j] == persisted[i]) { dup = true; break; }
        if (!dup) urls.push_back(persisted[i]);
    }
    for (int i = 0; BOOTSTRAP_MANIFEST_URLS[i]; i++) {
        std::string u = BOOTSTRAP_MANIFEST_URLS[i];
        bool dup = false;
        for (size_t j = 0; j < urls.size(); j++)
            if (urls[j] == u) { dup = true; break; }
        if (!dup) urls.push_back(u);
    }
}

// Persist remote "sources" into settings so the next check prefers them.
static void persist_update_sources(const std::string& manifest) {
    std::vector<std::string> sources;
    parse_json_str_array(manifest, "sources", sources);
    if (sources.empty()) return;
    std::string arr = "[";
    for (size_t i = 0; i < sources.size(); i++) {
        if (i) arr += ",";
        arr += "\"" + json_escape(sources[i]) + "\"";
    }
    arr += "]";
    cmd_set_setting("updateSources", arr);
    LOG("cmd", "check_update: persisted %zu updateSources", sources.size());
}

// Try each candidate URL (with short retries) until a body containing "files"
// arrives. Returns empty on total failure; *usedUrl (optional) gets the winner.
static std::string fetch_remote_manifest(const std::vector<std::string>& urls,
                                         std::string* usedUrl) {
    for (size_t ui = 0; ui < urls.size(); ui++) {
        const std::string& url = urls[ui];
        for (int attempt = 1; attempt <= 3; attempt++) {
            std::string body = winhttp_get_str(url, "update");
            if (!body.empty() && body.find("\"files\"") != std::string::npos) {
                if (usedUrl) *usedUrl = url;
                LOG("cmd", "check_update: manifest ok from %s (attempt %d)",
                    url.c_str(), attempt);
                return body;
            }
            LOG_WARN("cmd", "check_update: manifest attempt %d empty/invalid (len=%zu) url=%s",
                attempt, body.size(), url.c_str());
            Sleep(500);
        }
    }
    return "";
}

// check_update — multi-source manifest fetch + per-file sha256 diff.
// forceFull (or remote "full_update":true) → include every file (full package).
// Discovery no longer depends on the Gitee releases API: any static host that
// serves a signed version.json works (bootstrap + persisted sources).
static std::string cmd_check_update(bool forceFull) {
    LOG("cmd", "check_update: discovering remote manifest...");
    std::vector<std::string> urls;
    build_manifest_urls(urls);
    if (urls.empty()) {
        LOG_ERROR("cmd", "check_update: no manifest URLs configured");
        return R"({"ok":false,"error":"no manifest URLs configured"})";
    }

    std::string usedUrl;
    std::string remoteManifest = fetch_remote_manifest(urls, &usedUrl);
    if (remoteManifest.empty() || remoteManifest.find("\"files\"") == std::string::npos) {
        LOG_ERROR("cmd", "check_update: manifest fetch FAILED after all sources");
        return R"({"ok":false,"error":"manifest fetch failed (network/CDN) - please retry"})";
    }

    // Latest version lives in the manifest itself (schema v2/v3 "app" field).
    std::string latest = json_val(remoteManifest, "app");
    if (!latest.empty() && (latest[0] == 'v' || latest[0] == 'V'))
        latest = latest.substr(1);
    std::string changelog = json_val(remoteManifest, "message");

    // ── Validate schema + signature BEFORE trusting app/version (铁律 5) ──
    std::string schemaStr = json_val(remoteManifest, "schema");
    int remoteSchema = schemaStr.empty() ? 1 : atoi(schemaStr.c_str());
    std::string current = APP_VERSION;
    std::string tag = latest.empty() ? "" : ("v" + latest);
    if (remoteSchema > KNOWN_SCHEMA) {
        LOG_ERROR("cmd", "check_update: manifest schema %d > known %d - client too old for incremental",
            remoteSchema, KNOWN_SCHEMA);
        std::string relUrl = "https://gitee.com/Andyqwe44/mimic/releases/tag/" + tag;
        return "{\"ok\":false,\"needs_full_installer\":true"
               ",\"error\":\"this update needs a newer installer - please download the full package\""
               ",\"current\":\"" + json_escape(current) + "\""
               ",\"latest\":\"" + json_escape(latest) + "\""
               ",\"download_url\":\"" + json_escape(relUrl) + "\"}";
    }
    if (update_manifest_is_signed(remoteManifest)) {
        if (!update_verify_manifest(remoteManifest)) {
            LOG_ERROR("cmd", "check_update: manifest signature INVALID - refusing update");
            return "{\"ok\":false,\"error\":\"manifest signature invalid - refusing update\""
                   ",\"current\":\"" + json_escape(current) + "\""
                   ",\"latest\":\"" + json_escape(latest) + "\"}";
        }
        LOG("cmd", "check_update: manifest signature OK");
    } else {
        LOG_WARN("cmd", "check_update: manifest unsigned - skipping signature check");
    }
    persist_update_sources(remoteManifest);

    std::string name = latest.empty() ? "" : ("v" + latest);
    bool hasUpdate = !latest.empty() && latest != current;
    std::string diffJson = "[]";
    bool useFull = forceFull;
    std::string message = changelog, downloadBase;
    bool mandatory = false;
    std::string jumpPad = json_val(remoteManifest, "jump_pad");

    if (hasUpdate) {
        downloadBase = json_val(remoteManifest, "download_base");
        message      = json_val(remoteManifest, "message");
        mandatory    = json_val(remoteManifest, "mandatory") == "true";
        std::string minVer = json_val(remoteManifest, "min_version");
        if (!minVer.empty() && version_lt(current, minVer)) {
            useFull = true;
            LOG_WARN("cmd", "check_update: current %s < min_version %s - forcing full update",
                current.c_str(), minVer.c_str());
        }

        std::string installDir = paths_get_install_dir();
        std::string localPath = paths_get_appdata_dir() + "\\version.json";
        std::string localManifest = read_file(localPath.c_str());
        if (localManifest.empty()) {
            localPath = installDir + "\\version.json";
            localManifest = read_file(localPath.c_str());
        }
        if (localManifest.empty())
            LOG_WARN("cmd", "check_update: local manifest missing (%s) - all files count as changed",
                localPath.c_str());

        if (json_val(remoteManifest, "full_update") == "true") useFull = true;

        {
            diffJson = "[";
            bool first = true;
            size_t filesPos = remoteManifest.find("\"files\"");
            if (filesPos != std::string::npos) {
                size_t pos = remoteManifest.find("{", filesPos);
                if (pos != std::string::npos) {
                    int depth = 0;
                    for (size_t i = pos; i < remoteManifest.size(); i++) {
                        if (remoteManifest[i] == '{') depth++;
                        else if (remoteManifest[i] == '}') { depth--; if (depth == 0) break; }
                        else if (depth == 1 && remoteManifest[i] == '"' && (i == pos+1 || remoteManifest[i-1] != '\\')) {
                            size_t keyEnd = remoteManifest.find("\"", i+1);
                            if (keyEnd == std::string::npos) break;
                            std::string filePath = remoteManifest.substr(i+1, keyEnd - i - 1);
                            std::string remoteSha = json_val(remoteManifest, "sha256", keyEnd);
                            std::string localSha  = json_val(localManifest, "sha256",
                                localManifest.find("\"" + filePath + "\""));
                            std::string remoteVer = json_val(remoteManifest, "v", keyEnd);
                            std::string sz = json_val(remoteManifest, "size", keyEnd);
                            bool changed = useFull || (!remoteSha.empty() && remoteSha != localSha);
                            if (changed) {
                                if (!first) diffJson += ","; first = false;
                                std::string dlUrl = !downloadBase.empty()
                                    ? downloadBase + filePath
                                    : (std::string("http://47.107.43.5/mimic/client/") + filePath);
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
    } else {
        // Even when up-to-date, refresh persisted sources from a good fetch so
        // URL rotations still propagate without requiring an update.
        persist_update_sources(remoteManifest);
    }

    size_t nDiff = (size_t)std::count(diffJson.begin(), diffJson.end(), '{');
    if (hasUpdate && nDiff == 0)
        LOG_WARN("cmd", "check_update: hasUpdate but 0 files differ - local already matches remote content");
    LOG("cmd", "check_update: current=%s latest=%s hasUpdate=%d full=%d diff_files=%zu source=%s",
        current.c_str(), latest.c_str(), (int)hasUpdate, (int)useFull, nDiff, usedUrl.c_str());

    std::string stagingStateJson;
    if (hasUpdate && nDiff > 0) {
        std::string stagingDir = paths_get_appdata_dir() + "\\staging";
        int doneFiles = 0;
        unsigned long long doneBytes = 0, totalBytes2 = 0;
        std::string donePaths = "[";
        size_t sp = 0;
        while ((sp = diffJson.find("\"path\"", sp)) != std::string::npos) {
            std::string fp = json_val(diffJson, "path", sp);
            std::string sha = json_val(diffJson, "sha256", sp);
            std::string sz = json_val(diffJson, "size", sp);
            unsigned long long fsize = sz.empty() ? 0 : _strtoui64(sz.c_str(), nullptr, 10);
            totalBytes2 += fsize;
            sp++;
            if (fp.empty() || sha.empty()) continue;
            std::string spath = stagingDir + "\\" + fp;
            std::string existing = sha256_hex_file(spath.c_str());
            if (existing == sha) {
                doneFiles++; doneBytes += fsize;
                if (doneFiles > 1) donePaths += ",";
                donePaths += "\"" + fp + "\"";
            }
        }
        donePaths += "]";
        if (doneFiles > 0) {
            char buf[384];
            snprintf(buf, sizeof(buf),
                ",\"staging_state\":{\"has_partial\":true,\"done_files\":%d"
                ",\"total_files\":%zu,\"done_bytes\":%llu,\"total_bytes\":%llu"
                ",\"done_paths\":%s}",
                doneFiles, nDiff, doneBytes, totalBytes2, donePaths.c_str());
            stagingStateJson = buf;
            LOG("cmd", "check_update: staging_state has_partial %d/%zu files", doneFiles, nDiff);
        }
    }

    return "{\"ok\":true"
        ",\"platform\":\"windows\""
        ",\"current\":\"" + json_escape(current) + "\""
        ",\"latest\":\"" + json_escape(latest) + "\""
        ",\"name\":\"" + json_escape(name.empty() ? tag : name) + "\""
        ",\"body\":\"" + json_escape(changelog) + "\""
        ",\"has_update\":" + (hasUpdate ? "true" : "false")
        + ",\"mode\":\"" + (useFull ? "full" : "incremental") + "\""
        + ",\"mandatory\":" + (mandatory ? "true" : "false")
        + ",\"message\":\"" + json_escape(message) + "\""
        + ",\"jump_pad\":\"" + json_escape(jumpPad) + "\""
        + ",\"diff\":" + diffJson
        + stagingStateJson + "}";
}

// Background download thread: fetch each diff file to staging, verify sha256,
// update g_up + throttled-post WM_UPDATE_PROGRESS. On success sets `succeeded`
// so WndProc launches updater; on any failure sets `failed` and stops.
static void download_thread_func(std::string diffJsonStr, std::string stagingDir) {
    HWND hwnd = (HWND)get_main_hwnd();
    auto post = [&]() { if (hwnd) PostMessageW(hwnd, WM_UPDATE_PROGRESS, 0, 0); };

    unsigned long long baseBytes = 0;  // bytes fully written for prior files (incl. resumed)
    int index = 0;
    ULONGLONG lastPost = 0;
    bool ok = true;
    std::string firstUrl, firstPath;   // first file's url/path → derive download_base (P1a)

    // ── Pre-scan staging for already-complete files (resume) ──
    int skippedCount = 0;
    unsigned long long skippedBytes = 0;
    {
        size_t pp = 0;
        while ((pp = diffJsonStr.find("\"path\"", pp)) != std::string::npos) {
            std::string fp = json_val(diffJsonStr, "path", pp);
            std::string sha = json_val(diffJsonStr, "sha256", pp);
            pp++;
            if (fp.empty() || sha.empty()) continue;
            std::string outP = stagingDir + "\\" + fp;
            std::string existing = sha256_hex_file(outP.c_str());
            if (existing == sha) {
                WIN32_FILE_ATTRIBUTE_DATA attr;
                if (GetFileAttributesExA(outP.c_str(), GetFileExInfoStandard, &attr)) {
                    ULONGLONG fsize = ((ULONGLONG)attr.nFileSizeHigh << 32) | attr.nFileSizeLow;
                    baseBytes += fsize;
                    skippedBytes += fsize;
                }
                skippedCount++;
            }
        }
    }
    if (skippedCount > 0) {
        std::lock_guard<std::mutex> lk(g_up_mtx);
        g_up.done_bytes = baseBytes;
        g_up.skipped_files = skippedCount;
        g_up.skipped_bytes = skippedBytes;
        LOG("cmd", "download_update: resume pre-scan -> %d files already staged (%llu bytes)",
            skippedCount, skippedBytes);
    }
    post();

    size_t pos = 0;
    while ((pos = diffJsonStr.find("\"path\"", pos)) != std::string::npos) {
        std::string filePath = json_val(diffJsonStr, "path", pos);
        std::string wantSha  = json_val(diffJsonStr, "sha256", pos);
        std::string dlUrl    = json_val(diffJsonStr, "url", pos);
        pos++;
        if (filePath.empty() || dlUrl.empty()) continue;
        index++;
        if (firstUrl.empty()) { firstUrl = dlUrl; firstPath = filePath; }

        {
            std::lock_guard<std::mutex> lk(g_up_mtx);
            g_up.current_file = index;
            g_up.file_path = filePath;
            g_up.skipped_files = skippedCount;
            g_up.skipped_bytes = skippedBytes;
        }
        post();

        // ── Resume: skip if staging already has this file with matching sha256 ──
        std::string outPath = stagingDir + "\\" + filePath;
        if (!wantSha.empty()) {
            std::string existingHash = sha256_hex_file(outPath.c_str());
            if (existingHash == wantSha) {
                LOG("cmd", "download_update: resume skip %s (sha256 match)", filePath.c_str());
                WIN32_FILE_ATTRIBUTE_DATA attr;
                if (GetFileAttributesExA(outPath.c_str(), GetFileExInfoStandard, &attr)) {
                    ULONGLONG fsize = ((ULONGLONG)attr.nFileSizeHigh << 32) | attr.nFileSizeLow;
                    baseBytes += fsize;
                    skippedBytes += fsize;
                }
                skippedCount++;
                {
                    std::lock_guard<std::mutex> lk(g_up_mtx);
                    g_up.done_bytes = baseBytes;
                    g_up.skipped_files = skippedCount;
                    g_up.skipped_bytes = skippedBytes;
                }
                post();
                continue;
            } else if (!existingHash.empty()) {
                // File exists but sha256 doesn't match — delete corrupt partial
                LOG("cmd", "download_update: stale file %s (sha256 mismatch want=%s got=%s), re-downloading",
                    filePath.c_str(), wantSha.c_str(), existingHash.c_str());
                DeleteFileA(outPath.c_str());
            }
        }

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

        // Write to staging, creating parent dirs (outPath declared above in resume check).
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

    // P1a: stage the manifest (version.json) so the updater can refresh the install
    // manifest AND learn the desired file set (for deletion sync). Derive the download
    // base from the first file's URL (dlUrl == base + filePath). No sha check — it IS
    // the manifest. Failure is non-fatal (WARN): the update still applies additively.
    if (ok && index > 0 && !firstUrl.empty() && firstUrl.size() > firstPath.size()
        && firstUrl.compare(firstUrl.size() - firstPath.size(), firstPath.size(), firstPath) == 0) {
        std::string base = firstUrl.substr(0, firstUrl.size() - firstPath.size());
        std::string vj = winhttp_get_str(base + "version.json", "update");
        if (!vj.empty()) {
            std::string vjPath = stagingDir + "\\version.json";
            FILE* vf = fopen(vjPath.c_str(), "wb");
            if (vf) {
                fwrite(vj.data(), 1, vj.size(), vf);
                fclose(vf);
                LOG("cmd", "download_update: staged version.json (%zu bytes)", vj.size());
            } else {
                LOG_WARN("cmd", "download_update: cannot write staged version.json");
            }
        } else {
            LOG_WARN("cmd", "download_update: version.json fetch empty - install manifest stays stale");
        }
    }

    {
        std::lock_guard<std::mutex> lk(g_up_mtx);
        g_up.active = false;
        g_up.succeeded = ok && index > 0;
        g_up.skipped_files = skippedCount;
        g_up.skipped_bytes = skippedBytes;
        if (g_up.succeeded) g_up.done_bytes = g_up.total_bytes;
    }
    post();  // terminal push → WndProc sees done/error; on done it launches updater
    LOG("cmd", "download_update: thread finished ok=%d files=%d", (int)ok, index);
}

// clear_staging — remove all files from the staging directory.
// Called when user opts to "重新下载" instead of resuming a partial download.
static std::string cmd_clear_staging() {
    std::string stagingDir = paths_get_appdata_dir() + "\\staging";
    // Recursively delete everything in staging/
    auto remove_dir = [](const std::string& dir, auto& self) -> void {
        char searchPath[MAX_PATH];
        snprintf(searchPath, MAX_PATH, "%s\\*", dir.c_str());
        WIN32_FIND_DATAA fd;
        HANDLE hFind = FindFirstFileA(searchPath, &fd);
        if (hFind == INVALID_HANDLE_VALUE) { RemoveDirectoryA(dir.c_str()); return; }
        do {
            if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
            char full[MAX_PATH];
            snprintf(full, MAX_PATH, "%s\\%s", dir.c_str(), fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                self(full, self);
            } else {
                DeleteFileA(full);
            }
        } while (FindNextFileA(hFind, &fd));
        FindClose(hFind);
        RemoveDirectoryA(dir.c_str());
    };
    remove_dir(stagingDir, remove_dir);
    // Recreate empty staging dir
    CreateDirectoryA(stagingDir.c_str(), nullptr);
    LOG("cmd", "clear_staging: done");
    return R"({"ok":true})";
}

// download_update — spawn a background download thread and return immediately.
// Real-time progress + completion are delivered via WM_UPDATE_PROGRESS pushes.
// Auto-resumes: files already in staging with matching sha256 are skipped.
static std::string cmd_download_update(const std::string& diffJsonStr) {
    LOG("cmd", "download_update: diff=%s", diffJsonStr.c_str());

    {
        std::lock_guard<std::mutex> lk(g_up_mtx);
        if (g_up.active) return R"({"ok":false,"error":"already_downloading"})";
    }

    // Resolve + create staging dir.
    std::string stagingDir = paths_get_appdata_dir() + "\\staging";
    CreateDirectoryA(stagingDir.c_str(), nullptr);

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
// Lives under paths_get_appdata_dir()\config\settings.json
// AppData: %LOCALAPPDATA%\MimicClient (compile-time; no Dev split).
// Outside the install dir so app updates never wipe personalization.
static std::string g_settings_path;
static std::mutex g_settings_mtx;

static const char* get_settings_path() {
    if (!g_settings_path.empty()) return g_settings_path.c_str();
    // paths_get_appdata_dir() already ensure_dir's the config folder
    g_settings_path = paths_get_appdata_dir() + "\\config\\settings.json";
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

// Detect corrupt settings written by the old double-escape / concurrent-RMW path.
static void trim_json_inplace(std::string& json) {
    while (!json.empty() && (unsigned char)json.front() <= ' ') json.erase(json.begin());
    while (!json.empty() && (unsigned char)json.back() <= ' ') json.pop_back();
}

static bool is_valid_settings_json(const std::string& jsonIn) {
    std::string json = jsonIn;
    trim_json_inplace(json);
    if (json.size() < 2 || json.front() != '{' || json.back() != '}') return false;
    if (json.rfind("{},", 0) == 0) return false;           // classic corrupt prefix
    if (json.find("\":,") != std::string::npos) return false; // empty value e.g. "keepFiles":,
    if (json.find("\\\"") != std::string::npos) return false; // over-escaped quotes
    return true;
}

static std::string json_unescape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char n = s[++i];
            if (n == '"') out += '"';
            else if (n == '\\') out += '\\';
            else if (n == 'n') out += '\n';
            else if (n == 't') out += '\t';
            else if (n == 'r') out += '\r';
            else out += n;
        } else {
            out += s[i];
        }
    }
    return out;
}

// Atomic write: temp file + ReplaceFile / MoveFileEx
static bool write_settings_file(const std::string& json) {
    const char* path = get_settings_path();
    std::string pathStr(path);
    size_t lastSlash = pathStr.rfind('\\');
    if (lastSlash != std::string::npos)
        CreateDirectoryA(pathStr.substr(0, lastSlash).c_str(), nullptr);

    std::string tmp = pathStr + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    size_t n = fwrite(json.data(), 1, json.size(), f);
    fclose(f);
    if (n != json.size()) {
        DeleteFileA(tmp.c_str());
        return false;
    }
    if (!MoveFileExA(tmp.c_str(), path, MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileA(tmp.c_str());
        return false;
    }
    return true;
}

// Raw settings object for WebView2 boot injection (before first paint).
std::string settings_get_boot_json() {
    std::lock_guard<std::mutex> lk(g_settings_mtx);
    const char* path = get_settings_path();
    std::string json = read_file(path);
    trim_json_inplace(json);
    if (json.empty() || json.size() < 3 || !is_valid_settings_json(json)) return "{}";
    return json;
}

static std::string cmd_get_settings() {
    std::lock_guard<std::mutex> lk(g_settings_mtx);
    const char* path = get_settings_path();
    std::string json = read_file(path);
    trim_json_inplace(json);
    if (json.empty() || json.size() < 3) json = "{}";
    if (!is_valid_settings_json(json)) {
        LOG_WARN("cmd", "get_settings: corrupt file (%zub), resetting to {}", json.size());
        // Keep a .bak for forensics, then reset
        std::string bak = std::string(path) + ".bak";
        MoveFileExA(path, bak.c_str(), MOVEFILE_REPLACE_EXISTING);
        json = "{}";
        write_settings_file(json);
    }
    LOG("cmd", "get_settings: %s -> %zub", path, json.size());
    return "{\"ok\":true,\"settings\":" + json + "}";
}

// Bulk replace — preferred path. Frontend sends one object; no concurrent RMW.
static std::string cmd_set_settings(const std::string& settingsJson) {
    if (settingsJson.size() < 2 || settingsJson.front() != '{' || settingsJson.back() != '}')
        return R"({"ok":false,"error":"settings must be a JSON object"})";
    if (!is_valid_settings_json(settingsJson))
        return R"({"ok":false,"error":"invalid settings JSON"})";

    std::lock_guard<std::mutex> lk(g_settings_mtx);
    if (!write_settings_file(settingsJson))
        return R"({"ok":false,"error":"write failed"})";

    LOG("cmd", "set_settings: %zub", settingsJson.size());
    return R"({"ok":true})";
}

// Single-key update. `value` must be a JSON literal already
// (frontend passes JSON.stringify(x); json_get_str + unescape recovers it).
static std::string cmd_set_setting(const std::string& key, const std::string& valueLiteral) {
    if (key.empty()) return R"({"ok":false,"error":"key required"})";
    if (valueLiteral.empty()) return R"({"ok":false,"error":"value required"})";

    std::lock_guard<std::mutex> lk(g_settings_mtx);
    const char* path = get_settings_path();
    std::string json = read_file(path);
    if (json.empty() || json.size() < 3 || !is_valid_settings_json(json)) json = "{}";

    std::string inner = json;
    if (!inner.empty() && inner.front() == '{') inner = inner.substr(1);
    if (!inner.empty() && inner.back() == '}') inner.pop_back();

    while (!inner.empty() && (inner.front() == ' ' || inner.front() == '\n' || inner.front() == '\r'))
        inner = inner.substr(1);
    while (!inner.empty() && (inner.back() == ' ' || inner.back() == '\n' || inner.back() == '\r' || inner.back() == ','))
        inner.pop_back();

    std::string search = "\"" + key + "\":";
    size_t pos = inner.find(search);
    if (pos != std::string::npos) {
        size_t end = pos + search.length();
        int depth = 0;
        bool inStr = false;
        while (end < inner.size()) {
            char c = inner[end];
            if (inStr) {
                if (c == '"' && inner[end - 1] != '\\') inStr = false;
            } else {
                if (c == '"') inStr = true;
                else if (c == '{' || c == '[') depth++;
                else if (c == '}' || c == ']') { if (depth > 0) depth--; else break; }
                else if (c == ',' && depth == 0) { end++; break; }
            }
            end++;
        }
        inner.erase(pos, end - pos);
        while (!inner.empty() && (inner.back() == ',' || inner.back() == ' ')) inner.pop_back();
        while (!inner.empty() && (inner.front() == ',' || inner.front() == ' ')) inner = inner.substr(1);
    }

    if (!inner.empty()) inner += ",";
    // Insert JSON literal as-is (do NOT re-escape — that corrupted the file)
    inner += "\"" + key + "\":" + valueLiteral;
    json = "{" + inner + "}";

    if (!write_settings_file(json))
        return R"({"ok":false,"error":"write failed"})";

    LOG("cmd", "set_setting: %s=%s", key.c_str(), valueLiteral.c_str());
    return R"({"ok":true})";
}

// ── Main dispatch ─────────────────────────────────────────
std::string dispatch_command(const std::string& json) {
    std::string cmd = json_get_str(json, "cmd");
    int id = json_get_int(json, "id");
    std::string args = json_get_obj(json, "args");

    std::string result;
    if (cmd == "list_windows") result = cmd_list_windows();
    else if (cmd == "list_targets") {
        // v2 envelope around the same Windows target list (id/platform/kind enriched).
        std::string arr = cmd_list_windows();
        result = std::string("{\"ok\":true,\"peer_proto\":2,\"targets\":") + arr + "}";
    }
    else if (cmd == "list_processes") result = cmd_list_processes();
    else if (cmd == "capture_window") {
        result = cmd_capture_window(json_get_uint64(args, "hwnd"), json_get_str(args, "method"));
    }
    else if (cmd == "capture_stream_start") {
        result = cmd_capture_stream_start(json_get_uint64(args, "hwnd"),
            json_get_str(args, "method"), json_get_str(args, "transport"));
    }
    else if (cmd == "capture_stream_stop") result = cmd_capture_stream_stop();
    else if (cmd == "set_stream_gate") result = cmd_set_stream_gate(args);
    else if (cmd == "set_control_gate") result = cmd_set_control_gate(args);
    else if (cmd == "get_gates") result = cmd_get_gates();
    else if (cmd == "connect_server") result = cmd_connect_server(args);
    else if (cmd == "disconnect_server") result = cmd_disconnect_server();
    else if (cmd == "get_server_status") result = cmd_get_server_status();
    else if (cmd == "peer_register") {
        result = peer_register(json_get_str(args, "url"), json_get_str(args, "user"),
                               json_get_str(args, "password"));
    }
    else if (cmd == "peer_login") {
        result = peer_login(json_get_str(args, "url"), json_get_str(args, "user"),
                            json_get_str(args, "password"), json_get_str(args, "deviceName"));
    }
    else if (cmd == "peer_probe") {
        result = peer_probe(json_get_str(args, "url"));
    }
    else if (cmd == "peer_logout") { peer_logout(); result = R"({"ok":true})"; }
    else if (cmd == "peer_status") result = peer_status_json();
    else if (cmd == "peer_invite") result = peer_invite(json_get_str(args, "targetDeviceId"));
    else if (cmd == "peer_accept") result = peer_accept(json_get_str(args, "fromDeviceId"));
    else if (cmd == "peer_reject") result = peer_reject(json_get_str(args, "fromDeviceId"));
    else if (cmd == "peer_hangup") result = peer_hangup();
    else if (cmd == "peer_request_windows") result = peer_request_windows();
    else if (cmd == "peer_set_target") {
        std::string tid = json_get_str(args, "id");
        if (tid.empty()) tid = json_get_str(args, "target_id");
        result = peer_set_remote_target(json_get_uint64(args, "hwnd"),
                                        json_get_str(args, "title"), tid);
    }
    else if (cmd == "peer_send_control") {
        // args is the action object itself or {action:{...}}
        std::string action = json_get_obj(args, "action");
        if (action.empty() || action == "{}") action = args;
        result = peer_send_control(action);
    }
    else if (cmd == "peer_set_control_mode") result = peer_set_control_mode(json_get_str(args, "mode"));
    else if (cmd == "peer_request_keyframe") result = peer_request_keyframe();
    else if (cmd == "peer_get_frame") {
        std::vector<uint8_t> frame;
        std::string meta = peer_take_last_frame(frame);
        if (frame.size() < 16) result = meta;
        else {
            // Return base64 for WebCodecs in monitor_web (LAN frames).
            static const char* B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            std::string b64;
            b64.reserve(((frame.size() + 2) / 3) * 4);
            for (size_t i = 0; i < frame.size(); i += 3) {
                uint32_t n = ((uint32_t)frame[i]) << 16;
                if (i + 1 < frame.size()) n |= ((uint32_t)frame[i + 1]) << 8;
                if (i + 2 < frame.size()) n |= frame[i + 2];
                b64.push_back(B64[(n >> 18) & 63]);
                b64.push_back(B64[(n >> 12) & 63]);
                b64.push_back((i + 1 < frame.size()) ? B64[(n >> 6) & 63] : '=');
                b64.push_back((i + 2 < frame.size()) ? B64[n & 63] : '=');
            }
            uint32_t w=0,h=0,flags=0,ts=0;
            memcpy(&w, frame.data(), 4);
            memcpy(&h, frame.data() + 4, 4);
            memcpy(&flags, frame.data() + 8, 4);
            memcpy(&ts, frame.data() + 12, 4);
            char buf[128];
            snprintf(buf, sizeof(buf),
                     "{\"ok\":true,\"w\":%u,\"h\":%u,\"flags\":%u,\"ts\":%u,\"b64\":\"",
                     w, h, flags, ts);
            result = std::string(buf) + b64 + "\"}";
        }
    }
    else if (cmd == "read_logs") result = cmd_read_logs(json_get_int(args, "max_files"));
    else if (cmd == "read_log_file") result = cmd_read_log_file(json_get_str(args, "filename"));
    else if (cmd == "open_log_dir") result = cmd_open_log_dir();
    else if (cmd == "clear_log") result = cmd_clear_log();
    else if (cmd == "log_ui_event") {
        result = cmd_log_ui_event(json_get_str(args, "event"), json_get_str(args, "detail"));
    }
    else if (cmd == "crash_log") {
        // Shared UI crash handlers (Windows + Android) — never silent.
        LOG_ERROR("crash", "%s | %s",
                  json_get_str(args, "kind").c_str(),
                  json_get_str(args, "message").c_str());
        result = R"({"ok":true})";
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
    else if (cmd == "set_mapping_controller") {
        result = cmd_set_mapping_controller(args);
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
            char exePath[MAX_PATH];
            GetModuleFileNameA(nullptr, exePath, MAX_PATH);
            char* lastSlash = strrchr(exePath, '\\');
            if (lastSlash) *lastSlash = '\0';
            std::string exeDir = exePath;

            // Prefer staged copy next to mimic_client.exe, then repo checkout path.
            auto exists = [](const std::string& p) -> bool {
                DWORD a = GetFileAttributesA(p.c_str());
                return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
            };
            std::string candidates[] = {
                exeDir + "\\test_target\\test_target.exe",
                exeDir + "\\..\\..\\..\\test_target\\build\\test_target.exe",
                exeDir + "\\..\\..\\..\\test_target\\test_target.exe",
            };
            std::string path;
            for (const auto& c : candidates) {
                char full[MAX_PATH] = {};
                if (GetFullPathNameA(c.c_str(), MAX_PATH, full, nullptr) && exists(full)) {
                    path = full;
                    break;
                }
            }
            if (path.empty()) {
                result = R"tt({"ok":false,"error":"test_target.exe not found (build with -Module test_target)"})tt";
            } else {
                LOG("cmd", "launch_test_target: %s", path.c_str());
                // Working directory = exe folder so relative ui/ resolves if needed.
                std::string workDir = path;
                size_t slash = workDir.find_last_of("\\/");
                if (slash != std::string::npos) workDir.resize(slash);
                HINSTANCE h = ShellExecuteA(nullptr, "open", path.c_str(), nullptr,
                                           workDir.c_str(), SW_SHOW);
                if ((INT_PTR)h > 32) {
                    result = R"({"ok":true,"action":"launched"})";
                } else {
                    result = "{\"ok\":false,\"error\":\"failed to launch, code=" +
                             std::to_string((int)(INT_PTR)h) + "\"}";
                }
            }
        }
    }
    else if (cmd == "find_test_target") {
        HWND h = FindWindowW(L"GAMTestTarget", L"GAM Test Target");
        char b[64];
        snprintf(b, sizeof(b), "{\"hwnd\":%llu}", (unsigned long long)(uintptr_t)h);
        result = b;
    }
    else if (cmd == "get_agent_status") {
        // SSOT for BottomBar Agent indicator — TCP :9999 client count
        size_t n = 0;
        {
            std::lock_guard<std::mutex> lk(g_tcp_mutex);
            n = g_tcp_clients.size();
        }
        char b[80];
        snprintf(b, sizeof(b), "{\"connected\":%s,\"clients\":%zu}",
                 n > 0 ? "true" : "false", n);
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
        // Hover marker on the REAL capture target (not the Monitor canvas).
        int show = json_get_int(args, "show");
        if (!show) {
            cursor_overlay_hide();
            result = R"({"ok":true})";
        } else {
            HWND h = (HWND)(uintptr_t)json_get_uint64(args, "hwnd");
            double x_norm = json_get_double(args, "x_norm");
            double y_norm = json_get_double(args, "y_norm");
            int sx = 0, sy = 0;
            if (!overlay_norm_to_screen(h, x_norm, y_norm, sx, sy)) {
                result = R"({"ok":false,"error":"failed to map overlay coordinates"})";
            } else {
                cursor_overlay_show(sx, sy);
                result = R"({"ok":true})";
            }
        }
    }
    else if (cmd == "target_ripple") {
        // Click flash on the real target screen.
        HWND h = (HWND)(uintptr_t)json_get_uint64(args, "hwnd");
        double x_norm = json_get_double(args, "x_norm");
        double y_norm = json_get_double(args, "y_norm");
        std::string button = json_get_str(args, "button");
        int sx = 0, sy = 0;
        if (!overlay_norm_to_screen(h, x_norm, y_norm, sx, sy)) {
            result = R"({"ok":false,"error":"failed to map ripple coordinates"})";
        } else {
            ripple_overlay_show(sx, sy, button == "right");
            result = R"({"ok":true})";
        }
    }
    else if (cmd == "target_drag") {
        // Drag selection rectangle on the real target screen.
        int show = json_get_int(args, "show");
        if (!show) {
            drag_overlay_hide();
            result = R"({"ok":true})";
        } else {
            HWND h = (HWND)(uintptr_t)json_get_uint64(args, "hwnd");
            double x0 = json_get_double(args, "x0");
            double y0 = json_get_double(args, "y0");
            double x1 = json_get_double(args, "x1");
            double y1 = json_get_double(args, "y1");
            int sx0 = 0, sy0 = 0, sx1 = 0, sy1 = 0;
            if (!overlay_norm_to_screen(h, x0, y0, sx0, sy0) ||
                !overlay_norm_to_screen(h, x1, y1, sx1, sy1)) {
                result = R"({"ok":false,"error":"failed to map drag overlay coordinates"})";
            } else {
                drag_overlay_show(sx0, sy0, sx1, sy1);
                result = R"({"ok":true})";
            }
        }
    }
    else if (cmd == "target_overlays_hide") {
        target_overlays_hide_all();
        result = R"({"ok":true})";
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
    else if (cmd == "clear_staging") {
        result = cmd_clear_staging();
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
    else if (cmd == "set_settings") {
        result = cmd_set_settings(json_get_obj(args, "settings"));
    }
    else if (cmd == "set_setting") {
        // value is JSON.stringify(x) from TS → unescape to recover JSON literal
        result = cmd_set_setting(json_get_str(args, "key"),
                                 json_unescape(json_get_str(args, "value")));
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
    // Logs under AppData\log (Prod: MimicClient, Dev: MimicClient).
    // Writable for non-admin users; cleaned by uninstaller for Prod.
    std::string log_dir = paths_get_appdata_dir() + "\\log";
    capture_log_init("agent", APP_VERSION, log_dir.c_str(), 5, 5000);
    capture_log_set_notify(on_log_notify);  // C++ LOG() → push to TS in real-time
    capture_log_set_level(LOG_LEVEL_INFO);
    LOG("cmd", "perf: backend_init entered t+%llums (blocks msg loop until done)",
        GetTickCount64() - g_boot_tick);
    init_wic();
    tcp_server_start();
    // HTTP/WS controller lives in standalone controller_server.exe.
    // Agent connects outbound via connect_server hostCall.
    // Peer sessions: Mimic signaling + LAN media.
    peer_init(PeerCallbacks{
        // Must marshal to STA — off-thread PostWebMessage silently drops devices/invite.
        [](const std::string& json) { peer_ui_enqueue(json); },
        [](const std::string& actionJson) {
            std::string r = execute_remote_control_json(actionJson);
            if (r.find("\"ok\":false") != std::string::npos)
                LOG_WARN("peer", "control rejected: %s", r.c_str());
        },
        []() { g_h264_need_key.store(true); },
        []() -> std::string {
            // Return raw array from list_windows result
            std::string full = cmd_list_windows();
            // cmd_list_windows returns [{...},...] or wrapped — check
            if (!full.empty() && full[0] == '[') return full;
            size_t p = full.find('[');
            size_t e = full.rfind(']');
            if (p != std::string::npos && e != std::string::npos && e > p)
                return full.substr(p, e - p + 1);
            return "[]";
        },
        [](uint64_t hwnd, const std::string& target_id) -> std::string {
            // Resolve v2 id → hwnd (desktop/display → 0; hwnd:N → N).
            uint64_t effective = hwnd;
            if (!target_id.empty()) {
                if (target_id.rfind("desktop:", 0) == 0 || target_id.rfind("display:", 0) == 0) {
                    effective = 0;
                } else if (target_id.rfind("hwnd:", 0) == 0) {
                    effective = _strtoui64(target_id.c_str() + 5, nullptr, 10);
                }
            }
            LOG("peer", "set_target id=%s hwnd=%llu effective=%llu", target_id.c_str(),
                (unsigned long long)hwnd, (unsigned long long)effective);
            if (effective != 0 && !IsWindow((HWND)(uintptr_t)effective))
                return R"({"ok":false,"error":"hwnd not a window"})";
            // Must appear in current enumeration (security filter). Desktop hwnd=0 always allowed.
            if (effective != 0) {
                std::string list = cmd_list_windows();
                char needle[64];
                snprintf(needle, sizeof(needle), "\"hwnd\":%llu", (unsigned long long)effective);
                char needle2[64];
                snprintf(needle2, sizeof(needle2), "\"hwnd\": %llu", (unsigned long long)effective);
                if (list.find(needle) == std::string::npos &&
                    list.find(needle2) == std::string::npos)
                    return R"({"ok":false,"error":"hwnd not in allowed window list"})";
            }
            g_control_hwnd.store(effective);
            g_accept_control.store(true);
            g_allow_stream.store(true);
            // Restart stream on target change so controller sees the new surface.
            if (g_streaming.load())
                cmd_capture_stream_stop();
            std::string method = "wgc"; // hwnd=0 → monitor mode inside stream_start
            {
                std::lock_guard<std::mutex> lk(g_remote_cfg_mtx);
                if (!g_remote_capture.empty()) method = g_remote_capture;
            }
            cmd_capture_stream_start(effective, method, "h264");
            char buf[320];
            if (!target_id.empty()) {
                snprintf(buf, sizeof(buf),
                         "{\"ok\":true,\"hwnd\":%llu,\"id\":\"%s\",\"peer_proto\":2}",
                         (unsigned long long)effective, target_id.c_str());
            } else {
                snprintf(buf, sizeof(buf), "{\"ok\":true,\"hwnd\":%llu,\"peer_proto\":2}",
                         (unsigned long long)effective);
            }
            return buf;
        },
    });

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
    if (g_mapping_controller_on) mapping_controller_apply(false);
    LOG("cmd", "backend shutdown");
    g_mta_running = false;
    if (g_mta_thread.joinable()) g_mta_thread.join();
    st_cleanup();          // drop self-test client link
    peer_shutdown();
    ws_client_disconnect();
    tcp_server_stop();
    capture_log_flush();
    capture_log_shutdown();
    g_wic = nullptr;
}
