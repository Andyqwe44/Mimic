/**
 * monitor_app — Pure C++ WebView2 host for Game Agent Monitor.
 *
 * Replaces Rust/Tauri. One process: Win32 window + WebView2 + capture + MJPEG server.
 *
 * Dev:  build_dev\monitor_app.exe    → navigates to http://localhost:1420 (Vite HMR)
 * Prod: build\monitor_app.exe        → navigates to https://gam.local/index.html (dist/ embedded in exe, served from memory)
 * Mode is set at build time via /DDEV_MODE preprocessor define — no runtime --dev flag.
 */
#include <windows.h>
#include <objbase.h>
#include <wrl/client.h>
#include <string>
#include <functional>
#include <cstdio>
#include "paths.h"
#include <mutex>
#include <atomic>
#include <vector>

#include "../dep/WebView2.h"
#include "../../logger/logger.h"
#include "commands.h"

// Forward declare — avoid including virtual_desktop.h which pulls
// in COM GUIDs that conflict with WebView2.h static initializers.
void vd_set_main_hwnd(HWND hwnd);

using Microsoft::WRL::ComPtr;

// ── Forward declarations ────────────────────────────────────
void HandleWebMessage(const std::wstring& msg);
void PostJsonToWebView(const std::string& json);

// ── Lightweight COM callback wrappers (no WRL dependency) ──

template<typename Interface>
class ComCallbackBase : public Interface {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == __uuidof(Interface) || riid == __uuidof(IUnknown)) { *ppv = this; AddRef(); return S_OK; }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&ref_); }
    STDMETHODIMP_(ULONG) Release() override { ULONG r = InterlockedDecrement(&ref_); if (r == 0) delete this; return r; }
protected:
    ULONG ref_{1};
};

// Handler for CreateCoreWebView2EnvironmentWithOptions completion
struct EnvCreatedHandler : ComCallbackBase<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler> {
    using Func = std::function<HRESULT(HRESULT, ICoreWebView2Environment*)>;
    Func fn;
    explicit EnvCreatedHandler(Func f) : fn(std::move(f)) {}
    STDMETHODIMP Invoke(HRESULT result, ICoreWebView2Environment* env) override { return fn(result, env); }
};

// Handler for CreateCoreWebView2Controller completion
struct ControllerCreatedHandler : ComCallbackBase<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler> {
    using Func = std::function<HRESULT(HRESULT, ICoreWebView2Controller*)>;
    Func fn;
    explicit ControllerCreatedHandler(Func f) : fn(std::move(f)) {}
    STDMETHODIMP Invoke(HRESULT result, ICoreWebView2Controller* ctrl) override { return fn(result, ctrl); }
};

// Handler for WebMessageReceived events
struct WebMessageHandler : ComCallbackBase<ICoreWebView2WebMessageReceivedEventHandler> {
    STDMETHODIMP Invoke(ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) override {
        LPWSTR raw = nullptr;
        if (SUCCEEDED(args->TryGetWebMessageAsString(&raw)) && raw) {
            HandleWebMessage(raw);
            CoTaskMemFree(raw);
        }
        return S_OK;
    }
};

// ── Globals ────────────────────────────────────────────────
static HWND                  g_hwnd = nullptr;
static ComPtr<ICoreWebView2Controller> g_webviewController;
static ComPtr<ICoreWebView2> g_webview;
static ComPtr<ICoreWebView2Environment12> g_env12;
static ComPtr<ICoreWebView2_3>      g_webview3;
static ComPtr<ICoreWebView2_17> g_webview17;
#ifdef DEV_MODE
static constexpr bool g_dev_mode = true;
#else
static constexpr bool g_dev_mode = false;
#endif

// GIT (Global Interface Table) for cross-thread interface access.
// CoMarshalInterThreadInterfaceInStream fails (0x80040155) because
// WebView2 interfaces lack COM proxy/stub registration.
// GIT works with any COM interface — register once, retrieve from any thread.
static IGlobalInterfaceTable* g_git = nullptr;
static DWORD g_git_env12_cookie = 0;
static DWORD g_git_wv17_cookie = 0;

static constexpr int  DEFAULT_W  = 1280;
static constexpr int  DEFAULT_H  = 720;
// Dev/prod split: separate title, window class, and mutex so a dev build
// and a prod build can coexist on the same machine (each single-instance
// within its own kind). Dispatch ships prod only — this guards local debug.
#ifdef DEV_MODE
static constexpr PCWSTR TITLE        = L"Game Agent Monitor (Dev)";
static constexpr PCWSTR WINDOW_CLASS = L"GameAgentMonitor_Dev";
#else
static constexpr PCWSTR TITLE        = L"Game Agent Monitor";
static constexpr PCWSTR WINDOW_CLASS = L"GameAgentMonitor";
#endif
static constexpr int  DEV_PORT  = 1420;
static constexpr UINT WM_STREAM_FRAME = WM_USER + 100;

