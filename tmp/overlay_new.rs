// ── Yellow highlight overlay ──────────────────────────
static OVERLAY_TARGET: Mutex<isize> = Mutex::new(0);
static OVERLAY_RUNNING: AtomicBool = AtomicBool::new(false);
static OVERLAY_BARS: Mutex<[isize; 4]> = Mutex::new([0, 0, 0, 0]);

const BORDER_W: i32 = 3;
const BORDER_INSET: i32 = 1;
const YELLOW_COLOR: u32 = 0x0000FFFFu32;

unsafe fn destroy_overlay_bars() {
    let mut bars = OVERLAY_BARS.lock().unwrap();
    for i in 0..4 {
        if bars[i] != 0 {
            let _ = windows::Win32::UI::WindowsAndMessaging::DestroyWindow(
                windows::Win32::Foundation::HWND(std::ptr::with_exposed_provenance_mut::<std::ffi::c_void>(bars[i] as usize)));
            bars[i] = 0;
        }
    }
}

unsafe fn reposition_overlay() {
    use windows::Win32::UI::WindowsAndMessaging::{
        SetWindowPos, GetWindowRect, IsWindow, IsIconic, IsWindowVisible,
        ShowWindow, SWP_NOACTIVATE, SW_HIDE, SW_SHOWNOACTIVATE, GetSystemMetrics,
        SM_XVIRTUALSCREEN, SM_YVIRTUALSCREEN, SM_CXVIRTUALSCREEN, SM_CYVIRTUALSCREEN,
    };
    use windows::Win32::Graphics::Gdi::{GetDC, ReleaseDC, CreateSolidBrush, FillRect, DeleteObject};
    use windows::Win32::Foundation::{RECT, HWND, COLORREF};

    let hwnd = *OVERLAY_TARGET.lock().unwrap();
    if hwnd == 0 { return; }
    let target = HWND(std::ptr::with_exposed_provenance_mut::<std::ffi::c_void>(hwnd as usize));

    if !IsWindow(target).as_bool() { destroy_overlay_bars(); OVERLAY_RUNNING.store(false, Ordering::Relaxed); return; }

    let bars = OVERLAY_BARS.lock().unwrap();
    let bar_hwnds = [bars[0], bars[1], bars[2], bars[3]];
    drop(bars);

    if IsIconic(target).as_bool() || !IsWindowVisible(target).as_bool() {
        for &b in &bar_hwnds { if b != 0 { let _ = ShowWindow(HWND(std::ptr::with_exposed_provenance_mut::<std::ffi::c_void>(b as usize)), SW_HIDE); } }
        return;
    }

    let mut r = RECT::default();
    if GetWindowRect(target, &mut r).is_err() { return; }
    let w = r.right - r.left; let h = r.bottom - r.top;
    if w <= BORDER_W * 2 || h <= BORDER_W * 2 { return; }

    let sx = GetSystemMetrics(SM_XVIRTUALSCREEN); let sy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    let sw = GetSystemMetrics(SM_CXVIRTUALSCREEN); let sh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    let bx = (r.left + BORDER_INSET).max(sx); let by = (r.top + BORDER_INSET).max(sy);
    let br = (r.left + w - BORDER_INSET).min(sx + sw); let bb = (r.top + h - BORDER_INSET).min(sy + sh);

    let positions = [
        (bx, by, br - bx, BORDER_W),
        (bx, bb - BORDER_W, br - bx, BORDER_W),
        (bx, by, BORDER_W, bb - by),
        (br - BORDER_W, by, BORDER_W, bb - by),
    ];
    for i in 0..4 {
        if bar_hwnds[i] != 0 {
            let bar_h = HWND(std::ptr::with_exposed_provenance_mut::<std::ffi::c_void>(bar_hwnds[i] as usize));
            let (px, py, pw, ph) = positions[i];
            let _ = SetWindowPos(bar_h, target, px, py, pw, ph, SWP_NOACTIVATE);
            let dc = GetDC(bar_h);
            if !dc.0.is_null() {
                let brush = CreateSolidBrush(COLORREF(YELLOW_COLOR));
                let fill_r = RECT { left: 0, top: 0, right: pw, bottom: ph };
                FillRect(dc, &fill_r, brush);
                let _ = DeleteObject(brush); let _ = ReleaseDC(bar_h, dc);
            }
            let _ = ShowWindow(bar_h, SW_SHOWNOACTIVATE);
        }
    }
}

