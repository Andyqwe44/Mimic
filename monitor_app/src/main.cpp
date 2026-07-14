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
#include "json_helper.h"   // json_get_str
#include "version.h"       // APP_VERSION (heal_local_manifest)
#include "sha256_util.h"   // sha256_hex_file
#include <shellapi.h>      // ShellExecuteA

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

// perf: process boot time (ms via GetTickCount64), for startup latency
// instrumentation. Set first thing in WinMain; each startup milestone logs
// its offset so the white-screen breakdown (env vs backend) is measurable.
// Non-static: commands.cpp reads it via extern to time backend_init.
unsigned long long g_boot_tick = 0;

// Reveal the (initially hidden) main window; defined below. Declared here so the
// NavigationCompleted handler can trigger the reveal on a reliable C++ event —
// the frontend's requestAnimationFrame signal never fires while the window is
// hidden (the compositor is paused), which left the window stuck grey until the
// 8s safety timer.
void app_post_show_window();

// perf: log when the first navigation completes — this is ≈ the end of the
// white screen (content painted). Reuses the ComCallbackBase pattern.
struct NavCompletedHandler : ComCallbackBase<ICoreWebView2NavigationCompletedEventHandler> {
    STDMETHODIMP Invoke(ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*) override {
        LOG("main", "perf: NavigationCompleted t+%llums (~white-screen end)",
            GetTickCount64() - g_boot_tick);
        app_post_show_window();  // reliable reveal trigger (frontend rAF never fires while hidden)
        return S_OK;
    }
};

// ── Globals ────────────────────────────────────────────────
static HWND                  g_hwnd = nullptr;
static HANDLE                g_singleton_mutex = nullptr;  // single-instance lock; released on permission-switch relaunch
// Window starts hidden and is revealed only once the frontend paints its first
// frame (WM_SHOW_WINDOW) or the safety timer fires — hides the WebView2 startup
// gap so the window never appears as a white blank. Idempotent via this flag.
static bool                  g_window_shown = false;
static constexpr UINT_PTR    TIMER_SHOW_SAFETY = 1;   // fallback reveal if frontend never signals
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

// Read a whole file into a string (empty on failure).
static std::string read_file_str(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return "";
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    std::string s(n > 0 ? (size_t)n : 0, '\0');
    if (n > 0) fread(&s[0], 1, (size_t)n, f);
    fclose(f);
    return s;
}

// Minimal JSON string escaper (backslash + quote) for progress push messages.
static std::string js_escape(const std::string& s) {
    std::string o; o.reserve(s.size() + 8);
    for (char c : s) { if (c == '\\' || c == '"') o += '\\'; o += c; }
    return o;
}

