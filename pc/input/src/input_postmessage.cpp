/**
 * input_postmessage.cpp — PostMessage method (window message layer).
 */
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include "input_common.h"
#include "input_methods.h"
#include "../../logger/logger.h"
#include <windows.h>
#include <cstdio>
#include <cwchar>
#include <vector>

namespace {

struct MessageTarget {
    HWND hwnd = nullptr;
    POINT screen = {};
    POINT client = {};
    bool nonClient = false;
    LRESULT hitTest = HTCLIENT;
};

static LPARAM point_lparam(int x, int y) {
    return MAKELPARAM((WORD)(SHORT)x, (WORD)(SHORT)y);
}

static WPARAM button_mk(const std::string& button) {
    if (button == "right") return MK_RBUTTON;
    if (button == "middle") return MK_MBUTTON;
    return MK_LBUTTON;
}

static bool is_gam_overlay(HWND hwnd) {
    wchar_t className[64] = {};
    if (!hwnd || GetClassNameW(hwnd, className, 64) <= 0) return false;
    return wcscmp(className, L"GAM_CursorOverlay") == 0 ||
           wcscmp(className, L"GAM_RippleOverlay") == 0 ||
           wcscmp(className, L"GAM_DragOverlay") == 0;
}

static HWND top_window_at_point(POINT screen, HWND ignoredRoot) {
    // Always z-order walk. WindowFromPoint alone is wrong when:
    //  - GAM feedback overlays are topmost at the mapped point, or
    //  - a previous mistaken BringWindowToTop left an unrelated app above peers.
    // First visible, non-ignored, non-overlay root that contains the point wins
    // — same rule a real click uses for "which window is under the cursor".
    for (HWND candidate = GetTopWindow(nullptr); candidate;
         candidate = GetWindow(candidate, GW_HWNDNEXT)) {
        if (!IsWindowVisible(candidate) || candidate == ignoredRoot ||
            is_gam_overlay(candidate)) continue;
        RECT rect = {};
        if (GetWindowRect(candidate, &rect) && PtInRect(&rect, screen)) {
            return candidate;
        }
    }
    return nullptr;
}

static HWND deepest_child_at_point(HWND root, POINT screen) {
    HWND current = root;
    for (int depth = 0; current && depth < 32; ++depth) {
        POINT local = screen;
        if (!ScreenToClient(current, &local)) break;
        HWND child = ChildWindowFromPointEx(
            current, local, CWP_SKIPDISABLED | CWP_SKIPINVISIBLE | CWP_SKIPTRANSPARENT);
        if (!child || child == current) break;
        current = child;
    }
    return current;
}

static bool resolve_message_target(
    HWND captureHwnd, HWND ignoredRoot, double nx, double ny,
    MessageTarget& target, std::string& error) {
    if (!norm_to_screen_point(captureHwnd, nx, ny, target.screen)) {
        error = "failed to map capture coordinates to screen";
        return false;
    }

    HWND root = captureHwnd ? captureHwnd : top_window_at_point(target.screen, ignoredRoot);
    if (!root || !IsWindow(root)) {
        error = "no background-input target at mapped desktop position";
        return false;
    }

    target.hwnd = deepest_child_at_point(root, target.screen);
    // ScreenToClient converts in-place — must start from screen coords, not {0,0}.
    target.client = target.screen;
    if (!target.hwnd || !ScreenToClient(target.hwnd, &target.client)) {
        error = "failed to resolve target child window";
        return false;
    }

    RECT clientRect = {};
    if (!GetClientRect(target.hwnd, &clientRect)) {
        error = "failed to get target client rect";
        return false;
    }

    target.nonClient = target.hwnd == root && !PtInRect(&clientRect, target.client);
    if (target.nonClient) {
        DWORD_PTR hit = HTNOWHERE;
        if (!SendMessageTimeoutW(
                root, WM_NCHITTEST, 0, point_lparam(target.screen.x, target.screen.y),
                SMTO_ABORTIFHUNG | SMTO_BLOCK, 100, &hit)) {
            error = "target window did not answer WM_NCHITTEST";
            return false;
        }
        target.hitTest = (LRESULT)hit;
    }
    return true;
}

static bool dispatch_checked(
    bool synchronous, HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, std::string& error) {
    SetLastError(ERROR_SUCCESS);
    if (!synchronous && PostMessageW(hwnd, msg, wp, lp)) return true;

    if (synchronous) {
        DWORD_PTR result = 0;
        if (SendMessageTimeoutW(
                hwnd, msg, wp, lp, SMTO_ABORTIFHUNG | SMTO_BLOCK, 150, &result)) {
            return true;
        }
    }

    DWORD err = GetLastError();
    char detail[128] = {};
    snprintf(detail, sizeof(detail), "%s failed msg=0x%04x error=%lu",
             synchronous ? "SendMessageTimeout" : "PostMessage",
             (unsigned)msg, (unsigned long)err);
    error = detail;
    LOG_ERROR("input", "%s hwnd=0x%llx", detail,
              (unsigned long long)(uintptr_t)hwnd);
    return false;
}

} // namespace

