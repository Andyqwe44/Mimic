/**
 * virtual_desktop.cpp — Virtual desktop enumeration and switching.
 *
 * Uses undocumented IVirtualDesktopManagerInternal COM interface
 * obtained via IServiceProvider from the Immersive Shell.
 */
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include "virtual_desktop.h"
#include "../../logger/logger.h"
#include <string>
#include <vector>

// ── Documented IVirtualDesktopManager (local def, conflicts with shobjidl.h) ──
// CLSID: {AA509086-5CA9-4C25-8F95-589D3C07B48A}
static const GUID CLSID_VirtualDesktopManager = {
    0xAA509086, 0x5CA9, 0x4C25, {0x8F, 0x95, 0x58, 0x9D, 0x3C, 0x07, 0xB4, 0x8A}};
// IID: {A5CD92FF-29BE-454C-8D04-D82879FB3F1B}
static const GUID IID_IVirtualDesktopManager = {
    0xA5CD92FF, 0x29BE, 0x454C, {0x8D, 0x04, 0xD8, 0x28, 0x79, 0xFB, 0x3F, 0x1B}};

struct IVirtualDesktopManager : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE IsWindowOnCurrentVirtualDesktop(
        HWND topLevelWindow, BOOL* onCurrent) = 0;                            // 3
    virtual HRESULT STDMETHODCALLTYPE GetWindowDesktopId(
        HWND topLevelWindow, GUID* desktopId) = 0;                            // 4
    virtual HRESULT STDMETHODCALLTYPE MoveWindowToDesktop(
        HWND topLevelWindow, REFGUID desktopId) = 0;                          // 5
};

// ── IObjectArray COM interface (local def to avoid SDK ObjectArray.h conflict) ──
// {92CA9DCD-5622-4BBA-A805-5E9F541BD8C9}
static const GUID IID_IObjectArray = {
    0x92CA9DCD, 0x5622, 0x4BBA, {0xA8, 0x05, 0x5E, 0x9F, 0x54, 0x1B, 0xD8, 0xC9}};

struct IObjectArray : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetCount(UINT* pcObjects) = 0;           // 3
    virtual HRESULT STDMETHODCALLTYPE GetAt(
        UINT iIndex, REFIID riid, void** ppvObject) = 0;                      // 4
};

// ── Version / runtime state ──────────────────────────────────
static bool      g_vd_ok = false;
static int       g_vd_version = 0;   // 0=unknown, 10=Win10, 11=Win11, 12=Win11_23H2
static void*     g_vd_mgr = nullptr; // opaque pointer to the manager interface
static GUID      g_vd_desktop_guid = {}; // IID to use for GetAt (IVirtualDesktop)
static HWND      g_main_hwnd = nullptr;  // GAM main window for desktop detection

// ── IServiceProvider from Immersive Shell ────────────────────
static IServiceProvider* vd_get_service_provider() {
    IServiceProvider* sp = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ImmersiveShell, nullptr,
        CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&sp));
    if (FAILED(hr) || !sp) {
        LOG("vd", "CoCreateInstance(ImmersiveShell) failed: 0x%08lX", hr);
    }
    return sp;
}

// ── Probe for IVirtualDesktopManagerInternal ─────────────────
static bool vd_probe() {
    if (g_vd_ok) return true;

    IServiceProvider* sp = vd_get_service_provider();
    if (!sp) return false;

    void* mgr = nullptr;
    int version = 0;
    GUID desktop_guid = GUID_NULL;

    // Try Win11 23H2+ first
    HRESULT hr = sp->QueryService(CLSID_VDMInternal, IID_VDMInternal_Win11_23H2, &mgr);
    if (SUCCEEDED(hr) && mgr) {
        version = 12;
        desktop_guid = IID_IVirtualDesktop_Win11_23H2;
        LOG("vd", "using Win11 23H2+ interface");
    }

    // Try Win11 21H2
    if (!mgr) {
        hr = sp->QueryService(CLSID_VDMInternal, IID_VDMInternal_Win11, &mgr);
        if (SUCCEEDED(hr) && mgr) {
            version = 11;
            desktop_guid = IID_IVirtualDesktop;
            LOG("vd", "using Win11 21H2 interface");
        }
    }

    // Try Win10
    if (!mgr) {
        hr = sp->QueryService(CLSID_VDMInternal, IID_VDMInternal_Win10, &mgr);
        if (SUCCEEDED(hr) && mgr) {
            version = 10;
            desktop_guid = IID_IVirtualDesktop;
            LOG("vd", "using Win10 interface");
        }
    }

    sp->Release();

    if (!mgr) {
        LOG("vd", "all QueryService attempts failed");
        return false;
    }

    g_vd_mgr = mgr;
    g_vd_version = version;
    g_vd_desktop_guid = desktop_guid;
    g_vd_ok = true;
    return true;
}

