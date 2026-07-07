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
static bool                  g_dev_mode = false;

static constexpr int  DEFAULT_W  = 1280;
static constexpr int  DEFAULT_H  = 720;
static constexpr PCWSTR TITLE   = L"Game Agent Monitor";
static constexpr int  DEV_PORT  = 5173;
static constexpr int  PROD_PORT = 8888;

// ── Remaining fwd declarations (referenced before definition) ──
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
HRESULT InitWebView2(HWND hwnd);

// ── WinMain ─────────────────────────────────────────────────
int WINAPI WinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE, _In_ LPSTR lpCmdLine, _In_ int nCmdShow)
{
    g_dev_mode = (std::string(lpCmdLine).find("--dev") != std::string::npos);

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

    MSG msg = {};
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    mjpeg_server_stop();
    backend_shutdown();

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

            return env->CreateCoreWebView2Controller(hwnd,
                new ControllerCreatedHandler([hwnd](HRESULT result, ICoreWebView2Controller* ctrl) -> HRESULT {
                    if (FAILED(result)) return result;

                    g_webviewController = ctrl;
                    g_webviewController->get_CoreWebView2(&g_webview);

                    // Register WebMessage handler (replaces Tauri invoke)
                    g_webview->add_WebMessageReceived(new WebMessageHandler(), nullptr);

                    wchar_t url[128];
                    if (g_dev_mode) {
                        swprintf_s(url, L"http://localhost:%d", DEV_PORT);
                    } else {
                        swprintf_s(url, L"http://127.0.0.1:%d", PROD_PORT);
                    }
                    g_webview->Navigate(url);

                    RECT rc;
                    GetClientRect(hwnd, &rc);
                    g_webviewController->put_Bounds(rc);

                    return S_OK;
                }));
        }));
}

// ── WebMessage bridge (replaces Tauri invoke) ───────────────
void HandleWebMessage(const std::wstring& msg)
{
    // Convert wstring to UTF-8 string
    int len = WideCharToMultiByte(CP_UTF8, 0, msg.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string json(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, msg.c_str(), -1, &json[0], len, nullptr, nullptr);

    // Dispatch to commands module
    std::string result = dispatch_command(json);

    // Send response back to WebView
    if (!result.empty()) {
        PostJsonToWebView(result);
    }
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