unsafe fn create_overlay_bars(hwnd: isize) {
    use windows::Win32::UI::WindowsAndMessaging::{
        CreateWindowExW, ShowWindow, SetWindowPos, GetSystemMetrics,
        SWP_NOACTIVATE, SWP_NOMOVE, SWP_NOSIZE,
        WS_EX_TRANSPARENT, WS_EX_TOOLWINDOW, WS_POPUP,
        GetWindowRect, IsWindow,
        SM_XVIRTUALSCREEN, SM_YVIRTUALSCREEN, SM_CXVIRTUALSCREEN, SM_CYVIRTUALSCREEN,
    };
    use windows::Win32::Graphics::Gdi::{GetDC, ReleaseDC, CreateSolidBrush, FillRect, DeleteObject};
    use windows::Win32::Foundation::{RECT, HWND, COLORREF};

    destroy_overlay_bars();
    if hwnd == 0 { OVERLAY_RUNNING.store(false, Ordering::Relaxed); return; }

    let target = HWND(std::ptr::with_exposed_provenance_mut::<std::ffi::c_void>(hwnd as usize));
    if !IsWindow(target).as_bool() { OVERLAY_RUNNING.store(false, Ordering::Relaxed); return; }

    let mut r = RECT::default();
    if GetWindowRect(target, &mut r).is_err() { return; }
    let w = r.right - r.left; let h = r.bottom - r.top;
    if w <= BORDER_W * 2 || h <= BORDER_W * 2 { return; }

    let sx = GetSystemMetrics(SM_XVIRTUALSCREEN); let sy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    let sw = GetSystemMetrics(SM_CXVIRTUALSCREEN); let sh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    let bx = (r.left + BORDER_INSET).max(sx); let by = (r.top + BORDER_INSET).max(sy);
    let br = (r.left + w - BORDER_INSET).min(sx + sw); let bb = (r.top + h - BORDER_INSET).min(sy + sh);

    let create_bar = |cx: i32, cy: i32, cw: i32, ch: i32| -> isize {
        match CreateWindowExW(
            WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW,
            windows::core::w!("STATIC"), windows::core::w!(""), WS_POPUP,
            cx, cy, cw.max(1), ch.max(1), None, None, None, None,
        ) {
            Ok(h) => {
                let dc = GetDC(h);
                if !dc.0.is_null() {
                    let brush = CreateSolidBrush(COLORREF(YELLOW_COLOR));
                    FillRect(dc, &RECT{left:0,top:0,right:cw,bottom:ch}, brush);
                    let _ = DeleteObject(brush); let _ = ReleaseDC(h, dc);
                }
                let _ = ShowWindow(h, SW_SHOWNOACTIVATE);
                let _ = SetWindowPos(h, target, 0, 0, 0, 0, SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
                std::mem::transmute_copy(&h)
            }
            Err(_) => 0,
        }
    };

    let mut bars = OVERLAY_BARS.lock().unwrap();
    bars[0] = create_bar(bx, by, br - bx, BORDER_W);
    bars[1] = create_bar(bx, bb - BORDER_W, br - bx, BORDER_W);
    bars[2] = create_bar(bx, by, BORDER_W, bb - by);
    bars[3] = create_bar(br - BORDER_W, by, BORDER_W, bb - by);
    *OVERLAY_TARGET.lock().unwrap() = hwnd;
    OVERLAY_RUNNING.store(true, Ordering::Relaxed);
}

fn start_overlay_tracker() {
    std::thread::spawn(|| unsafe {
        use windows::Win32::UI::Accessibility::{
            SetWinEventHook, UnhookWinEvent, HWINEVENTHOOK,
        };
        use windows::Win32::UI::WindowsAndMessaging::{
            GetMessageW, DispatchMessageW, MSG,
            EVENT_SYSTEM_MOVESIZEEND, EVENT_OBJECT_LOCATIONCHANGE,
            WINEVENT_OUTOFCONTEXT,
        };
        let hook: HWINEVENTHOOK = SetWinEventHook(
            EVENT_SYSTEM_MOVESIZEEND, EVENT_OBJECT_LOCATIONCHANGE,
            None, Some(overlay_win_event_proc), 0, 0, WINEVENT_OUTOFCONTEXT,
        ).unwrap_or(HWINEVENTHOOK(std::ptr::null_mut()));
        let mut msg = MSG::default();
        while GetMessageW(&mut msg, None, 0, 0).as_bool() { let _ = DispatchMessageW(&msg); }
        if !hook.0.is_null() { let _ = UnhookWinEvent(hook); }
    });
}

unsafe extern "system" fn overlay_win_event_proc(
    _hook: windows::Win32::UI::Accessibility::HWINEVENTHOOK, _event: u32,
    hwnd: windows::Win32::Foundation::HWND, _id_object: i32, _id_child: i32,
    _event_thread: u32, _event_time: u32,
) {
    let target = *OVERLAY_TARGET.lock().unwrap();
    if target != 0 && hwnd.0 as isize == target { reposition_overlay(); }
}

#[tauri::command]
fn highlight_window(hwnd: u64) {
    unsafe { create_overlay_bars(hwnd as isize); }
    dlog!("highlight: hwnd={}", hwnd);
}

static STREAM: Mutex<Option<StreamState>> = Mutex::new(None);