static void vd_release_manager() {
    if (g_vd_mgr) {
        ((IUnknown*)g_vd_mgr)->Release();
        g_vd_mgr = nullptr;
    }
    g_vd_ok = false;
    g_vd_version = 0;
    g_vd_desktop_guid = GUID_NULL;
}

// ── Get IObjectArray of all desktops (caller must Release) ───
static IObjectArray* vd_get_desktop_array() {
    if (!g_vd_ok || !g_vd_mgr) return nullptr;

    IUnknown* unk_arr = nullptr;
    HRESULT hr = E_FAIL;

    if (g_vd_version == 12) {
        auto* mgr = (IVDManagerInternal_Win11_23H2*)g_vd_mgr;
        hr = mgr->GetDesktops(nullptr, &unk_arr);
    } else if (g_vd_version >= 10) {
        auto* mgr = (IVDManagerInternal_Win11*)g_vd_mgr;
        hr = mgr->GetDesktops(nullptr, &unk_arr);
    }

    if (FAILED(hr) || !unk_arr) {
        LOG("vd", "GetDesktops failed: 0x%08lX", hr);
        return nullptr;
    }

    // Query for our local IObjectArray interface
    IObjectArray* arr = nullptr;
    hr = unk_arr->QueryInterface(IID_IObjectArray, (void**)&arr);
    unk_arr->Release();

    if (FAILED(hr) || !arr) {
        LOG("vd", "QI for IObjectArray failed: 0x%08lX", hr);
    }
    return arr;
}

// ── Get desktop object at index (returns IUnknown*; caller releases) ──
static IUnknown* vd_get_desktop_at(UINT index) {
    IObjectArray* arr = vd_get_desktop_array();
    if (!arr) return nullptr;

    IUnknown* desk = nullptr;
    HRESULT hr = arr->GetAt(index, g_vd_desktop_guid, (void**)&desk);
    arr->Release();

    if (FAILED(hr)) {
        LOG("vd", "GetAt(%u) failed: 0x%08lX", index, hr);
    }
    return desk;
}

// ── Get GUID of a desktop object ─────────────────────────────
static GUID vd_get_desktop_guid(IUnknown* desk) {
    GUID g = GUID_NULL;
    if (!desk) return g;

    if (g_vd_version == 12) {
        auto* vd = (IVirtualDesktop_Win11*)desk;
        vd->GetId(&g);
    } else {
        auto* vd = (IVirtualDesktop_Win10*)desk;
        vd->GetId(&g);
    }
    return g;
}

// ── Helpers: get current desktop index ───────────────────────
static int vd_get_current_index() {
    GUID current_guid = vd_get_current_desktop_guid();
    if (IsEqualGUID(current_guid, GUID_NULL)) return -1;

    IObjectArray* arr = vd_get_desktop_array();
    if (!arr) return -1;

    UINT count = 0;
    arr->GetCount(&count);

    int current_idx = -1;
    for (UINT i = 0; i < count; i++) {
        IUnknown* desk = nullptr;
        if (SUCCEEDED(arr->GetAt(i, g_vd_desktop_guid, (void**)&desk)) && desk) {
            GUID g = vd_get_desktop_guid(desk);
            if (IsEqualGUID(g, current_guid)) {
                current_idx = (int)i;
                desk->Release();
                break;
            }
            desk->Release();
        }
    }

    arr->Release();
    return current_idx;
}

// ── Public API: list desktops ────────────────────────────────
std::string vd_list_desktops() {
    // Prefer registry order (absolute, Task View left-to-right)
    std::vector<GUID> order = vd_get_registry_desktop_order();
    if (!order.empty()) {
        GUID current_guid = vd_get_current_desktop_guid();
        std::string json = "[";
        for (size_t i = 0; i < order.size(); i++) {
            if (i > 0) json += ",";
            bool is_current = IsEqualGUID(order[i], current_guid);
            int num = (int)i + 1; // D1, D2, D3...
            json += "{\"name\":\"D" + std::to_string(num) + "\"";
            json += ",\"index\":" + std::to_string(num);
            json += ",\"current\":" + std::string(is_current ? "true" : "false") + "}";
        }
        json += "]";
        LOG("vd", "list_desktops(reg): %zu desktops", order.size());
        return json;
    }

    // Fallback: try undocumented COM API
    if (!vd_probe()) return "[]";

    IObjectArray* arr = vd_get_desktop_array();
    if (!arr) return "[]";

    UINT count = 0;
    arr->GetCount(&count);
    int current_idx = vd_get_current_index();

    std::string json = "[";
    for (UINT i = 0; i < count; i++) {
        if (i > 0) json += ",";
        bool is_current = (current_idx >= 0 && (UINT)current_idx == i);
        int num = (int)i + 1;
        json += "{\"name\":\"D" + std::to_string(num) + "\"";
        json += ",\"index\":" + std::to_string(num);
        json += ",\"current\":" + std::string(is_current ? "true" : "false") + "}";
    }
    json += "]";

    arr->Release();
    LOG("vd", "list_desktops(COM): %u desktops, current=%d", count, current_idx);
    return json;
}