// Cross-thread frame bridge: stream thread (MTA) writes pixels here,
// posts WM_STREAM_FRAME to main window, main thread pushes SharedBuffer.
static std::mutex g_bridge_mutex;
static std::vector<uint8_t> g_bridge_buf;
static int g_bridge_w = 0, g_bridge_h = 0;
static std::atomic<bool> g_bridge_has_frame{false};

// ── Remaining fwd declarations (referenced before definition) ──
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
HRESULT InitWebView2(HWND hwnd);

#ifdef DEV_MODE
static constexpr PCWSTR SINGLE_INSTANCE_MUTEX = L"Global\\GameAgentMonitor_8A3F2D_Dev";
#else
static constexpr PCWSTR SINGLE_INSTANCE_MUTEX = L"Global\\GameAgentMonitor_8A3F2D";
#endif

// ── WinMain ─────────────────────────────────────────────────
int WINAPI WinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE, _In_ LPSTR lpCmdLine, _In_ int nCmdShow)
{
    // ── Single-instance guard ──
    // Named mutex prevents multiple instances of the same app.
    // If another instance is already running, activate its window and exit.
    // Runs BEFORE capture_log_init on purpose: this path must NOT touch the
    // logging system — a short-lived second process must not spawn a new log
    // session file (would pollute the front-end's history list). Signal via
    // exit code only: 2 = instance already running (raised existing window).
    HANDLE hMutex = CreateMutexW(NULL, TRUE, SINGLE_INSTANCE_MUTEX);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (hMutex) CloseHandle(hMutex);
        HWND hExisting = FindWindowW(WINDOW_CLASS, TITLE);
        if (hExisting) {
            if (IsIconic(hExisting)) ShowWindow(hExisting, SW_RESTORE);
            SetForegroundWindow(hExisting);
        }
        return 2;  // already running — caller inspects $? to distinguish from fresh launch
    }

    bool auto_stream = (std::string(lpCmdLine).find("--auto-stream") != std::string::npos);
    LOG("main", "GAM starting (dev=%d auto_stream=%d)", (int)g_dev_mode, (int)auto_stream);

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.hCursor       = LoadCursor(nullptr, MAKEINTRESOURCE(32512)); // IDC_ARROW
    wc.lpszClassName = WINDOW_CLASS;
    RegisterClassExW(&wc);

    g_hwnd = CreateWindowExW(
        0, WINDOW_CLASS, TITLE,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, DEFAULT_W, DEFAULT_H,
        nullptr, nullptr, hInstance, nullptr);

    if (!g_hwnd) return 1;

    vd_set_main_hwnd(g_hwnd);  // tell virtual_desktop which HWND to use for desktop detection

    ShowWindow(g_hwnd, nCmdShow);
    UpdateWindow(g_hwnd);

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    InitWebView2(g_hwnd);

    paths_init();
    backend_init();

    // Auto-start streaming for testing (bypasses GUI)
    if (auto_stream) {
        LOG("main", "auto-stream: starting desktop stream");
        std::string result = dispatch_command(
            R"({"cmd":"capture_stream_start","id":0,"args":{"hwnd":0,"method":"WGC","transport":"shared"}})");
        LOG("main", "auto-stream: %s", result.c_str());
    }

    MSG msg = {};
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    backend_shutdown();  // stops stream, flushes logger, shuts down TCP

    return (int)msg.wParam;
}

// ── Window procedure ────────────────────────────────────────
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_SIZE:
        if (g_webviewController) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            g_webviewController->put_Bounds(rc);
        }
        break;
    case WM_STREAM_FRAME:
        // Stream thread posted a new frame — push via SharedBuffer on main STA thread
        if (g_bridge_has_frame.load(std::memory_order_acquire)) {
            std::lock_guard<std::mutex> lk(g_bridge_mutex);
            shared_buffer_push_frame(g_bridge_buf.data(), g_bridge_w, g_bridge_h);
            g_bridge_has_frame.store(false, std::memory_order_release);
        }
        break;
    case WM_DESTROY:
        g_webviewController = nullptr;
        g_webview = nullptr;
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    return 0;
}