// ── First-launch local-manifest self-heal ──────────────────────────────────
// The baseline manifest that check_update diffs against must match the ACTUAL
// installed files. The installed {app}\version.json goes stale (an old updater
// never refreshed it) and {app} (Program Files) isn't writable by this normal-
// user process — so the authoritative baseline lives in appdata and is rebuilt
// here at startup whenever it's missing or its "app" != this exe's version.
// Result: the diff is always correct (no re-downloading unchanged files) without
// depending on the updater to refresh anything.
static void manifest_scan_dir(const std::string& base, const std::string& sub,
                              std::string& out, bool& first) {
    std::string dir = base + "\\" + sub;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
        std::string rel = sub + "/" + fd.cFileName;   // forward-slash, matches remote manifest
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            manifest_scan_dir(base, rel, out, first);
        } else {
            std::string sha = sha256_hex_file((base + "\\" + rel).c_str());
            unsigned long long sz = ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            if (!first) out += ",";
            first = false;
            out += "\"" + rel + "\":{\"sha256\":\"" + sha + "\",\"size\":" + std::to_string(sz) + "}";
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static void heal_local_manifest() {
    std::string localPath = paths_get_appdata_dir() + "\\version.json";
    std::string existing = read_file_str(localPath);
    if (!existing.empty() && json_get_str(existing, "app") == APP_VERSION) return;  // baseline current
    std::string installDir = paths_get_install_dir();
    if (GetFileAttributesA((installDir + "\\bin\\monitor_app.exe").c_str()) == INVALID_FILE_ATTRIBUTES) {
        LOG_WARN("main", "heal_local_manifest: install not found at %s - skip", installDir.c_str());
        return;
    }
    std::string files;
    bool first = true;
    manifest_scan_dir(installDir, "bin", files, first);
    manifest_scan_dir(installDir, "frontend", files, first);
    manifest_scan_dir(installDir, "config", files, first);
    if (first) { LOG_WARN("main", "heal_local_manifest: scanned 0 files - skip"); return; }
    std::string json = std::string("{\"app\":\"") + APP_VERSION + "\",\"files\":{" + files + "}}";
    FILE* f = fopen(localPath.c_str(), "wb");
    if (f) {
        fwrite(json.data(), 1, json.size(), f);
        fclose(f);
        LOG("main", "heal_local_manifest: rebuilt appdata baseline -> %s (%s)", localPath.c_str(), APP_VERSION);
    } else {
        LOG_WARN("main", "heal_local_manifest: cannot write %s", localPath.c_str());
    }
}

// First-launch updater self-heal. Breaks the pre-0.3.5 updater deadlock: an old
// updater cannot overwrite its own running image, so a stale bin\updater.exe would
// persist forever. If the installed updater.exe sha != version.json's expected sha
// and bin\updater.new (a copy of the current updater) exists, launch it with
// --self-install to replace updater.exe. We only START it; it does the copy.
static void check_and_heal_updater() {
    std::string installDir = paths_get_install_dir();
    // Prefer the appdata baseline (rebuilt at startup to match the running exe);
    // the installed version.json can be stale (old updater never refreshed it).
    std::string manifest = read_file_str(paths_get_appdata_dir() + "\\version.json");
    if (manifest.empty()) manifest = read_file_str(installDir + "\\version.json");
    if (manifest.empty()) return;
    size_t p = manifest.find("\"bin/updater.exe\"");
    if (p == std::string::npos) return;
    std::string expectSha = json_get_str(manifest.substr(p), "sha256");
    if (expectSha.empty()) return;
    std::string actualSha = sha256_hex_file((installDir + "\\bin\\updater.exe").c_str());
    if (actualSha == expectSha) return;  // already current
    std::string newPath = installDir + "\\bin\\updater.new";
    if (GetFileAttributesA(newPath.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    LOG("main", "updater stale (have=%s want=%s) - launching updater.new --self-install",
        actualSha.empty() ? "?" : actualSha.c_str(), expectSha.c_str());
    ShellExecuteA(nullptr, "open", newPath.c_str(), "--self-install", installDir.c_str(), SW_HIDE);
}

// ── WinMain ─────────────────────────────────────────────────
int WINAPI WinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE, _In_ LPSTR lpCmdLine, _In_ int nCmdShow)
{
    g_boot_tick = GetTickCount64();  // perf: t0 for startup latency instrumentation
    // ── Single-instance guard ──
    // Named mutex prevents multiple instances of the same app.
    // If another instance is already running, activate its window and exit.
    // Runs BEFORE capture_log_init on purpose: this path must NOT touch the
    // logging system — a short-lived second process must not spawn a new log
    // session file (would pollute the front-end's history list). Signal via
    // exit code only: 2 = instance already running (raised existing window).
    g_singleton_mutex = CreateMutexW(NULL, TRUE, SINGLE_INSTANCE_MUTEX);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (g_singleton_mutex) CloseHandle(g_singleton_mutex);
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
    // App icon (app.rc IDI_APPICON=1, a multi-size .ico). Big = taskbar / Alt-Tab,
    // small = title-bar corner. Without these the window showed no title-bar icon
    // and a tiny/default taskbar icon.
    // Windows 10/11 taskbar renders the icon DPI-scaled (nominally 24px logical),
    // so load at the CURRENT system-DPI metrics — NOT a fixed 32 (too small on
    // high-DPI) nor a huge 256 (that can fail to load and silently fall back to the
    // 16px small icon, which looked ~1/3 size). GetSystemMetricsForDpi picks the
    // right physical size; LoadImage then grabs the matching frame from the .ico.
    UINT sysDpi = GetDpiForSystem();
    int bigSz = GetSystemMetricsForDpi(SM_CXICON,   sysDpi);
    int smSz  = GetSystemMetricsForDpi(SM_CXSMICON, sysDpi);
    HICON hIconBig   = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(1), IMAGE_ICON, bigSz, bigSz, LR_DEFAULTCOLOR);
    HICON hIconSmall = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(1), IMAGE_ICON, smSz,  smSz,  LR_DEFAULTCOLOR);
    wc.hIcon         = hIconBig;
    wc.hIconSm       = hIconSmall;
    // Dark background brush: if the window is ever painted before the webview
    // attaches, it shows dark chrome instead of a white flash.
    wc.hbrBackground = CreateSolidBrush(RGB(24, 24, 27));
    wc.lpszClassName = WINDOW_CLASS;
    RegisterClassExW(&wc);

    g_hwnd = CreateWindowExW(
        0, WINDOW_CLASS, TITLE,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, DEFAULT_W, DEFAULT_H,
        nullptr, nullptr, hInstance, nullptr);

    if (!g_hwnd) return 1;

    // Append elevation to window title — "Game Agent Monitor (Dev) (管理员)"
    {
        BOOL isAdmin = FALSE;
        PSID adminGroup = nullptr;
        SID_IDENTIFIER_AUTHORITY NtAuthority = SECURITY_NT_AUTHORITY;
        if (AllocateAndInitializeSid(&NtAuthority, 2,
                SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
                0, 0, 0, 0, 0, 0, &adminGroup)) {
            CheckTokenMembership(nullptr, adminGroup, &isAdmin);
            FreeSid(adminGroup);
        }
        wchar_t titled[192];
        swprintf(titled, 192, L"%s (%s)", TITLE, isAdmin ? L"管理员" : L"普通");
        SetWindowTextW(g_hwnd, titled);
        LOG("main", "Window title updated (admin=%d)", (int)isAdmin);
    }

    // Belt-and-suspenders: also set the window icons explicitly — some shells
    // don't adopt the class icon for the taskbar button.
    if (hIconBig)   SendMessageW(g_hwnd, WM_SETICON, ICON_BIG,   (LPARAM)hIconBig);
    if (hIconSmall) SendMessageW(g_hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hIconSmall);

    vd_set_main_hwnd(g_hwnd);  // tell virtual_desktop which HWND to use for desktop detection

    // Keep the window HIDDEN. The frontend calls 'show_window' once its first
    // frame is painted (see show_main_window / WM_SHOW_WINDOW), so the window
    // pops in already showing the UI instead of a ~2-4s white blank during
    // WebView2 env creation + React mount. Mirrors MXU/Tauri's visible:false.
    // Safety net: reveal anyway after 8s if the frontend never signals (broken JS).
    (void)nCmdShow;  // intentionally not shown here
    SetTimer(g_hwnd, TIMER_SHOW_SAFETY, 8000, nullptr);

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    InitWebView2(g_hwnd);

    paths_init();

    // Rebuild the appdata baseline manifest if it doesn't match this exe — keeps
    // the update diff correct without depending on the updater (see helper above).
    heal_local_manifest();

    // Self-heal a stale updater.exe left by an older updater (see helper above).
    check_and_heal_updater();

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

// Reveal the main window (idempotent). Called once the frontend's first frame is
// painted (WM_SHOW_WINDOW, from the 'show_window' command) or by the safety timer.
static void show_main_window()
{
    if (g_window_shown) return;
    g_window_shown = true;
    KillTimer(g_hwnd, TIMER_SHOW_SAFETY);
    ShowWindow(g_hwnd, SW_SHOWNORMAL);
    SetForegroundWindow(g_hwnd);
    // Help text painted on the dark fallback background (hbrBackground = grey).
    // Only visible when the frontend failed to load — WebView2 paints over it if
    // it ever attaches. Uses a static child control instead of SetWindowText so
    // the title bar stays clean.
    {
        const wchar_t* help =
            L"Game Agent Monitor 未能正常加载。\r\n\r\n"
            L"这可能是因为 WebView2 环境创建失败,或前端资源损坏。\r\n\r\n"
            L"请尝试:\r\n"
            L"1. 重新安装 Game Agent Monitor\r\n"
            L"2. 安装/修复 Microsoft Edge WebView2 Runtime\r\n"
            L"   (go.microsoft.com/fwlink/p/?LinkId=2124703)\r\n"
            L"3. 访问 gitee.com/Andyqwe44/tictactoe 获取最新安装包";
        RECT rc; GetClientRect(g_hwnd, &rc);
        HWND hHelp = CreateWindowExW(0, L"STATIC", help,
            WS_CHILD | WS_VISIBLE | SS_CENTER | SS_EDITCONTROL,
            0, 0, rc.right, rc.bottom, g_hwnd, nullptr, nullptr, nullptr);
        if (hHelp) {
            // White text on the dark background, monospaced for readability.
            SetWindowLongPtrW(hHelp, GWL_STYLE, GetWindowLongPtrW(hHelp, GWL_STYLE) & ~WS_BORDER);
        }
    }
    // The WebView2 was created on a HIDDEN window → its compositor was paused and
    // it holds a blank frame. Revealing the HWND alone does NOT force a repaint
    // (that was the grey-window bug). Make the controller visible and re-apply
    // bounds to trigger a fresh composition now that the window is on-screen.
    if (g_webviewController) {
        g_webviewController->put_IsVisible(TRUE);
        RECT rc; GetClientRect(g_hwnd, &rc);
        g_webviewController->put_Bounds(rc);
    }
    LOG("main", "perf: window shown t+%llums", GetTickCount64() - g_boot_tick);
}

// Exposed to commands.cpp: the frontend sent 'show_window'. Post to WndProc so
// the reveal always runs on the UI thread regardless of the caller's thread.
void app_post_show_window()
{
    if (g_hwnd) PostMessageW(g_hwnd, WM_APP_SHOW_WINDOW, 0, 0);
}

// Release the single-instance mutex so a relaunched copy of ourselves (the
// permission switch) can start immediately. Without this, the new process hits
// the single-instance guard and exits (code 2) because we haven't fully quit yet
// — which is why the app "closed but didn't reopen" on a permission switch.
void app_release_singleton()
{
    if (g_singleton_mutex) { CloseHandle(g_singleton_mutex); g_singleton_mutex = nullptr; }
}

// Re-acquire the single-instance lock if a relaunch was aborted (e.g. the user
// denied the UAC prompt) — restores single-instance protection for the instance
// that stays running.
void app_acquire_singleton()
{
    if (!g_singleton_mutex) g_singleton_mutex = CreateMutexW(NULL, TRUE, SINGLE_INSTANCE_MUTEX);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_APP_SHOW_WINDOW:
        show_main_window();
        break;
    case WM_TIMER:
        if (wParam == TIMER_SHOW_SAFETY) {
            LOG_WARN("main", "frontend did not signal ready in 8s — revealing window (safety)");
            show_main_window();
        }
        break;
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
    case WM_UPDATE_PROGRESS: {
        // Download thread posted a progress/terminal update. Read the shared state
        // on this STA thread and push it to JS; on completion, launch updater + exit.
        UpdateProgress up = update_get_progress();
        const char* phase = up.failed ? "error" : (up.succeeded ? "done" : "download");
        std::string json = std::string("{\"type\":\"update_progress\",\"phase\":\"") + phase + "\""
            + ",\"current_file\":" + std::to_string(up.current_file)
            + ",\"total_files\":" + std::to_string(up.total_files)
            + ",\"skipped_files\":" + std::to_string(up.skipped_files)
            + ",\"file\":\"" + js_escape(up.file_path) + "\""
            + ",\"done_bytes\":" + std::to_string(up.done_bytes)
            + ",\"total_bytes\":" + std::to_string(up.total_bytes)
            + ",\"skipped_bytes\":" + std::to_string(up.skipped_bytes);
        if (up.failed) json += ",\"error_file\":\"" + js_escape(up.error_file) + "\"";
        json += "}";
        PostJsonToWebView(json);
        if (up.succeeded) {
            if (update_launch_updater()) { Sleep(200); PostQuitMessage(0); }
        }
        break;
    }
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
    // Explicit writable user-data folder. Passing nullptr makes WebView2 default
    // the folder to next-to-the-exe (e.g. C:\Program Files\GameAgentMonitor\bin\),
    // which a standard (non-admin) user cannot write → env creation fails → white
    // screen. LOCALAPPDATA is always user-writable. This is the root white-screen fix.
    std::string udf = paths_get_appdata_dir() + "\\WebView2";
    int wlen = MultiByteToWideChar(CP_UTF8, 0, udf.c_str(), (int)udf.size(), nullptr, 0);
    std::wstring wudf(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, udf.c_str(), (int)udf.size(), &wudf[0], wlen);

    return CreateCoreWebView2EnvironmentWithOptions(
        nullptr, wudf.c_str(), nullptr,
        new EnvCreatedHandler([hwnd](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
            if (FAILED(result)) {
                LOG_ERROR("main", "WebView2 env create failed: 0x%08lX", (unsigned long)result);
                MessageBoxW(hwnd,
                    L"WebView2 environment creation failed.\n"
                    L"Ensure the WebView2 Runtime is installed and the user "
                    L"data folder is writable.",
                    L"Game Agent Monitor", MB_ICONERROR | MB_OK);
                return result;
            }

            // Capture SharedBuffer interfaces
            env->QueryInterface(IID_PPV_ARGS(&g_env12));
            LOG("main", "perf: WebView2 env created t+%llums", GetTickCount64() - g_boot_tick);

            return env->CreateCoreWebView2Controller(hwnd,
                new ControllerCreatedHandler([hwnd](HRESULT result, ICoreWebView2Controller* ctrl) -> HRESULT {
                    if (FAILED(result)) {
                        LOG_ERROR("main", "WebView2 controller create failed: 0x%08lX",
                            (unsigned long)result);
                        MessageBoxW(hwnd, L"WebView2 controller creation failed.",
                            L"Game Agent Monitor", MB_ICONERROR | MB_OK);
                        return result;
                    }

                    g_webviewController = ctrl;
                    g_webviewController->get_CoreWebView2(&g_webview);

                    // Capture ICoreWebView2_17 for PostSharedBufferToScript
                    g_webview->QueryInterface(IID_PPV_ARGS(&g_webview3));
                    g_webview->QueryInterface(IID_PPV_ARGS(&g_webview17));

                    // Register WebMessage handler (replaces Tauri invoke)
                    g_webview->add_WebMessageReceived(new WebMessageHandler(), nullptr);

                    // perf: mark when the first navigation finishes (≈ white end)
                    g_webview->add_NavigationCompleted(new NavCompletedHandler(), nullptr);
                    LOG("main", "perf: controller ready t+%llums", GetTickCount64() - g_boot_tick);

                    // Inject user settings BEFORE first paint so React never flashes defaults.
                    // Runs on every document create (full navigation); HMR module swaps keep state.
                    {
                        std::string boot = settings_get_boot_json();
                        // Apply theme/accent in the same script so LoadingScreen is already tinted.
                        std::string js =
                            "window.__BOOT_SETTINGS__=" + boot + ";"
                            "(function(){try{var s=window.__BOOT_SETTINGS__||{};"
                            "var theme=s.theme||'light';"
                            "var dark=theme==='dark'||(theme==='system'&&window.matchMedia('(prefers-color-scheme: dark)').matches);"
                            "document.documentElement.classList.toggle('dark',!!dark);"
                            "var useDev=!!s.devMode;"
                            "var accent=(useDev?s.devAccent:s.normalAccent)||'#3B82F6';"
                            "var sec=(useDev?s.devSecondaryAccent:s.normalSecondaryAccent)||'#F97316';"
                            "document.documentElement.style.setProperty('--color-accent',accent);"
                            "document.documentElement.style.setProperty('--color-accent-secondary',sec);"
                            "var v=parseInt(String(accent).slice(1),16);if(!isNaN(v)){"
                            "var r=Math.max(0,((v>>16)&255)-38),g=Math.max(0,((v>>8)&255)-38),b=Math.max(0,(v&255)-38);"
                            "var h='#'+((r<<16)|(g<<8)|b).toString(16).padStart(6,'0');"
                            "document.documentElement.style.setProperty('--color-accent-hover',h);}"
                            "}catch(e){}})();";
                        int wlen = MultiByteToWideChar(CP_UTF8, 0, js.c_str(), (int)js.size(), nullptr, 0);
                        std::wstring wjs(wlen, L'\0');
                        MultiByteToWideChar(CP_UTF8, 0, js.c_str(), (int)js.size(), &wjs[0], wlen);
                        g_webview->AddScriptToExecuteOnDocumentCreated(wjs.c_str(), nullptr);
                        LOG("main", "boot settings injected (%zub)", boot.size());
                    }

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
