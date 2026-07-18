/**
 * test_target.cpp — WebView2 input-test window for Game Agent Monitor.
 *
 * Acts as an ordinary desktop app: rich HTML UI (grid / buttons / scroll /
 * drag / input / context menu). Every interaction is reported to GAM over
 * TCP JSON-lines. Logs are also pushed as {"type":"log",...} so GAM owns
 * the log pipeline (no local console).
 *
 * Build: scripts\Build.ps1 -Module test_target
 * Listen: 127.0.0.1:19998
 */

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <objbase.h>
#include <wrl/client.h>
#include <string>
#include <thread>
#include <mutex>
#include <atomic>
#include <functional>
#include <cstring>
#include <cstdio>

#include "../mimic_client/dep/WebView2.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "version.lib")

using Microsoft::WRL::ComPtr;

// ── Geometry (must match ui/index.html + selftest.ts) ──
static constexpr int GRID = 5;
static constexpr int CELL = 60;
static constexpr int PAD = 10;
static constexpr int HIT_MARGIN = 16;
static constexpr int CLIENT_W = 640;
static constexpr int CLIENT_H = 720;
static constexpr int REPORT_PORT = 19998;

static constexpr wchar_t kClassName[] = L"GAMTestTarget";
static constexpr wchar_t kWindowTitle[] = L"GAM Test Target";

// ── Globals ──
static HWND g_hwnd = nullptr;
static ComPtr<ICoreWebView2Controller> g_controller;
static ComPtr<ICoreWebView2> g_webview;
static ComPtr<ICoreWebView2_3> g_webview3;

static SOCKET g_srv = INVALID_SOCKET;
static SOCKET g_cli = INVALID_SOCKET;
static std::thread g_srvThread;
static std::mutex g_cliMtx;
static std::atomic<bool> g_srvRun{false};
static unsigned g_seq = 0;

// ── COM callback base ──
template <typename Interface>
class ComCallbackBase : public Interface {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == __uuidof(Interface) || riid == __uuidof(IUnknown)) {
            *ppv = this;
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&ref_); }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG r = InterlockedDecrement(&ref_);
        if (r == 0) delete this;
        return r;
    }
protected:
    ULONG ref_{1};
};

struct EnvCreatedHandler : ComCallbackBase<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler> {
    using Func = std::function<HRESULT(HRESULT, ICoreWebView2Environment*)>;
    Func fn;
    explicit EnvCreatedHandler(Func f) : fn(std::move(f)) {}
    STDMETHODIMP Invoke(HRESULT result, ICoreWebView2Environment* env) override {
        return fn(result, env);
    }
};

struct ControllerCreatedHandler
    : ComCallbackBase<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler> {
    using Func = std::function<HRESULT(HRESULT, ICoreWebView2Controller*)>;
    Func fn;
    explicit ControllerCreatedHandler(Func f) : fn(std::move(f)) {}
    STDMETHODIMP Invoke(HRESULT result, ICoreWebView2Controller* ctrl) override {
        return fn(result, ctrl);
    }
};

struct WebMessageHandler : ComCallbackBase<ICoreWebView2WebMessageReceivedEventHandler> {
    STDMETHODIMP Invoke(ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) override;
};

// ── Path helpers ──
static std::wstring exe_dir_w() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    wchar_t* slash = wcsrchr(path, L'\\');
    if (slash) *slash = L'\0';
    return path;
}

static std::wstring join_path(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b;
    if (a.back() == L'\\' || a.back() == L'/') return a + b;
    return a + L'\\' + b;
}

static bool dir_exists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static std::wstring find_ui_dir() {
    std::wstring exe = exe_dir_w();
    // 1) next to exe: <exeDir>/ui
    std::wstring c1 = join_path(exe, L"ui");
    if (dir_exists(c1)) return c1;
    // 2) repo layout when launched from test_target/ or test_target/build/
    std::wstring c2 = join_path(exe, L"..\\ui");
    wchar_t full[MAX_PATH] = {};
    if (GetFullPathNameW(c2.c_str(), MAX_PATH, full, nullptr) && dir_exists(full))
        return full;
    std::wstring c3 = join_path(exe, L"..\\..\\test_target\\ui");
    if (GetFullPathNameW(c3.c_str(), MAX_PATH, full, nullptr) && dir_exists(full))
        return full;
    return c1;
}

// ── JSON helpers ──
static std::string json_escape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
        case '\\': o += "\\\\"; break;
        case '"':  o += "\\\""; break;
        case '\n': o += "\\n"; break;
        case '\r': o += "\\r"; break;
        case '\t': o += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                snprintf(buf, sizeof(buf), "\\u%04x", c);
                o += buf;
            } else {
                o.push_back((char)c);
            }
        }
    }
    return o;
}

static std::string wide_to_utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