// ── WebView2 initialization ─────────────────────────────────
HRESULT InitWebView2(HWND hwnd)
{
    return CreateCoreWebView2EnvironmentWithOptions(
        nullptr, nullptr, nullptr,
        new EnvCreatedHandler([hwnd](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
            if (FAILED(result)) return result;

            // Capture SharedBuffer interfaces
            env->QueryInterface(IID_PPV_ARGS(&g_env12));

            return env->CreateCoreWebView2Controller(hwnd,
                new ControllerCreatedHandler([hwnd](HRESULT result, ICoreWebView2Controller* ctrl) -> HRESULT {
                    if (FAILED(result)) return result;

                    g_webviewController = ctrl;
                    g_webviewController->get_CoreWebView2(&g_webview);

                    // Capture ICoreWebView2_17 for PostSharedBufferToScript
                    g_webview->QueryInterface(IID_PPV_ARGS(&g_webview3));
                    g_webview->QueryInterface(IID_PPV_ARGS(&g_webview17));

                    // Register WebMessage handler (replaces Tauri invoke)
                    g_webview->add_WebMessageReceived(new WebMessageHandler(), nullptr);

                    // Register interfaces in GIT for cross-thread access.
                    // CoMarshalInterThreadInterfaceInStream fails (0x80040155)
                    // because WebView2 lacks COM proxy/stub. GIT works always.
                    CoCreateInstance(CLSID_StdGlobalInterfaceTable, nullptr,
                        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_git));
                    if (g_git) {
                        if (g_env12) g_git->RegisterInterfaceInGlobal(
                            g_env12.Get(), __uuidof(ICoreWebView2Environment12), &g_git_env12_cookie);
                        if (g_webview17) g_git->RegisterInterfaceInGlobal(
                            g_webview17.Get(), __uuidof(ICoreWebView2_17), &g_git_wv17_cookie);
                        LOG("main", "GIT registered: env12=%lu wv17=%lu",
                            (unsigned long)g_git_env12_cookie, (unsigned long)g_git_wv17_cookie);
                    }

                    if (g_dev_mode) {
                        g_webview->Navigate(L"http://localhost:1420");
                    } else {
                        // Prod: map virtual host to frontend folder on disk.
                        // SetVirtualHostNameToFolderMapping lets gam.local/* serve from
                        // the install dir's frontend/ folder with proper CORS behavior.
                        std::string frontendDir = paths_get_install_dir() + "\\frontend\\";
                        int wlen = MultiByteToWideChar(CP_UTF8, 0, frontendDir.c_str(),
                            (int)frontendDir.size(), nullptr, 0);
                        std::wstring wdir(wlen, L'\0');
                        MultiByteToWideChar(CP_UTF8, 0, frontendDir.c_str(),
                            (int)frontendDir.size(), &wdir[0], wlen);

                        g_webview3->SetVirtualHostNameToFolderMapping(
                            L"gam.local", wdir.c_str(),
                            COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
                        g_webview->Navigate(L"https://gam.local/index.html");
                        LOG("main", "prod: frontend served from %s via gam.local",
                            frontendDir.c_str());
                    }

                    RECT rc;
                    GetClientRect(hwnd, &rc);
                    g_webviewController->put_Bounds(rc);

                    return S_OK;
                }));
        }));
}

