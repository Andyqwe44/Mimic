/**
 * commands.cpp — Backend command dispatch (replaces Rust main.rs commands).
 *
 * WebMessage JSON → dispatch_command → FFI/lib calls → JSON response.
 */
#define NOMINMAX
#include "commands.h"
#include "json_helper.h"
#include "../../logger/logger.h"
#include "../../capture/include/capture_methods.h"
#include "../../capture/include/capture_wgc_ffi.h"
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

static BOOL CALLBACK enum_callback(HWND hwnd, LPARAM lparam) {
    auto* list = reinterpret_cast<std::vector<WindowInfo>*>(lparam);
    if (!IsWindowVisible(hwnd)) return TRUE;

    LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (!(style & WS_CAPTION)) return TRUE;
    if (ex & WS_EX_TOOLWINDOW) return TRUE;

    // Not cloaked
    BOOL cloaked = FALSE;
    DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    if (cloaked) return TRUE;

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

    list->push_back({title, "window", (uint64_t)(uintptr_t)hwnd});
    return TRUE;
}

static std::string cmd_list_windows() {
    std::vector<WindowInfo> list;
    list.push_back({" Entire Desktop", "desktop", 0});
    EnumWindows(enum_callback, (LPARAM)&list);
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

    char buf[512];
    snprintf(buf, sizeof(buf),
        R"({"image":"%s","w":%d,"h":%d,"x":%d,"y":%d,"screen_w":%d,"screen_h":%d,"method":"%s"})",
        b64.c_str(), r.w, r.h, x, y, sw, sh, r.method.c_str());
    return buf;
}

static std::string cmd_capture_window(uint64_t hwnd, const std::string& method) {
    auto r = call_capture(hwnd, method.empty() ? "auto" : method);
    if (r.w <= 0 || r.h <= 0) {
        LOG("cmd", "capture_window: FAILED hwnd=%llu", (unsigned long long)hwnd);
        return "";
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

static std::string cmd_capture_stream_start(uint64_t hwnd, const std::string& method, const std::string& transport) {
    HWND h = (HWND)(uintptr_t)hwnd;
    g_stream_handle = wgc_stream_start(h, 1280);
    if (!g_stream_handle) {
        LOG("cmd", "stream_start: FAILED");
        return R"({"ok":false,"error":"wgc_stream_start failed"})";
    }
    g_streaming = true;

    g_stream_thread = std::thread([transport]() {
        std::vector<uint8_t> buf(MAX_PX);
        while (g_streaming) {
            int w, h, ch;
            int size = wgc_stream_read(g_stream_handle, buf.data(), MAX_PX, &w, &h, &ch);
            if (size > 0) {
                // Frame captured — push to transport (SharedBuffer/MJPEG is handled externally for now)
                // Future: call shared_buffer_push() or mjpeg_server_push_frame() here
            }
            Sleep(1);
        }
    });

    LOG("cmd", "stream_start: hwnd=%llu method=%s transport=%s", (unsigned long long)hwnd, method.c_str(), transport.c_str());
    return R"({"ok":true})";
}

static std::string cmd_capture_stream_stop() {
    g_streaming = false;
    if (g_stream_handle) {
        wgc_stream_signal_stop(g_stream_handle);
        if (g_stream_thread.joinable()) g_stream_thread.join();
        g_stream_handle = nullptr;
    }
    LOG("cmd", "stream_stop");
    return R"({"ok":true})";
}

// ── Log commands ──────────────────────────────────────────
static std::string cmd_read_logs(int max_files) {
    char* mem = capture_log_read_memory();
    std::string live = mem ? mem : "";
    capture_log_free(mem);

    char* fjson = capture_log_list_files(max_files);
    std::string files = fjson ? fjson : "[]";
    capture_log_free(fjson);

    char buf[2048];
    snprintf(buf, sizeof(buf), R"({"live":"%s","files":%s})", json_escape(live).c_str(), files.c_str());
    return buf;
}

static std::string cmd_clear_log() {
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

// ── Main dispatch ─────────────────────────────────────────
std::string dispatch_command(const std::string& json) {
    std::string cmd = json_get_str(json, "cmd");
    int id = json_get_int(json, "id");
    std::string args = json_get_obj(json, "args");

    std::string result;
    if (cmd == "list_windows") result = cmd_list_windows();
    else if (cmd == "list_processes") result = cmd_list_processes();
    else if (cmd == "capture_window") {
        result = cmd_capture_window(json_get_int(args, "hwnd"), json_get_str(args, "method"));
    }
    else if (cmd == "capture_stream_start") {
        result = cmd_capture_stream_start(json_get_int(args, "hwnd"),
            json_get_str(args, "method"), json_get_str(args, "transport"));
    }
    else if (cmd == "capture_stream_stop") result = cmd_capture_stream_stop();
    else if (cmd == "read_logs") result = cmd_read_logs(json_get_int(args, "max_files"));
    else if (cmd == "clear_log") result = cmd_clear_log();
    else if (cmd == "log_ui_event") {
        result = cmd_log_ui_event(json_get_str(args, "event"), json_get_str(args, "detail"));
    }
    else if (cmd == "benchmark_methods") {
        result = cmd_benchmark_methods(json_get_int(args, "hwnd"), json_get_str(args, "method"));
    }
    else if (cmd == "debug_dump_frames") {
        result = cmd_debug_dump_frames(json_get_int(args, "enable") != 0);
    }

    if (id <= 0) return result; // fire-and-forget (no id field)
    if (result.empty()) return "{\"id\":" + std::to_string(id) + ",\"error\":\"unknown command\"}";
    return "{\"id\":" + std::to_string(id) + ",\"result\":" + result + "}";
}

// ── Init / Shutdown ───────────────────────────────────────
void backend_init() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); // For WIC
    capture_log_init("agent", "0.2.0", "log/", 5, 5000);
    wgc_init_apartment();
    init_wic();
    LOG("cmd", "backend init OK");
}

void backend_shutdown() {
    if (g_streaming) {
        g_streaming = false;
        if (g_stream_handle) wgc_stream_signal_stop(g_stream_handle);
        if (g_stream_thread.joinable()) g_stream_thread.join();
    }
    capture_log_flush();
    capture_log_shutdown();
    wgc_deinit_apartment();
    g_wic = nullptr;
    LOG("cmd", "backend shutdown OK");
}