// ── TCP report server ──
static void report_send_line(const std::string& line) {
    std::lock_guard<std::mutex> lk(g_cliMtx);
    if (g_cli == INVALID_SOCKET) return;
    std::string s = line;
    s.push_back('\n');
    if (send(g_cli, s.c_str(), (int)s.size(), 0) == SOCKET_ERROR) {
        closesocket(g_cli);
        g_cli = INVALID_SOCKET;
    }
}

static std::string hello_json() {
    // Content layout is fixed CSS (640×720). Report both the HTML/client space
    // (for grid math + JS event coords) and the outer window frame (for WGC /
    // send_input GetWindowRect normalization) so GAM can keep them consistent.
    RECT cr{};
    RECT wr{};
    if (g_hwnd) {
        GetClientRect(g_hwnd, &cr);
        GetWindowRect(g_hwnd, &wr);
    }
    int client_w = cr.right - cr.left;
    int client_h = cr.bottom - cr.top;
    if (client_w <= 0) client_w = CLIENT_W;
    if (client_h <= 0) client_h = CLIENT_H;
    int win_w = wr.right - wr.left;
    int win_h = wr.bottom - wr.top;
    if (win_w <= 0) win_w = client_w;
    if (win_h <= 0) win_h = client_h;
    POINT tl = {0, 0};
    if (g_hwnd) ClientToScreen(g_hwnd, &tl);
    int client_off_x = tl.x - wr.left;
    int client_off_y = tl.y - wr.top;

    char buf[1024];
    snprintf(buf, sizeof(buf),
        "{\"type\":\"hello\",\"version\":3,\"port\":%d,"
        "\"client_w\":%d,\"client_h\":%d,"
        "\"win_w\":%d,\"win_h\":%d,"
        "\"client_off_x\":%d,\"client_off_y\":%d,"
        "\"grid\":%d,\"cell\":%d,\"pad\":%d,\"hit_margin\":%d,"
        "\"regions\":{"
        "\"grid\":{\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d},"
        "\"buttons\":{\"x\":10,\"y\":320,\"w\":620,\"h\":36},"
        "\"scroll\":{\"x\":10,\"y\":366,\"w\":300,\"h\":160},"
        "\"drag\":{\"x\":320,\"y\":366,\"w\":310,\"h\":160},"
        "\"input\":{\"x\":10,\"y\":538,\"w\":620,\"h\":110}"
        "}}",
        REPORT_PORT, client_w, client_h, win_w, win_h, client_off_x, client_off_y,
        GRID, CELL, PAD, HIT_MARGIN,
        PAD, PAD, GRID * CELL, GRID * CELL);
    return buf;
}

static void tt_log(const char* level, const char* msg) {
    char buf[1024];
    snprintf(buf, sizeof(buf),
        "{\"type\":\"log\",\"level\":\"%s\",\"tag\":\"tt\",\"msg\":\"%s\"}",
        level, json_escape(msg).c_str());
    report_send_line(buf);
}

static void server_loop() {
    while (g_srvRun.load()) {
        SOCKET c = accept(g_srv, nullptr, nullptr);
        if (c == INVALID_SOCKET) {
            if (!g_srvRun.load()) break;
            Sleep(50);
            continue;
        }
        {
            std::lock_guard<std::mutex> lk(g_cliMtx);
            if (g_cli != INVALID_SOCKET) closesocket(g_cli);
            g_cli = c;
        }
        report_send_line(hello_json());
        tt_log("INFO", "TCP client connected");
    }
}

static bool server_start() {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
    g_srv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_srv == INVALID_SOCKET) return false;
    int reuse = 1;
    setsockopt(g_srv, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(REPORT_PORT);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(g_srv, (sockaddr*)&a, sizeof(a)) != 0) {
        closesocket(g_srv);
        g_srv = INVALID_SOCKET;
        return false;
    }
    listen(g_srv, 1);
    g_srvRun = true;
    g_srvThread = std::thread(server_loop);
    return true;
}

static void server_stop() {
    g_srvRun = false;
    if (g_srv != INVALID_SOCKET) {
        closesocket(g_srv);
        g_srv = INVALID_SOCKET;
    }
    {
        std::lock_guard<std::mutex> lk(g_cliMtx);
        if (g_cli != INVALID_SOCKET) {
            closesocket(g_cli);
            g_cli = INVALID_SOCKET;
        }
    }
    if (g_srvThread.joinable()) g_srvThread.join();
    WSACleanup();
}

// Forward UI events from WebView → TCP (add monotonic seq if missing).
static void forward_ui_event(const std::string& jsonUtf8) {
    if (jsonUtf8.empty()) return;
    // UI already embeds seq for most events; host may wrap if needed.
    report_send_line(jsonUtf8);
    ++g_seq;
}