// ── SharedBuffer helper (exposed to commands.cpp) ──────────
void shared_buffer_push_frame(const uint8_t* bgra, int w, int h,
                               ICoreWebView2Environment12* env12_opt,
                               ICoreWebView2_17* wv17_opt) {
    auto* env12 = env12_opt ? env12_opt : g_env12.Get();
    auto* wv17  = wv17_opt  ? wv17_opt  : g_webview17.Get();
    if (!env12 || !wv17) {
        LOG("main", "shared_buffer_push_frame: missing interfaces env12=%d webview17=%d",
            env12 ? 1 : 0, wv17 ? 1 : 0);
        return;
    }
    size_t size = (size_t)w * h * 4;
    LOG("main", "shared_buffer_push_frame: %dx%d size=%zu bgra=%p", w, h, size, (void*)bgra);
    ComPtr<ICoreWebView2SharedBuffer> buf;
    if (FAILED(env12->CreateSharedBuffer((UINT)size, &buf))) {
        LOG("main", "shared_buffer_push_frame: CreateSharedBuffer FAILED size=%u", (UINT)size);
        return;
    }
    BYTE* dst = nullptr;
    if (FAILED(buf->get_Buffer(&dst)) || !dst) {
        LOG("main", "shared_buffer_push_frame: get_Buffer FAILED");
        return;
    }
    LOG("main", "shared_buffer_push_frame: dst=%p, converting BGRA→RGBA...", (void*)dst);

    // Convert BGRA → RGBA inline (ImageData expects RGBA).
    // DWORD per iteration: swap B/R bytes via bit ops — ~4× faster than byte access.
    int total = w * h;
    uint32_t* dwords = (uint32_t*)dst;
    const uint32_t* src = (const uint32_t*)bgra;
    for (int i = 0; i < total; i++) {
        uint32_t px = src[i];
        // BGRA=0xBBGGRRAA → RGBA=0xRRGGBBAA
        dwords[i] = (px & 0xFF00FF00) | ((px & 0x00FF0000) >> 16) | ((px & 0x000000FF) << 16);
    }
    LOG("main", "shared_buffer_push_frame: conversion done, posting...");

    // Pass dimensions as metadata so JS can construct ImageData
    wchar_t meta[64];
    swprintf(meta, 64, L"{\"w\":%d,\"h\":%d}", w, h);

    // CRITICAL: PostSharedBufferToScript BEFORE Close — per WebView2 docs,
    // the buffer must remain open when posted to script.
    wv17->PostSharedBufferToScript(buf.Get(), COREWEBVIEW2_SHARED_BUFFER_ACCESS_READ_ONLY, meta);
    LOG("main", "shared_buffer_push_frame: post done, closing buffer...");
    buf->Close();
    LOG("main", "shared_buffer_push_frame: %dx%d OK", w, h);
}

// ── Cross-thread SharedBuffer helpers ───────────────────────
// g_env12/g_webview17 are STA-created; stream thread (MTA) can't use them
// directly. GIT cookies allow retrieval from any COM apartment.

void shared_buffer_marshal_for_stream(DWORD* out_env_cookie, DWORD* out_wv_cookie) {
    *out_env_cookie = g_git_env12_cookie;
    *out_wv_cookie = g_git_wv17_cookie;
    LOG("main", "shared_buffer GIT cookies: env12=%lu wv17=%lu git=%d",
        (unsigned long)g_git_env12_cookie, (unsigned long)g_git_wv17_cookie,
        g_git ? 1 : 0);
}

IGlobalInterfaceTable* shared_buffer_get_git() { return g_git; }

// ── Stream thread → main thread frame bridge ──────────────
// Stream thread captures on MTA; SharedBuffer needs STA (main thread).
// Stream calls this → copies frame + PostMessage → WndProc pushes SharedBuffer.

void stream_bridge_push_frame(const uint8_t* bgra, int w, int h) {
    if (!g_hwnd) return;
    {
        std::lock_guard<std::mutex> lk(g_bridge_mutex);
        size_t sz = (size_t)w * h * 4;
        if (g_bridge_buf.size() < sz) g_bridge_buf.resize(sz);
        memcpy(g_bridge_buf.data(), bgra, sz);
        g_bridge_w = w;
        g_bridge_h = h;
    }
    g_bridge_has_frame.store(true, std::memory_order_release);
    PostMessageW(g_hwnd, WM_STREAM_FRAME, 0, 0);
}

// ── WebMessage bridge (replaces Tauri invoke) ───────────────
void HandleWebMessage(const std::wstring& msg)
{
    // Convert wstring to UTF-8 string
    int len = WideCharToMultiByte(CP_UTF8, 0, msg.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string json(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, msg.c_str(), -1, &json[0], len, nullptr, nullptr);

    // Extract id for response wrapping (frontend matches by id)
    int id = 0;
    auto id_pos = json.find("\"id\":");
    if (id_pos != std::string::npos) {
        // Parse the integer after "id":
        const char* p = json.c_str() + id_pos + 5;
        while (*p == ' ' || *p == ':') p++;
        id = atoi(p);
    }

    // Dispatch to commands module
    std::string result = dispatch_command(json);

    // Wrap response with id so frontend can match the pending promise
    std::string wrapped = "{\"id\":" + std::to_string(id) + ",\"result\":" + result + "}";
    PostJsonToWebView(wrapped);
}

void PostJsonToWebView(const std::string& json)
{
    if (g_webview) {
        int len = MultiByteToWideChar(CP_UTF8, 0, json.c_str(), -1, nullptr, 0);
        std::wstring w(len, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, json.c_str(), -1, &w[0], len);
        g_webview->PostWebMessageAsJson(w.c_str());
    }
}

// Accessor for commands.cpp — returns the main window HWND for self-rect queries.
void* get_main_hwnd() { return g_hwnd; }
