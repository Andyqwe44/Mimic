/**
 * virtual_desktop.h — Undocumented IVirtualDesktopManagerInternal COM interface.
 *
 * Windows 11 virtual desktop enumeration + switching.
 * These interfaces are reverse-engineered from twinui.pcshell.dll.
 * They may break on Windows updates — runtime version probing is used.
 *
 * Reference: https://github.com/MScholtes/VirtualDesktop (C# definitions)
 */

#pragma once
#include <windows.h>
#include <objidl.h>
#include <Unknwn.h>
#include <string>
#include <vector>

// ── GUIDs ────────────────────────────────────────────────────
// Immersive Shell service provider
// {C2F03A33-21F5-47FA-B4BB-156362A2F239}
static const GUID CLSID_ImmersiveShell = {
    0xC2F03A33, 0x21F5, 0x47FA, {0xB4, 0xBB, 0x15, 0x63, 0x62, 0xA2, 0xF2, 0x39}};

// CLSID for IVirtualDesktopManagerInternal (same across Win10/Win11)
// {C5E0CDCA-7B6E-41B2-9FC4-D93975CC467B}
static const GUID CLSID_VDMInternal = {
    0xC5E0CDCA, 0x7B6E, 0x41B2, {0x9F, 0xC4, 0xD9, 0x39, 0x75, 0xCC, 0x46, 0x7B}};

// ── IIDs — version-dependent, tried in order ────────────────

// Win11 23H2+ (build ≥ 22621)
// {A3175F2D-239C-4BD2-8AA0-EEBA8B0B138E}
static const GUID IID_VDMInternal_Win11_23H2 = {
    0xA3175F2D, 0x239C, 0x4BD2, {0x8A, 0xA0, 0xEE, 0xBA, 0x8B, 0x0B, 0x13, 0x8E}};

// Win11 21H2 (build 22000–22620)
// {B2F925B9-5A0F-4D2E-9F4D-2B1507593C10}
static const GUID IID_VDMInternal_Win11 = {
    0xB2F925B9, 0x5A0F, 0x4D2E, {0x9F, 0x4D, 0x2B, 0x15, 0x07, 0x59, 0x3C, 0x10}};

// Win10 (build < 22000)
// {F31574D6-B682-4CDC-BD56-1827860ABEC6}
static const GUID IID_VDMInternal_Win10 = {
    0xF31574D6, 0xB682, 0x4CDC, {0xBD, 0x56, 0x18, 0x27, 0x86, 0x0A, 0xBE, 0xC6}};

// IVirtualDesktop IIDs
// Win11 23H2+
// {3F07F4BE-B107-441A-AF0F-39D82529072C}
static const GUID IID_IVirtualDesktop_Win11_23H2 = {
    0x3F07F4BE, 0xB107, 0x441A, {0xAF, 0x0F, 0x39, 0xD8, 0x25, 0x29, 0x07, 0x2C}};

// Win10/Win11 21H2
// {FF72FFDD-BE7E-43FC-9C03-AD81681E88E4}
static const GUID IID_IVirtualDesktop = {
    0xFF72FFDD, 0xBE7E, 0x43FC, {0x9C, 0x03, 0xAD, 0x81, 0x68, 0x1E, 0x88, 0xE4}};

// ── IServiceProvider — defined in shobjidl.h but define here for clarity ──
// {6D5140C1-7436-11CE-8034-00AA006009FA}
#ifndef __IServiceProvider_INTERFACE_DEFINED__
#define __IServiceProvider_INTERFACE_DEFINED__
struct IServiceProvider : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE QueryService(
        REFGUID guidService, REFIID riid, void** ppvObject) = 0;
};
#endif

// ── IVirtualDesktop — minimal definition (just methods we need) ──

// Win11 23H2+ version: {3F07F4BE-B107-441A-AF0F-39D82529072C}
struct IVirtualDesktop_Win11 : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE IsViewVisible(
        IUnknown* pView, BOOL* pfVisible) = 0;                                // 3
    virtual HRESULT STDMETHODCALLTYPE GetId(GUID* pGuid) = 0;                  // 4
};

// Win10/Win11 21H2 version: {FF72FFDD-...}
struct IVirtualDesktop_Win10 : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE IsViewVisible(
        IUnknown* pView, BOOL* pfVisible) = 0;                                // 3
    virtual HRESULT STDMETHODCALLTYPE GetId(GUID* pGuid) = 0;                  // 4
};

// ── IVirtualDesktopManagerInternal ───────────────────────────
// Vtable layouts differ per Windows version.
// NOTE: IObjectArray conflicts with WinRT IObjectArray from ObjectArray.h,
// so we use IUnknown* in vtable signatures. Actual type is IObjectArray.