// ── Public API: switch desktop ───────────────────────────────
std::string vd_switch_desktop(int index) {
    if (!g_vd_ok || !g_vd_mgr) {
        if (!vd_probe()) {
            return R"({"ok":false,"error":"VirtualDesktopManagerInternal not available"})";
        }
    }

    if (index < 0) {
        return R"({"ok":false,"error":"invalid desktop index"})";
    }

    IUnknown* desk = vd_get_desktop_at((UINT)index);
    if (!desk) {
        return R"({"ok":false,"error":"desktop not found"})";
    }

    HRESULT sw_hr = E_FAIL;
    if (g_vd_version == 12) {
        auto* mgr = (IVDManagerInternal_Win11_23H2*)g_vd_mgr;
        sw_hr = mgr->SwitchDesktop(nullptr, (IVirtualDesktop_Win11*)desk);
    } else if (g_vd_version >= 10) {
        auto* mgr = (IVDManagerInternal_Win11*)g_vd_mgr;
        sw_hr = mgr->SwitchDesktop(nullptr, (IVirtualDesktop_Win10*)desk);
    }

    desk->Release();

    if (SUCCEEDED(sw_hr)) {
        LOG("vd", "switched to desktop %d", index);
        return R"({"ok":true})";
    } else {
        LOG("vd", "SwitchDesktop failed: 0x%08lX", sw_hr);
        return R"({"ok":false,"error":"SwitchDesktop failed"})";
    }
}

// ── Public API: set main window HWND ──────────────────────────
void vd_set_main_hwnd(HWND hwnd) {
    g_main_hwnd = hwnd;
}

// ── Public API: get current desktop GUID ─────────────────────
GUID vd_get_current_desktop_guid() {
    GUID guid = GUID_NULL;
    IVirtualDesktopManager* vdm = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_VirtualDesktopManager, nullptr,
        CLSCTX_INPROC_SERVER, IID_IVirtualDesktopManager, (void**)&vdm);
    if (SUCCEEDED(hr) && vdm) {
        // Prefer GAM's real main window — a temp popup always lands on the
        // process's home desktop, not where the window was moved via Task View.
        HWND hwnd = (g_main_hwnd && IsWindow(g_main_hwnd)) ? g_main_hwnd : nullptr;
        if (hwnd) {
            vdm->GetWindowDesktopId(hwnd, &guid);
        } else {
            HWND tmp = CreateWindowExW(0, L"Static", L"", WS_POPUP,
                0, 0, 1, 1, nullptr, nullptr, nullptr, nullptr);
            if (tmp) {
                vdm->GetWindowDesktopId(tmp, &guid);
                DestroyWindow(tmp);
            }
        }
        vdm->Release();
    }
    return guid;
}

// ── Public API: get desktop order from registry ──────────────
std::vector<GUID> vd_get_registry_desktop_order() {
    std::vector<GUID> result;

    // Read VirtualDesktopIDs from registry (Task View left-to-right order)
    const wchar_t* key_path = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\VirtualDesktops";
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, key_path, 0, KEY_READ, &hKey) != ERROR_SUCCESS) {
        LOG("vd", "reg: cannot open VirtualDesktops key");
        return result;
    }

    DWORD size = 0;
    DWORD type = 0;
    if (RegQueryValueExW(hKey, L"VirtualDesktopIDs", nullptr, &type, nullptr, &size) != ERROR_SUCCESS
        || type != REG_BINARY || size == 0) {
        RegCloseKey(hKey);
        LOG("vd", "reg: VirtualDesktopIDs not found or not binary");
        return result;
    }

    std::vector<BYTE> data(size);
    if (RegQueryValueExW(hKey, L"VirtualDesktopIDs", nullptr, nullptr, data.data(), &size) != ERROR_SUCCESS) {
        RegCloseKey(hKey);
        return result;
    }
    RegCloseKey(hKey);

    // Each desktop GUID is 16 bytes in binary form
    int count = (int)size / 16;
    for (int i = 0; i < count; i++) {
        GUID g;
        memcpy(&g, data.data() + i * 16, 16);
        result.push_back(g);
    }

    LOG("vd", "reg: %d desktops in Task View order", count);
    return result;
}