STDMETHODIMP WebMessageHandler::Invoke(
    ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) {
    LPWSTR raw = nullptr;
    // Prefer string messages from chrome.webview.postMessage(string)
    if (SUCCEEDED(args->TryGetWebMessageAsString(&raw)) && raw) {
        forward_ui_event(wide_to_utf8(raw));
        CoTaskMemFree(raw);
        return S_OK;
    }
    // Fallback: JSON object → stringify via get_WebMessageAsJson
    if (SUCCEEDED(args->get_WebMessageAsJson(&raw)) && raw) {
        forward_ui_event(wide_to_utf8(raw));
        CoTaskMemFree(raw);
    }
    return S_OK;
}

// ── WebView2 init ──
static void InitWebView2(HWND hwnd) {
    std::wstring udf = join_path(exe_dir_w(), L"WebView2UserData");
    CreateDirectoryW(udf.c_str(), nullptr);

    CreateCoreWebView2EnvironmentWithOptions(
        nullptr, udf.c_str(), nullptr,
        new EnvCreatedHandler([hwnd](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
            if (FAILED(result) || !env) {
                MessageBoxW(hwnd,
                    L"Failed to create WebView2 environment.\n"
                    L"Install Microsoft Edge WebView2 Runtime.",
                    kWindowTitle, MB_ICONERROR);
                return result;
            }
            return env->CreateCoreWebView2Controller(
                hwnd,
                new ControllerCreatedHandler([hwnd](HRESULT result, ICoreWebView2Controller* ctrl) -> HRESULT {
                    if (FAILED(result) || !ctrl) {
                        MessageBoxW(hwnd, L"Failed to create WebView2 controller.",
                                    kWindowTitle, MB_ICONERROR);
                        return result;
                    }
                    g_controller = ctrl;
                    g_controller->get_CoreWebView2(&g_webview);
                    g_webview->QueryInterface(IID_PPV_ARGS(&g_webview3));

                    ComPtr<ICoreWebView2Settings> settings;
                    if (SUCCEEDED(g_webview->get_Settings(&settings)) && settings) {
                        settings->put_AreDefaultContextMenusEnabled(FALSE);
                        settings->put_IsStatusBarEnabled(FALSE);
                        settings->put_AreDevToolsEnabled(TRUE);
                    }

                    g_webview->add_WebMessageReceived(new WebMessageHandler(), nullptr);

                    std::wstring uiDir = find_ui_dir();
                    if (g_webview3 && dir_exists(uiDir)) {
                        g_webview3->SetVirtualHostNameToFolderMapping(
                            L"tt.local", uiDir.c_str(),
                            COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
                        g_webview->Navigate(L"https://tt.local/index.html");
                    } else {
                        MessageBoxW(hwnd,
                            L"ui/ folder not found next to test_target.exe.\n"
                            L"Rebuild with scripts\\Build.ps1 -Module test_target",
                            kWindowTitle, MB_ICONERROR);
                    }

                    RECT rc{};
                    GetClientRect(hwnd, &rc);
                    g_controller->put_Bounds(rc);
                    g_controller->put_IsVisible(TRUE);

                    tt_log("INFO", "WebView2 controller ready");
                    return S_OK;
                }));
        }));
}

// ── Window proc ──
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_SIZE:
        if (g_controller) {
            RECT rc{};
            GetClientRect(hwnd, &rc);
            g_controller->put_Bounds(rc);
        }
        return 0;
    case WM_DESTROY:
        server_stop();
        g_controller = nullptr;
        g_webview = nullptr;
        g_webview3 = nullptr;
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

int WINAPI WinMain(_In_ HINSTANCE hInst, _In_opt_ HINSTANCE, _In_ LPSTR, _In_ int nCmdShow) {
    // Single instance
    HWND existing = FindWindowW(kClassName, kWindowTitle);
    if (existing) {
        ShowWindow(existing, SW_RESTORE);
        SetForegroundWindow(existing);
        return 0;
    }

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(RGB(40, 44, 52));
    wc.lpszClassName = kClassName;
    wc.style = CS_DBLCLKS;
    RegisterClassExW(&wc);

    // Fixed client size = HTML layout (640×720). No maximize/resize — keeps
    // capture, send_input window-rect mapping, and UI content in one geometry.
    const DWORD style = WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX;
    RECT wr = {0, 0, CLIENT_W, CLIENT_H};
    AdjustWindowRect(&wr, style, FALSE);

    g_hwnd = CreateWindowExW(
        0, kClassName, kWindowTitle, style,
        CW_USEDEFAULT, CW_USEDEFAULT,
        wr.right - wr.left, wr.bottom - wr.top,
        nullptr, nullptr, hInst, nullptr);
    if (!g_hwnd) return 1;

    if (!server_start()) {
        MessageBoxW(g_hwnd,
            L"TCP bind failed on 127.0.0.1:19998.\n"
            L"Another test_target instance may be running.",
            kWindowTitle, MB_ICONERROR);
        DestroyWindow(g_hwnd);
        return 2;
    }

    ShowWindow(g_hwnd, nCmdShow > 0 ? nCmdShow : SW_SHOW);
    UpdateWindow(g_hwnd);

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    InitWebView2(g_hwnd);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    CoUninitialize();
    return (int)m.wParam;
}
