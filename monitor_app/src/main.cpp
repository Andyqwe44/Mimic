/**
 * monitor_app — Pure C++ WebView2 host for Game Agent Monitor.
 *
 * Replaces Rust/Tauri. One process: Win32 window + WebView2 + capture + MJPEG server.
 *
 * Dev:  monitor_app.exe --dev        → navigates to http://localhost:5173 (Vite HMR)
 * Prod: monitor_app.exe              → navigates to http://127.0.0.1:8888 (built-in server)
 */
#include <windows.h>
#include <objbase.h>
#include <wrl/client.h>
#include <string>
#include <functional>
#include <cstdio>

#include "../dep/WebView2.h"
#include "../../logger/logger.h"
#include "commands.h"
#include "mjpeg_server.h"

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
static bool                  g_dev_mode = false;

static constexpr int  DEFAULT_W  = 1280;
static constexpr int  DEFAULT_H  = 720;
static constexpr PCWSTR TITLE   = L"Game Agent Monitor";
static constexpr int  DEV_PORT  = 1420;

// ── Remaining fwd declarations (referenced before definition) ──
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
HRESULT InitWebView2(HWND hwnd);

// ── WinMain ─────────────────────────────────────────────────
int WINAPI WinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE, _In_ LPSTR lpCmdLine, _In_ int nCmdShow)
{
    bool auto_stream = (std::string(lpCmdLine).find("--auto-stream") != std::string::npos);
    g_dev_mode = (std::string(lpCmdLine).find("--dev") != std::string::npos);
    AllocConsole();
    freopen("CONOUT$", "w", stdout);
    freopen("CONOUT$", "w", stderr);
    LOG("main", "GAM starting (dev=%d auto_stream=%d)", (int)g_dev_mode, (int)auto_stream);

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.hCursor       = LoadCursor(nullptr, MAKEINTRESOURCE(32512)); // IDC_ARROW
    wc.lpszClassName = L"GameAgentMonitor";
    RegisterClassExW(&wc);

    g_hwnd = CreateWindowExW(
        0, L"GameAgentMonitor", TITLE,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, DEFAULT_W, DEFAULT_H,
        nullptr, nullptr, hInstance, nullptr);

    if (!g_hwnd) return 1;

    ShowWindow(g_hwnd, nCmdShow);
    UpdateWindow(g_hwnd);

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    InitWebView2(g_hwnd);

    backend_init();
    mjpeg_server_start();

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

    backend_shutdown();  // stops stream first, then MJPEG
    mjpeg_server_stop();

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

                    if (g_dev_mode) {
                        g_webview->Navigate(L"http://localhost:1420");
                    } else {
                        // Prod: map virtual host to dist/ folder → no HTTP server needed
                        if (g_webview3) {
                            wchar_t exe_dir[MAX_PATH * 2];
                            GetModuleFileNameW(nullptr, exe_dir, MAX_PATH);
                            wchar_t* last_slash = wcsrchr(exe_dir, L'\\');
                            if (last_slash) *last_slash = L'\0';
                            wcscat_s(exe_dir, L"\\..\\..\\monitor_web\\dist");
                            g_webview3->SetVirtualHostNameToFolderMapping(
                                L"gam.local", exe_dir,
                                COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
                            g_webview->Navigate(L"https://gam.local/index.html");
                        }
                    }

                    RECT rc;
                    GetClientRect(hwnd, &rc);
                    g_webviewController->put_Bounds(rc);

                    return S_OK;
                }));
        }));
}

// ── SharedBuffer helper (exposed to commands.cpp) ──────────
void shared_buffer_push_frame(const uint8_t* bgra, int w, int h) {
    if (!g_env12 || !g_webview17) return;
    size_t size = (size_t)w * h * 4;
    ComPtr<ICoreWebView2SharedBuffer> buf;
    if (FAILED(g_env12->CreateSharedBuffer((UINT)size, &buf))) return;
    BYTE* dst = nullptr;
    if (FAILED(buf->get_Buffer(&dst)) || !dst) return;

    // Convert BGRA → RGBA inline (ImageData expects RGBA)
    for (int i = 0; i < w * h; i++) {
        dst[i * 4 + 0] = bgra[i * 4 + 2];  // B → R
        dst[i * 4 + 1] = bgra[i * 4 + 1];  // G → G
        dst[i * 4 + 2] = bgra[i * 4 + 0];  // R → B
        dst[i * 4 + 3] = bgra[i * 4 + 3];  // A → A
    }

    // Pass dimensions as metadata so JS can construct ImageData
    wchar_t meta[64];
    swprintf(meta, 64, L"{\"w\":%d,\"h\":%d}", w, h);

    // CRITICAL: PostSharedBufferToScript BEFORE Close — per WebView2 docs,
    // the buffer must remain open when posted to script.
    g_webview17->PostSharedBufferToScript(buf.Get(), COREWEBVIEW2_SHARED_BUFFER_ACCESS_READ_ONLY, meta);
    buf->Close();
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