// Win11 23H2+ (build ≥ 22621): IID {A3175F2D-...}
struct IVDManagerInternal_Win11_23H2 : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetCount(
        HMONITOR hMonitor, UINT* pcDesktops) = 0;                             // 3
    virtual HRESULT STDMETHODCALLTYPE MoveViewToDesktop(
        IUnknown* pView, IVirtualDesktop_Win11* pDesktop) = 0;                // 4
    virtual HRESULT STDMETHODCALLTYPE CanViewMoveDesktops(
        IUnknown* pView, BOOL* pfCan) = 0;                                    // 5
    virtual HRESULT STDMETHODCALLTYPE GetCurrentDesktop(
        HMONITOR hMonitor, IVirtualDesktop_Win11** ppDesktop) = 0;            // 6
    virtual HRESULT STDMETHODCALLTYPE GetAllCurrentDesktops(
        IUnknown** ppDesktops) = 0;                                           // 7
    virtual HRESULT STDMETHODCALLTYPE GetDesktops(
        HMONITOR hMonitor, IUnknown** ppDesktops) = 0;                        // 8
    virtual HRESULT STDMETHODCALLTYPE GetAdjacentDesktop(
        IVirtualDesktop_Win11* pFrom, int uDirection,
        IVirtualDesktop_Win11** ppAdjacent) = 0;                              // 9
    virtual HRESULT STDMETHODCALLTYPE SwitchDesktop(
        HMONITOR hMonitor, IVirtualDesktop_Win11* pDesktop) = 0;              // 10
    virtual HRESULT STDMETHODCALLTYPE CreateDesktopW(
        HMONITOR hMonitor, IVirtualDesktop_Win11** ppDesktop) = 0;            // 11
    virtual HRESULT STDMETHODCALLTYPE MoveDesktop(
        IVirtualDesktop_Win11* pDesktop, HMONITOR hMonitor,
        int nIndex) = 0;                                                      // 12
    virtual HRESULT STDMETHODCALLTYPE RemoveDesktop(
        IVirtualDesktop_Win11* pRemove,
        IVirtualDesktop_Win11* pFallback) = 0;                                // 13
    virtual HRESULT STDMETHODCALLTYPE FindDesktop(
        GUID* pDesktopId, IVirtualDesktop_Win11** ppDesktop) = 0;             // 14
};

// Win11 21H2 (build 22000–22620): IID {B2F925B9-...}
// No GetAllCurrentDesktops, GetCount takes no HMONITOR
struct IVDManagerInternal_Win11 : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetCount(
        UINT* pcDesktops) = 0;                                                // 3
    virtual HRESULT STDMETHODCALLTYPE MoveViewToDesktop(
        IUnknown* pView, IVirtualDesktop_Win10* pDesktop) = 0;                // 4
    virtual HRESULT STDMETHODCALLTYPE CanViewMoveDesktops(
        IUnknown* pView, BOOL* pfCan) = 0;                                    // 5
    virtual HRESULT STDMETHODCALLTYPE GetCurrentDesktop(
        IVirtualDesktop_Win10** ppDesktop) = 0;                               // 6
    virtual HRESULT STDMETHODCALLTYPE GetDesktops(
        HMONITOR hMonitor, IUnknown** ppDesktops) = 0;                        // 7
    virtual HRESULT STDMETHODCALLTYPE GetAdjacentDesktop(
        IVirtualDesktop_Win10* pFrom, int uDirection,
        IVirtualDesktop_Win10** ppAdjacent) = 0;                              // 8
    virtual HRESULT STDMETHODCALLTYPE SwitchDesktop(
        HMONITOR hMonitor, IVirtualDesktop_Win10* pDesktop) = 0;              // 9
    virtual HRESULT STDMETHODCALLTYPE CreateDesktopW(
        IVirtualDesktop_Win10** ppDesktop) = 0;                               // 10
    virtual HRESULT STDMETHODCALLTYPE RemoveDesktop(
        IVirtualDesktop_Win10* pRemove,
        IVirtualDesktop_Win10* pFallback) = 0;                                // 11
    virtual HRESULT STDMETHODCALLTYPE FindDesktop(
        GUID* pDesktopId, IVirtualDesktop_Win10** ppDesktop) = 0;             // 12
};

// ── Public API ───────────────────────────────────────────────

/// List all virtual desktops. Returns JSON array: [{"name":"Desktop 1","current":true},...]
std::string vd_list_desktops();

/// Switch to a virtual desktop by index (0-based).
/// Returns JSON: {"ok":true} or {"ok":false,"error":"..."}
std::string vd_switch_desktop(int index);

/// Get the current desktop GUID (for matching windows to desktops).
/// Returns GUID_NULL on failure.
GUID vd_get_current_desktop_guid();

/// Read desktop GUIDs in Task View order from registry (left-to-right).
/// Returns ordered vector of GUIDs. Empty if registry read fails.
std::vector<GUID> vd_get_registry_desktop_order();