std::string input_postmessage(HWND hWnd, const InputArgs& a) {
    const bool synchronous = a.method == "sendmessage";
    auto dispatch = [&](HWND targetHwnd, UINT msg, WPARAM wp, LPARAM lp, std::string& error) {
        // Hover is high-frequency and does not need synchronous delivery.
        bool syncThisMessage = synchronous && msg != WM_MOUSEMOVE && msg != WM_NCMOUSEMOVE;
        return dispatch_checked(syncThisMessage, targetHwnd, msg, wp, lp, error);
    };
    bool isMouse = (a.type == "click" || a.type == "dblclick" || a.type == "mousedown" ||
                    a.type == "mouseup" || a.type == "move" || a.type == "drag" || a.type == "wheel");
    MessageTarget target;
    std::string error;
    if (isMouse && !resolve_message_target(
            hWnd, (HWND)(uintptr_t)a.ignore_hwnd, a.x_norm, a.y_norm, target, error)) {
        return "{\"ok\":false,\"error\":\"" + error + "\"}";
    }

    if (isMouse) {
        wchar_t className[64] = {};
        GetClassNameW(target.hwnd, className, 64);
        LOG_DEBUG("input", "%s target=0x%llx class=%ls screen=(%ld,%ld) client=(%ld,%ld)",
            a.method.c_str(), (unsigned long long)(uintptr_t)target.hwnd, className,
            (long)target.screen.x, (long)target.screen.y,
            (long)target.client.x, (long)target.client.y);
    }

    auto mouse_msgs = [&](UINT& downMsg, UINT& upMsg, UINT& dblMsg, WPARAM& wDown) {
        downMsg = target.nonClient
            ? ((a.button == "right") ? WM_NCRBUTTONDOWN :
               (a.button == "middle") ? WM_NCMBUTTONDOWN : WM_NCLBUTTONDOWN)
            : ((a.button == "right") ? WM_RBUTTONDOWN :
               (a.button == "middle") ? WM_MBUTTONDOWN : WM_LBUTTONDOWN);
        upMsg = target.nonClient
            ? ((a.button == "right") ? WM_NCRBUTTONUP :
               (a.button == "middle") ? WM_NCMBUTTONUP : WM_NCLBUTTONUP)
            : ((a.button == "right") ? WM_RBUTTONUP :
               (a.button == "middle") ? WM_MBUTTONUP : WM_LBUTTONUP);
        dblMsg = target.nonClient
            ? ((a.button == "right") ? WM_NCRBUTTONDBLCLK :
               (a.button == "middle") ? WM_NCMBUTTONDBLCLK : WM_NCLBUTTONDBLCLK)
            : ((a.button == "right") ? WM_RBUTTONDBLCLK :
               (a.button == "middle") ? WM_MBUTTONDBLCLK : WM_LBUTTONDBLCLK);
        wDown = button_mk(a.button);
    };

    if (a.type == "click" || a.type == "dblclick") {
        UINT downMsg = 0, upMsg = 0, dblMsg = 0;
        WPARAM wDown = 0;
        mouse_msgs(downMsg, upMsg, dblMsg, wDown);
        LPARAM lp = target.nonClient
            ? point_lparam(target.screen.x, target.screen.y)
            : point_lparam(target.client.x, target.client.y);
        WPARAM downWp = target.nonClient ? (WPARAM)target.hitTest : wDown;
        WPARAM upWp = target.nonClient ? (WPARAM)target.hitTest : 0;
        input_remember_keyboard_target(hWnd, target.hwnd);
        if (a.type == "click") {
            if (!dispatch(target.hwnd, downMsg, downWp, lp, error)) {
                return "{\"ok\":false,\"error\":\"" + error + "\"}";
            }
            if (!dispatch(target.hwnd, upMsg, upWp, lp, error)) {
                std::string releaseError;
                dispatch(target.hwnd, upMsg, upWp, lp, releaseError);
                return "{\"ok\":false,\"error\":\"" + error + "\"}";
            }
        } else {
            if (!dispatch(target.hwnd, dblMsg, downWp, lp, error)) {
                return "{\"ok\":false,\"error\":\"" + error + "\"}";
            }
            if (!dispatch(target.hwnd, upMsg, upWp, lp, error)) {
                std::string releaseError;
                dispatch(target.hwnd, upMsg, upWp, lp, releaseError);
                return "{\"ok\":false,\"error\":\"" + error + "\"}";
            }
        }
    }
    else if (a.type == "mousedown" || a.type == "mouseup") {
        if (target.nonClient)
            return "{\"ok\":false,\"error\":\"background non-client press is not supported\"}";
        UINT downMsg = 0, upMsg = 0, dblMsg = 0;
        WPARAM wDown = 0;
        mouse_msgs(downMsg, upMsg, dblMsg, wDown);
        LPARAM lp = point_lparam(target.client.x, target.client.y);
        // Remember hit control for subsequent key/text. Do NOT WM_MOUSEACTIVATE /
        // SetFocus / SetForeground — production must not steal the user's UI.
        input_remember_keyboard_target(hWnd, target.hwnd);
        if (a.type == "mousedown") {
            if (!dispatch(target.hwnd, WM_MOUSEMOVE, 0, lp, error) ||
                !dispatch(target.hwnd, downMsg, wDown, lp, error)) {
                return "{\"ok\":false,\"error\":\"" + error + "\"}";
            }
        } else {
            if (!dispatch(target.hwnd, upMsg, 0, lp, error)) {
                return "{\"ok\":false,\"error\":\"" + error + "\"}";
            }
        }
    }
    else if (a.type == "move") {
        UINT msg = target.nonClient ? WM_NCMOUSEMOVE : WM_MOUSEMOVE;
        WPARAM wp = target.nonClient ? (WPARAM)target.hitTest
                  : (a.held ? button_mk(a.button.empty() ? "left" : a.button) : 0);
        LPARAM lp = target.nonClient
            ? point_lparam(target.screen.x, target.screen.y)
            : point_lparam(target.client.x, target.client.y);
        if (!dispatch(target.hwnd, msg, wp, lp, error))
            return "{\"ok\":false,\"error\":\"" + error + "\"}";
    }
    else if (a.type == "drag") {
        if (a.dragPath.empty()) return "{\"ok\":false,\"error\":\"drag requires at least one point\"}";
        if (target.nonClient) return "{\"ok\":false,\"error\":\"background non-client drag is not supported\"}";
        UINT dM = (a.button=="right")?WM_RBUTTONDOWN:(a.button=="middle")?WM_MBUTTONDOWN:WM_LBUTTONDOWN;
        UINT uM = (a.button=="right")?WM_RBUTTONUP:(a.button=="middle")?WM_MBUTTONUP:WM_LBUTTONUP;
        WPARAM wD = (a.button=="right")?MK_RBUTTON:(a.button=="middle")?MK_MBUTTON:MK_LBUTTON;
        input_remember_keyboard_target(hWnd, target.hwnd);
        bool buttonDown = false;
        POINT lastClient = target.client;
        auto releaseAfterFailure = [&]() {
            if (!buttonDown) return;
            std::string releaseError;
            if (!dispatch(
                    target.hwnd, uM, 0, point_lparam(lastClient.x, lastClient.y), releaseError)) {
                LOG_ERROR("input", "background drag emergency release failed: %s",
                          releaseError.c_str());
            }
            buttonDown = false;
        };
        for (size_t i = 0; i < a.dragPath.size(); ++i) {
            POINT screen = {};
            if (!norm_to_screen_point(hWnd, a.dragPath[i].first, a.dragPath[i].second, screen) ||
                !ScreenToClient(target.hwnd, &screen)) {
                releaseAfterFailure();
                return "{\"ok\":false,\"error\":\"failed to map background drag point\"}";
            }
            lastClient = screen;
            LPARAM lp = point_lparam(screen.x, screen.y);
            if (i == 0) {
                if (!dispatch(target.hwnd, WM_MOUSEMOVE, 0, lp, error) ||
                    !dispatch(target.hwnd, dM, wD, lp, error)) {
                    releaseAfterFailure();
                    return "{\"ok\":false,\"error\":\"" + error + "\"}";
                }
                buttonDown = true;
            } else if (!dispatch(target.hwnd, WM_MOUSEMOVE, wD, lp, error)) {
                releaseAfterFailure();
                return "{\"ok\":false,\"error\":\"" + error + "\"}";
            }
            Sleep(5);
        }
        POINT end = {};
        if (!norm_to_screen_point(hWnd, a.dragPath.back().first, a.dragPath.back().second, end) ||
            !ScreenToClient(target.hwnd, &end) ||
            !dispatch(target.hwnd, uM, 0, point_lparam(end.x, end.y), error)) {
            releaseAfterFailure();
            if (error.empty()) error = "failed to release background drag";
            return "{\"ok\":false,\"error\":\"" + error + "\"}";
        }
        buttonDown = false;
    }
    else if (a.type == "wheel") {
        int delta = -a.delta;
        // WM_MOUSEWHEEL is the exception: lParam is always in screen coordinates.
        if (!dispatch(
                target.hwnd, WM_MOUSEWHEEL, MAKEWPARAM(0, (short)delta),
                point_lparam(target.screen.x, target.screen.y), error)) {
            return "{\"ok\":false,\"error\":\"" + error + "\"}";
        }
    }
    else if (a.type == "keydown" || a.type == "keyup" || a.type == "keypress") {
        if (a.vk == 0) return "{\"ok\":false,\"error\":\"key requires valid vk\"}";
        HWND keyHwnd = input_keyboard_target(hWnd);
        if (!keyHwnd) return "{\"ok\":false,\"error\":\"no keyboard target; click a mapped control first\"}";
        auto dk = [&](bool up) -> bool {
            UINT msg = up ? WM_KEYUP : WM_KEYDOWN;
            LPARAM lp = key_message_lparam((WORD)a.vk, up);
            return dispatch(keyHwnd, msg, (WPARAM)a.vk, lp, error);
        };
        if (a.type == "keypress") {
            if (!dk(false)) return "{\"ok\":false,\"error\":\"" + error + "\"}";
            Sleep(5);
            if (!dk(true)) return "{\"ok\":false,\"error\":\"" + error + "\"}";
        } else if (!dk(a.type == "keyup")) {
            return "{\"ok\":false,\"error\":\"" + error + "\"}";
        }
    }
    else if (a.type == "combo") {
        if (a.vk == 0) return "{\"ok\":false,\"error\":\"combo requires valid vk\"}";
        HWND keyHwnd = input_keyboard_target(hWnd);
        if (!keyHwnd) return "{\"ok\":false,\"error\":\"no keyboard target; click a mapped control first\"}";
        struct { bool ac; WORD vk; } m[] = {{a.ctrlKey,VK_CONTROL},{a.shiftKey,VK_SHIFT},{a.altKey,VK_MENU},{a.metaKey,VK_LWIN}};
        auto dk = [&](WORD kv, bool up) -> bool {
            UINT msg = up ? WM_KEYUP : WM_KEYDOWN;
            LPARAM lp = key_message_lparam(kv, up);
            return dispatch(keyHwnd, msg, (WPARAM)kv, lp, error);
        };
        for (auto& md : m) {
            if (md.ac && !dk(md.vk, false))
                return "{\"ok\":false,\"error\":\"" + error + "\"}";
        }
        if (!dk((WORD)a.vk, false)) return "{\"ok\":false,\"error\":\"" + error + "\"}";
        Sleep(5);
        if (!dk((WORD)a.vk, true)) return "{\"ok\":false,\"error\":\"" + error + "\"}";
        for (int i = 3; i >= 0; i--) {
            if (m[i].ac && !dk(m[i].vk, true))
                return "{\"ok\":false,\"error\":\"" + error + "\"}";
        }
    }
    else if (a.type == "text") {
        HWND keyHwnd = input_keyboard_target(hWnd);
        std::string terr = input_deliver_committed_text(keyHwnd, a.text, a.focus);
        if (!terr.empty()) return "{\"ok\":false,\"error\":\"" + terr + "\"}";
    }
    else { return "{\"ok\":false,\"error\":\"unknown type: " + a.type + "\"}"; }
    return "{\"ok\":true}";
}
