#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

use std::process::{Command, Child, Stdio};
use std::os::windows::process::CommandExt;
use std::fs::OpenOptions;
use std::io::{Write, BufReader, BufRead, Read};
use std::sync::Mutex;
use std::thread;
use serde::Serialize;
use tauri::Emitter;
use std::time::Instant;
use windows::Win32::UI::WindowsAndMessaging::{
    EnumWindows, GetWindowTextW, GetWindowLongPtrW, IsWindowVisible,
    GetDesktopWindow, GetWindow, GetWindowRect, GetSystemMetrics,
    IsIconic, IsWindow, GW_OWNER, GWL_STYLE, GWL_EXSTYLE, WS_CAPTION, WS_EX_TOOLWINDOW,
    SM_CXSCREEN, SM_CYSCREEN,
};
use windows::Win32::Foundation::{RECT, HWND, BOOL, TRUE, LPARAM};
use windows::Win32::Graphics::Gdi::{
    GetDC, GetWindowDC, CreateCompatibleDC, CreateCompatibleBitmap,
    SelectObject, BitBlt, GetDIBits, DeleteDC, DeleteObject, ReleaseDC,
    BITMAPINFOHEADER, BITMAPINFO, SRCCOPY, DIB_RGB_COLORS, BI_RGB,
    HDC, HBRUSH,
};

mod fmp4;

// ── Session-based debug logging ──
// Each launch creates a new log file: agent_20260704_174500.log, max 5 kept
static LOG_FILE: Mutex<Option<std::fs::File>> = Mutex::new(None);

macro_rules! dlog {
    ($($arg:tt)*) => {
        if let Ok(mut guard) = $crate::LOG_FILE.lock() {
            if let Some(ref mut f) = *guard {
                let _ = writeln!(f, "[{}] {}", chrono::Local::now().format("%H:%M:%S%.3f"), format!($($arg)*));
                let _ = f.flush(); // flush immediately so crash doesn't lose log
            }
        }
    }
}

fn init_log(max_logs: usize) {
    let log_dir = std::env::current_exe()
        .map(|p| p.parent().unwrap_or(&p).to_path_buf())
        .unwrap_or_else(|_| std::path::PathBuf::from("."));

    // Create per-session log filename with timestamp
    let ts = chrono::Local::now().format("%Y%m%d_%H%M%S");
    let log_path = log_dir.join(format!("agent_{}.log", ts));

    // Clean old logs: keep only the newest max_logs-1 (plus current = max_logs total)
    if let Ok(entries) = std::fs::read_dir(&log_dir) {
        let mut log_files: Vec<_> = entries
            .filter_map(|e| e.ok())
            .filter(|e| e.file_name().to_string_lossy().starts_with("agent_") && e.file_name().to_string_lossy().ends_with(".log"))
            .collect();
        log_files.sort_by_key(|e| e.metadata().map(|m| m.modified().unwrap_or(std::time::SystemTime::UNIX_EPOCH)).unwrap_or(std::time::SystemTime::UNIX_EPOCH));
        while log_files.len() >= max_logs {
            if let Some(old) = log_files.first() {
                let _ = std::fs::remove_file(old.path());
                log_files.remove(0);
            }
        }
    }

    match OpenOptions::new().create(true).append(true).open(&log_path) {
        Ok(f) => {
            let _ = writeln!(&f, "=== Game Agent Monitor v0.1.0 ===");
            let _ = writeln!(&f, "Session: {} | PID: {}", ts, std::process::id());
            let _ = writeln!(&f, "Log: {}", log_path.display());
            *LOG_FILE.lock().unwrap() = Some(f);
        }
        Err(_) => {}
    }
}

#[derive(Clone, Serialize)]
struct WindowInfo { title: String, category: String, hwnd: u64 }

#[tauri::command]
fn list_processes() -> Vec<WindowInfo> {
    let t0 = Instant::now();
    let exe = std::env::current_exe()
        .map(|p| p.parent().unwrap_or(&p).join("..").join("..").join("..").join("..").join("capture").join("build").join("process_list.exe"))
        .unwrap_or_else(|_| "capture/build/process_list.exe".into());
    let exe = std::fs::canonicalize(&exe).unwrap_or(exe);

    dlog!("list_processes: calling {}", exe.display());
    match Command::new(&exe).output() {
        Ok(out) => {
            let stderr = String::from_utf8_lossy(&out.stderr);
            if !stderr.is_empty() { dlog!("  stderr: {}", stderr.trim()); }
            let result: Vec<_> = String::from_utf8_lossy(&out.stdout).lines().filter_map(|line| {
                let line = line.trim(); if line.is_empty() { return None; }
                let title = extract_json_str(line, "title").unwrap_or("Unknown");
                let hwnd = extract_json_str(line, "hwnd").and_then(|s| s.parse().ok()).unwrap_or(0);
                Some(WindowInfo { title: title.to_string(), category: "process".into(), hwnd })
            }).collect();
            dlog!("list_processes: {} entries in {:.0}ms", result.len(), t0.elapsed().as_millis());
            result
        }
        Err(e) => {
            dlog!("list_processes: ERROR {}", e);
            vec![]
        }
    }
}

#[tauri::command]
fn list_windows() -> Vec<WindowInfo> {
    let t0 = Instant::now();
    let mut result = Vec::new();

    // Desktop always first
    result.push(WindowInfo {
        title: " Entire Desktop".into(),
        category: "desktop".into(),
        hwnd: 0,
    });

    // Enumerate taskbar-visible windows via Win32 API (no subprocess)
    unsafe {
        let _ = EnumWindows(Some(enum_window_callback), LPARAM(&mut result as *mut _ as isize));
    }

    dlog!("list_windows: {} entries in {:.0}ms", result.len(), t0.elapsed().as_millis());
    result
}

unsafe extern "system" fn enum_window_callback(hwnd: HWND, lparam: LPARAM) -> BOOL {
    let result = &mut *(lparam.0 as *mut Vec<WindowInfo>);

    if !IsWindowVisible(hwnd).as_bool() { return TRUE; }

    let style = GetWindowLongPtrW(hwnd, GWL_STYLE) as u32;
    let ex_style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE) as u32;

    // Must have title bar, not a tool window
    if style & (WS_CAPTION.0 as u32) == 0 { return TRUE; }
    if ex_style & (WS_EX_TOOLWINDOW.0 as u32) != 0 { return TRUE; }

    // Not cloaked
    let mut cloaked: u32 = 0;
    windows::Win32::Graphics::Dwm::DwmGetWindowAttribute(
        hwnd,
        windows::Win32::Graphics::Dwm::DWMWA_CLOAKED,
        &mut cloaked as *mut _ as *mut _,
        std::mem::size_of::<u32>() as u32,
    ).ok();
    if cloaked != 0 { return TRUE; }

    // Has non-zero size
    let mut rect = RECT::default();
    if GetWindowRect(hwnd, &mut rect).is_err() { return TRUE; }
    if rect.right - rect.left <= 0 || rect.bottom - rect.top <= 0 { return TRUE; }

    // No owner window (GetWindow returns Result<HWND>)
    if let Ok(owner) = GetWindow(hwnd, GW_OWNER) {
        if !owner.0.is_null() { return TRUE; }
    }

    // Get title
    let mut buf = [0u16; 256];
    let len = GetWindowTextW(hwnd, &mut buf);
    if len == 0 { return TRUE; }
    let title = String::from_utf16_lossy(&buf[..len as usize]);
    let title = title.trim();
    if title.is_empty() || title == "Program Manager" { return TRUE; }

    result.push(WindowInfo {
        title: title.to_string(),
        category: "window".into(),
        hwnd: hwnd.0 as u64,
    });

    TRUE
}

fn extract_json_str<'a>(line: &'a str, key: &str) -> Option<&'a str> {
    let search = format!("\"{}\":\"", key);
    let start = line.find(&search)? + search.len();
    // Find the closing unescaped quote using char indices
    let rest = &line[start..];
    let mut prev_backslash = false;
    for (i, c) in rest.char_indices() {
        if c == '\\' { prev_backslash = true; continue; }
        if c == '"' && !prev_backslash { return Some(&rest[..i]); }
        prev_backslash = false;
    }
    Some(rest)
}

// ── Screenshot: multi-method capture with diagnostics ──

// PrintWindow — not in windows crate, use raw FFI
const PW_RENDERFULLCONTENT: u32 = 0x00000002;
const PW_CLIENTONLY: u32 = 0x00000001;
extern "system" {
    fn PrintWindow(hwnd: HWND, hdc: HDC, flags: u32) -> BOOL;
    fn FillRect(hdc: HDC, lprc: *const RECT, hbr: HBRUSH) -> i32;
    fn CreateSolidBrush(color: u32) -> HBRUSH;
}

struct CaptureResult {
    pixels: Vec<u8>, w: i32, h: i32,
    method: &'static str,    // "GDI(GetWindowDC)", "PrintWindow", "ScreenBitBlt", "ALL_FAILED"
    window_state: &'static str, // "normal", "minimized", "hidden", "desktop", "zero-size"
}

fn is_solid_color(pixels: &[u8]) -> bool {
    if pixels.len() < 16 { return pixels.len() < 4; }
    let step = (pixels.len() / 400).max(4);
    let (r0, g0, b0) = (pixels[2], pixels[1], pixels[0]);
    let mut same = 0usize; let mut n = 0usize;
    for i in (0..pixels.len()).step_by(step * 4) { n += 1;
        if pixels[i+2]==r0 && pixels[i+1]==g0 && pixels[i]==b0 { same+=1; } }
    n > 0 && same == n
}

/// Check for magenta sentinel pixels (R=255, G=0, B=255).
/// PrintWindow fills with magenta before drawing — if more than 5% of samples
/// are still magenta, PrintWindow likely failed to render the window content.
fn has_magenta_sentinel(pixels: &[u8]) -> bool {
    if pixels.len() < 16 { return false; }
    let step = (pixels.len() / 200).max(4);
    let mut magenta = 0usize; let mut n = 0usize;
    for i in (0..pixels.len()).step_by(step * 4) { n += 1;
        if pixels[i+2] == 255 && pixels[i+1] == 0 && pixels[i] == 255 { magenta += 1; } }
    // If >5% of samples are magenta, PrintWindow failed
    n > 0 && magenta * 20 > n
}

unsafe fn bitblt_bgra(dc: HDC, src_dc: HDC, w: i32, h: i32) -> Option<(Vec<u8>, i32, i32)> {
    let mem_dc = CreateCompatibleDC(dc);
    if mem_dc.0.is_null() { return None; }
    let bitmap = CreateCompatibleBitmap(dc, w, h);
    if bitmap.0.is_null() { let _ = DeleteDC(mem_dc); return None; }
    let old_bmp = SelectObject(mem_dc, bitmap);
    if BitBlt(mem_dc, 0, 0, w, h, src_dc, 0, 0, SRCCOPY).is_err() {
        let _ = SelectObject(mem_dc, old_bmp); let _ = DeleteObject(bitmap); let _ = DeleteDC(mem_dc); return None; }
    let mut bi = BITMAPINFOHEADER::default();
    bi.biSize = std::mem::size_of::<BITMAPINFOHEADER>() as u32;
    bi.biWidth=w; bi.biHeight=-h; bi.biPlanes=1; bi.biBitCount=32; bi.biCompression=BI_RGB.0 as u32;
    let mut pixels = vec![0u8; (w * h * 4) as usize];
    let copied = GetDIBits(mem_dc, bitmap, 0, h as u32, Some(pixels.as_mut_ptr() as *mut _),
        &mut bi as *mut _ as *mut BITMAPINFO, DIB_RGB_COLORS);
    let _ = SelectObject(mem_dc, old_bmp); let _ = DeleteObject(bitmap); let _ = DeleteDC(mem_dc);
    if copied == 0 { None } else { Some((pixels, w, h)) }
}

unsafe fn capture_window_internal(hwnd: u64) -> CaptureResult {
    let hwnd_ptr = HWND(std::ptr::with_exposed_provenance_mut::<std::ffi::c_void>(hwnd as usize));
    let is_desktop = hwnd_ptr.0.is_null() || hwnd_ptr == GetDesktopWindow();
    let def = CaptureResult { pixels: vec![], w: 0, h: 0, method: "None", window_state: "error" };

    // Desktop: simple GDI BitBlt
    if is_desktop {
        let dc = GetDC(None); if dc.0.is_null() { return def; }
        let w=GetSystemMetrics(SM_CXSCREEN); let h=GetSystemMetrics(SM_CYSCREEN);
        if w<=0||h<=0 { let _=ReleaseDC(None, dc); return def; }
        let r = bitblt_bgra(dc, dc, w, h);
        let _=ReleaseDC(None, dc);
        return match r {
            Some((p,pw,ph)) => CaptureResult { pixels:p, w:pw, h:ph, method:"GDI", window_state:"desktop" },
            None => def
        };
    }

    // Window: detect state
    let mut state = "normal";
    if !IsWindow(hwnd_ptr).as_bool() {
        return CaptureResult{window_state:"closed",..def};
    }
    if IsIconic(hwnd_ptr).as_bool() { state = "minimized"; }
    else if !IsWindowVisible(hwnd_ptr).as_bool() { state = "hidden"; }
    let mut wr = RECT::default();
    if GetWindowRect(hwnd_ptr, &mut wr).is_err() { return CaptureResult{window_state:"no-rect",..def}; }
    let ww=wr.right-wr.left; let wh=wr.bottom-wr.top;
    if ww<=0||wh<=0 { return CaptureResult{window_state:"zero-size",..def}; }

    // Method 1: GetWindowDC + BitBlt
    let dc=GetWindowDC(hwnd_ptr);
    if !dc.0.is_null() {
        if let Some((p,pw,ph)) = bitblt_bgra(dc, dc, ww, wh) {
            if is_solid_color(&p) {
                dlog!("capture: GetWindowDC → solid({},{},{})", p[2], p[1], p[0]);
            } else {
                let _=ReleaseDC(hwnd_ptr, dc);
                return CaptureResult{pixels:p,w:pw,h:ph,method:"GDI(GetWindowDC)",window_state:state};
            }
        } else { dlog!("capture: GetWindowDC bitblt failed"); }
        let _=ReleaseDC(hwnd_ptr, dc);
    } else { dlog!("capture: GetWindowDC returned null"); }

    // Method 2: PrintWindow
    let sdc=GetDC(None);
    if !sdc.0.is_null() {
        let mdc=CreateCompatibleDC(sdc);
        if !mdc.0.is_null() {
            let bmp=CreateCompatibleBitmap(sdc,ww,wh);
            if !bmp.0.is_null() {
                let old=SelectObject(mdc,bmp);
                let fill_r = RECT{left:0,top:0,right:ww,bottom:wh};
                let brush=CreateSolidBrush(0x00FF00FF); // magenta sentinel
                if !brush.0.is_null() { FillRect(mdc,&fill_r,brush); let _=DeleteObject(brush); }
                let pw_ok = PrintWindow(hwnd_ptr,mdc,PW_RENDERFULLCONTENT|PW_CLIENTONLY).as_bool();
                let mut bi=BITMAPINFOHEADER::default();
                bi.biSize=std::mem::size_of::<BITMAPINFOHEADER>() as u32;
                bi.biWidth=ww; bi.biHeight=-wh; bi.biPlanes=1; bi.biBitCount=32; bi.biCompression=BI_RGB.0 as u32;
                let mut pix=vec![0u8;(ww*wh*4)as usize];
                let copied=GetDIBits(mdc,bmp,0,wh as u32,Some(pix.as_mut_ptr() as *mut _),
                    &mut bi as *mut _ as *mut BITMAPINFO, DIB_RGB_COLORS);
                let _=SelectObject(mdc,old); let _=DeleteObject(bmp);
                if copied == 0 { dlog!("capture: PrintWindow GetDIBits failed"); }
                else if !pw_ok { dlog!("capture: PrintWindow returned FALSE"); }
                else if is_solid_color(&pix) { dlog!("capture: PrintWindow → solid({},{},{})", pix[2], pix[1], pix[0]); }
                else if has_magenta_sentinel(&pix) { dlog!("capture: PrintWindow → magenta sentinel detected"); }
                else {
                    let _=DeleteDC(mdc); let _=ReleaseDC(None,sdc);
                    let m = if state=="minimized" {"PrintWindow(minimized)"} else {"PrintWindow"};
                    return CaptureResult{pixels:pix,w:ww,h:wh,method:m,window_state:state}; }
            }
            let _=DeleteDC(mdc);
        }
        let _=ReleaseDC(None,sdc);
    }

    // Method 3: BitBlt from screen DC at window screen position
    let sc=GetDC(None);
    if !sc.0.is_null() {
        // BitBlt from screen coordinates (wr.left, wr.top) — not (0,0)
        let mem_dc = CreateCompatibleDC(sc);
        if !mem_dc.0.is_null() {
            let bmp = CreateCompatibleBitmap(sc, ww, wh);
            if !bmp.0.is_null() {
                let old = SelectObject(mem_dc, bmp);
                if BitBlt(mem_dc, 0, 0, ww, wh, sc, wr.left, wr.top, SRCCOPY).is_ok() {
                    let mut bi = BITMAPINFOHEADER::default();
                    bi.biSize = std::mem::size_of::<BITMAPINFOHEADER>() as u32;
                    bi.biWidth=ww; bi.biHeight=-wh; bi.biPlanes=1; bi.biBitCount=32; bi.biCompression=BI_RGB.0 as u32;
                    let mut pix = vec![0u8;(ww*wh*4)as usize];
                    let copied = GetDIBits(mem_dc, bmp, 0, wh as u32, Some(pix.as_mut_ptr() as *mut _),
                        &mut bi as *mut _ as *mut BITMAPINFO, DIB_RGB_COLORS);
                    if copied != 0 && !is_solid_color(&pix) {
                        let _=SelectObject(mem_dc,old); let _=DeleteObject(bmp); let _=DeleteDC(mem_dc);
                        let _=ReleaseDC(None,sc);
                        return CaptureResult{pixels:pix,w:ww,h:wh,method:"ScreenBitBlt",window_state:state};
                    } else if copied != 0 {
                        dlog!("capture: ScreenBitBlt → solid({},{},{})", pix[2], pix[1], pix[0]);
                    }
                }
                let _=SelectObject(mem_dc,old); let _=DeleteObject(bmp);
            }
            let _=DeleteDC(mem_dc);
        }
        let _=ReleaseDC(None,sc);
    } else { dlog!("capture: ScreenBitBlt GetDC(null) failed"); }

    dlog!("capture: ALL methods failed for hwnd={} state={} {}x{}", hwnd, state, ww, wh);
    CaptureResult{pixels:vec![],w:ww,h:wh,method:"ALL_FAILED",window_state:state}
}

/// Scale BGRA → max 640px wide, BGRA→RGBA, PNG encode, base64
fn pixels_to_png_base64(pixels: &[u8], w: i32, h: i32) -> String {
    let t0 = Instant::now();
    let scale = (640.0 / w as f32).min(1.0);
    let sw = (w as f32 * scale) as i32;
    let sh = (h as f32 * scale) as i32;
    let mut rgba = vec![0u8; (sw * sh * 4) as usize];
    for y in 0..sh { let sy = (y as f32 / scale) as usize;
        for x in 0..sw { let sx = (x as f32 / scale) as usize;
            let di = (y * sw + x) as usize * 4; let si = (sy * w as usize + sx) * 4;
            rgba[di]=pixels[si+2]; rgba[di+1]=pixels[si+1]; rgba[di+2]=pixels[si]; rgba[di+3]=255; } }
    let scale_ms = t0.elapsed().as_millis();
    let png_t0 = Instant::now();
    let mut out = Vec::new();
    out.extend_from_slice(&[137,80,78,71,13,10,26,10]);
    let mut ihdr = Vec::new();
    ihdr.extend_from_slice(&(sw as u32).to_be_bytes()); ihdr.extend_from_slice(&(sh as u32).to_be_bytes());
    ihdr.extend_from_slice(&[8,6,0,0,0]); write_png_chunk(&mut out, b"IHDR", &ihdr);
    let mut raw = Vec::new();
    for y in 0..sh { raw.push(0); raw.extend_from_slice(&rgba[(y*sw)as usize*4..(y*sw+sw)as usize*4]); }
    let compressed = miniz_oxide::deflate::compress_to_vec_zlib(&raw, 6);
    write_png_chunk(&mut out, b"IDAT", &compressed); write_png_chunk(&mut out, b"IEND", &[]);
    let png_ms = png_t0.elapsed().as_millis();
    let b64_t0 = Instant::now(); let b64 = base64_encode(&out); let b64_ms = b64_t0.elapsed().as_millis();
    dlog!("  scale={}ms PNG={}ms b64={}ms", scale_ms, png_ms, b64_ms);
    b64
}

#[tauri::command]
fn capture_single() -> String {
    let t0 = Instant::now();
    dlog!("capture_single: desktop...");
    unsafe {
        let result = capture_window_internal(0);
        let screen_w = GetSystemMetrics(SM_CXSCREEN);
        let screen_h = GetSystemMetrics(SM_CYSCREEN);
        if !result.pixels.is_empty() {
            let b64 = pixels_to_png_base64(&result.pixels, result.w, result.h);
            dlog!("capture_single: {}x{} method={} → {}b, total={:.0}ms",
                result.w, result.h, result.method, b64.len(), t0.elapsed().as_millis());
            serde_json::json!({
                "image": b64,
                "w": result.w, "h": result.h,
                "x": 0, "y": 0,
                "screen_w": screen_w, "screen_h": screen_h,
                "method": result.method
            }).to_string()
        } else {
            dlog!("capture_single: FAILED method={} state={}", result.method, result.window_state);
            String::new()
        }
    }
}

#[tauri::command]
fn capture_window(hwnd: u64) -> String {
    let t0 = Instant::now();
    dlog!("capture_window: hwnd={}...", hwnd);
    unsafe {
        let result = capture_window_internal(hwnd);
        let screen_w = GetSystemMetrics(SM_CXSCREEN);
        let screen_h = GetSystemMetrics(SM_CYSCREEN);

        // Get window screen position for proportional display
        let (wx, wy) = if hwnd != 0 {
            let hwnd_ptr = HWND(std::ptr::with_exposed_provenance_mut::<std::ffi::c_void>(hwnd as usize));
            let mut wr = RECT::default();
            if GetWindowRect(hwnd_ptr, &mut wr).is_ok() {
                (wr.left, wr.top)
            } else { (0, 0) }
        } else { (0, 0) };

        if !result.pixels.is_empty() {
            let b64 = pixels_to_png_base64(&result.pixels, result.w, result.h);
            dlog!("capture_window: {}x{} @({},{}) screen={}x{} method={} → {}b, total={:.0}ms",
                result.w, result.h, wx, wy, screen_w, screen_h, result.method, b64.len(), t0.elapsed().as_millis());
            serde_json::json!({
                "image": b64,
                "w": result.w, "h": result.h,
                "x": wx, "y": wy,
                "screen_w": screen_w, "screen_h": screen_h,
                "method": result.method
            }).to_string()
        } else {
            dlog!("capture_window: FAILED method={} state={} w={} h={}",
                result.method, result.window_state, result.w, result.h);
            String::new()
        }
    }
}

/// PNG encode RGBA pixels (already BGRA→RGBA converted, no scaling)
fn encode_png_rgba(rgba: &[u8], w: i32, h: i32) -> Vec<u8> {
    let mut out = Vec::new();
    out.extend_from_slice(&[137, 80, 78, 71, 13, 10, 26, 10]);
    let mut ihdr = Vec::new();
    ihdr.extend_from_slice(&(w as u32).to_be_bytes());
    ihdr.extend_from_slice(&(h as u32).to_be_bytes());
    ihdr.extend_from_slice(&[8, 6, 0, 0, 0]);
    write_png_chunk(&mut out, b"IHDR", &ihdr);

    let mut raw = Vec::new();
    for y in 0..h {
        raw.push(0);
        let row = (y * w) as usize * 4;
        raw.extend_from_slice(&rgba[row..row + w as usize * 4]);
    }
    let compressed = miniz_oxide::deflate::compress_to_vec_zlib(&raw, 6);
    write_png_chunk(&mut out, b"IDAT", &compressed);
    write_png_chunk(&mut out, b"IEND", &[]);
    out
}

// ── PNG helpers ────────────────────────────────────────
fn write_png_chunk(out: &mut Vec<u8>, name: &[u8; 4], data: &[u8]) {
    out.extend_from_slice(&(data.len() as u32).to_be_bytes());
    out.extend_from_slice(name);
    out.extend_from_slice(data);
    let crc = crc32(name, data);
    out.extend_from_slice(&crc.to_be_bytes());
}

fn crc32(name: &[u8; 4], data: &[u8]) -> u32 {
    let mut crc: u32 = 0xFFFFFFFF;
    for &b in name.iter().chain(data) {
        crc ^= b as u32;
        for _ in 0..8 { crc = if crc & 1 != 0 { (crc >> 1) ^ 0xEDB88320 } else { crc >> 1 }; }
    }
    !crc
}

fn base64_encode(data: &[u8]) -> String {
    const CHARS: &[u8] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let mut out = String::new();
    for chunk in data.chunks(3) {
        let b0 = chunk[0] as u32;
        let b1 = if chunk.len() > 1 { chunk[1] as u32 } else { 0 };
        let b2 = if chunk.len() > 2 { chunk[2] as u32 } else { 0 };
        let n = (b0 << 16) | (b1 << 8) | b2;
        out.push(CHARS[((n >> 18) & 0x3F) as usize] as char);
        out.push(CHARS[((n >> 12) & 0x3F) as usize] as char);
        out.push(if chunk.len() > 1 { CHARS[((n >> 6) & 0x3F) as usize] } else { b'=' } as char);
        out.push(if chunk.len() > 2 { CHARS[(n & 0x3F) as usize] } else { b'=' } as char);
    }
    out
}

// ── Capture Stream (Rust-native, multi-method, BMP → frontend <img>) ──

use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::net::{TcpListener, TcpStream};

struct StreamState {
    running: Arc<AtomicBool>,
    _tcp_thread: Option<thread::JoinHandle<()>>,
}

static STREAM: Mutex<Option<StreamState>> = Mutex::new(None);
static STREAM_FRAME: Mutex<(String, i32, i32, String)> = Mutex::new((String::new(), 0, 0, String::new())); // b64, w, h, method
// Raw BGRA pixels for TCP/pipe clients (scaled, uncompressed)
static RAW_FRAME: Mutex<(Vec<u8>, i32, i32)> = Mutex::new((Vec::new(), 0, 0));

/// Write a binary frame [w:4][h:4][ch:4][size:4][pixels] to a writer
fn write_frame(w: &mut dyn Write, pixels: &[u8], w_val: i32, h_val: i32) -> std::io::Result<()> {
    w.write_all(&(w_val as u32).to_le_bytes())?;
    w.write_all(&(h_val as u32).to_le_bytes())?;
    w.write_all(&4u32.to_le_bytes())?;
    w.write_all(&(pixels.len() as u32).to_le_bytes())?;
    w.write_all(pixels)?;
    w.flush()
}

/// TCP broadcast thread: accepts clients, broadcasts latest frame to all
fn tcp_broadcast_thread(
    port: u16,
    running: Arc<AtomicBool>,
) {
    let listener = match TcpListener::bind(("127.0.0.1", port)) {
        Ok(l) => l,
        Err(e) => { dlog!("tcp: bind :{} failed: {}", port, e); return; }
    };
    // Non-blocking accept
    let _ = listener.set_nonblocking(true);
    dlog!("tcp: listening on 127.0.0.1:{}", port);

    let mut clients: Vec<TcpStream> = Vec::new();

    while running.load(Ordering::Relaxed) {
        // Accept new connections
        match listener.accept() {
            Ok((stream, addr)) => {
                let _ = stream.set_nonblocking(true);
                dlog!("tcp: client connected from {}", addr);
                clients.push(stream);
            }
            Err(ref e) if e.kind() == std::io::ErrorKind::WouldBlock => {}
            Err(_) => {}
        }

        // Get latest frame
        if let Ok(guard) = RAW_FRAME.lock() {
            let (ref pixels, w, h) = *guard;
            if !pixels.is_empty() {
                // Broadcast to all clients
                clients.retain_mut(|client| {
                    if write_frame(client, pixels, w, h).is_err() {
                        // Client disconnected
                        false
                    } else {
                        true
                    }
                });
            }
        }

        std::thread::sleep(std::time::Duration::from_millis(1));
    }

    dlog!("tcp: broadcast thread exiting");
}


/// Fast capture using a specific method (no fallback chain)
unsafe fn capture_fast(method: &str, hwnd_ptr: HWND, w: i32, h: i32) -> Option<(Vec<u8>, i32, i32)> {
    // Quick check: window still alive?
    if !hwnd_ptr.0.is_null() && hwnd_ptr != GetDesktopWindow() && !IsWindow(hwnd_ptr).as_bool() {
        return None;
    }
    if w <= 0 || h <= 0 { return None; }
    match method {
        "GDI" | "GDI(GetWindowDC)" => {
            let dc = GetWindowDC(hwnd_ptr);
            if dc.0.is_null() { return None; }
            let r = bitblt_bgra(dc, dc, w, h);
            let _ = ReleaseDC(hwnd_ptr, dc);
            r
        }
        "PrintWindow" | "PrintWindow(minimized)" => {
            let sdc = GetDC(None);
            if sdc.0.is_null() { return None; }
            let mdc = CreateCompatibleDC(sdc);
            if mdc.0.is_null() { let _ = ReleaseDC(None, sdc); return None; }
            let bmp = CreateCompatibleBitmap(sdc, w, h);
            if bmp.0.is_null() { let _ = DeleteDC(mdc); let _ = ReleaseDC(None, sdc); return None; }
            let old = SelectObject(mdc, bmp);
            let _ = PrintWindow(hwnd_ptr, mdc, PW_RENDERFULLCONTENT | PW_CLIENTONLY);
            let mut bi = BITMAPINFOHEADER::default();
            bi.biSize = std::mem::size_of::<BITMAPINFOHEADER>() as u32;
            bi.biWidth=w; bi.biHeight=-h; bi.biPlanes=1; bi.biBitCount=32; bi.biCompression=BI_RGB.0 as u32;
            let mut pix = vec![0u8;(w*h*4)as usize];
            let copied = GetDIBits(mdc,bmp,0,h as u32,Some(pix.as_mut_ptr() as *mut _),
                &mut bi as *mut _ as *mut BITMAPINFO, DIB_RGB_COLORS);
            let _=SelectObject(mdc,old); let _=DeleteObject(bmp); let _=DeleteDC(mdc); let _=ReleaseDC(None,sdc);
            if copied != 0 && !is_solid_color(&pix) { Some((pix, w, h)) } else { None }
        }
        "ScreenBitBlt" => {
            let sc = GetDC(None);
            if sc.0.is_null() { return None; }
            let r = bitblt_bgra(sc, sc, w, h);
            let _ = ReleaseDC(None, sc);
            r
        }
        _ => {
            // Desktop: simple GDI
            let dc = GetDC(None);
            if dc.0.is_null() { return None; }
            let r = bitblt_bgra(dc, dc, w, h);
            let _ = ReleaseDC(None, dc);
            r
        }
    }
}

/// BGRA pixels → BMP data URI (base64). Zero-compression, browser-native decode.
fn bgra_to_bmp_uri(pixels: &[u8], w: i32, h: i32) -> String {
    let row_size = ((w * 3 + 3) / 4) * 4;
    let img_size = (row_size * h) as usize;
    let file_size = 54 + img_size;
    let mut bmp = Vec::with_capacity(file_size);
    bmp.extend_from_slice(b"BM"); bmp.extend_from_slice(&(file_size as u32).to_le_bytes());
    bmp.extend_from_slice(&[0u8;4]); bmp.extend_from_slice(&54u32.to_le_bytes());
    bmp.extend_from_slice(&40u32.to_le_bytes());
    bmp.extend_from_slice(&(w as i32).to_le_bytes()); bmp.extend_from_slice(&(-h as i32).to_le_bytes());
    bmp.extend_from_slice(&1u16.to_le_bytes()); bmp.extend_from_slice(&24u16.to_le_bytes());
    bmp.extend_from_slice(&[0u8;24]);
    for y in 0..h as usize {
        let rs = y * w as usize * 4;
        for x in 0..w as usize {
            let si = rs + x * 4;
            bmp.push(pixels[si]); bmp.push(pixels[si+1]); bmp.push(pixels[si+2]);
        }
        for _ in 0..row_size as usize - w as usize * 3 { bmp.push(0); }
    }
    format!("data:image/bmp;base64,{}", base64_encode(&bmp))
}

#[tauri::command]
fn capture_stream_start(app: tauri::AppHandle, hwnd: u64, tcp_port: Option<u16>) -> Result<String, String> {
    let _ = capture_stream_stop();

    let port = tcp_port.unwrap_or(9999);

    let running = Arc::new(AtomicBool::new(true));
    let running_clone = running.clone();

    dlog!("stream_start: hwnd={} port={} (Rust-native)", hwnd, port);

    // Start TCP broadcast server
    let tcp_running = running.clone();
    let tcp_handle = thread::spawn(move || { tcp_broadcast_thread(port, tcp_running); });

    thread::spawn(move || {
        // --- First frame: detect best method ---
        let detect = unsafe { capture_window_internal(hwnd) };
        let mut method = detect.method.to_string();
        let mut w = detect.w;
        let mut h = detect.h;
        dlog!("stream: detected method={} state={} {}x{}", method, detect.window_state, w, h);

        if method == "None" || method == "ALL_FAILED" || w <= 0 || h <= 0 {
            dlog!("stream: no working capture method, aborting");
            return;
        }

        // Use first frame pixels
        if !detect.pixels.is_empty() {
            let uri = bgra_to_bmp_uri(&detect.pixels, w, h);
            if let Ok(mut state) = STREAM_FRAME.lock() {
                *state = (uri, w, h, method.clone());
            }
            let _ = app.emit("stream-tick", serde_json::json!({"method": &method}));
        }

        let hwnd_ptr = HWND(std::ptr::with_exposed_provenance_mut::<std::ffi::c_void>(hwnd as usize));
        let target_ms = 16u64; // 60fps
        let mut frames: u64 = 0;
        let mut prev_pixels: Vec<u8> = Vec::new();
        let mut recheck_counter = 0u32;
        let fps_t0 = Instant::now();

        while running_clone.load(Ordering::Relaxed) {
            // Window gone → abort (skip for desktop)
            if hwnd != 0 && unsafe { !IsWindow(hwnd_ptr).as_bool() } {
                dlog!("stream: window closed, stopping");
                break;
            }

            let frame_t0 = Instant::now();

            let result = unsafe { capture_fast(&method, hwnd_ptr, w, h) };

            if let Some((pixels, pw, ph)) = result {
                w = pw; h = ph;

                // Frame differ: skip unchanged frames
                if pixels.len() == prev_pixels.len() && pixels == prev_pixels {
                    // Emit size=0 signal (frontend reuses previous)
                    if let Ok(mut state) = STREAM_FRAME.lock() {
                        state.3 = method.clone();
                    }
                } else {
                    let uri = bgra_to_bmp_uri(&pixels, w, h);
                    // Store raw BGRA for TCP/pipe clients
                    if let Ok(mut raw) = RAW_FRAME.lock() {
                        *raw = (pixels.clone(), w, h);
                    }
                    prev_pixels = pixels;
                    if let Ok(mut state) = STREAM_FRAME.lock() {
                        *state = (uri, w, h, method.clone());
                    }
                }
                let _ = app.emit("stream-tick", serde_json::json!({"method": &method}));
                frames += 1;
                recheck_counter = 0;
            } else {
                // Capture failed — may need to re-detect method
                recheck_counter += 1;
                if recheck_counter > 30 {
                    // Re-detect every ~0.5s of failures
                    let redetect = unsafe { capture_window_internal(hwnd) };
                    if redetect.method != "None" && redetect.method != "ALL_FAILED" {
                        method = redetect.method.to_string();
                        w = redetect.w; h = redetect.h;
                        dlog!("stream: re-detected method={}", method);
                    }
                    recheck_counter = 0;
                }
            }

            // FPS log every 120 frames
            if frames > 0 && frames % 120 == 0 {
                let elapsed = fps_t0.elapsed().as_secs_f64();
                dlog!("stream: {} frames in {:.1}s = {:.0}fps method={}",
                    frames, elapsed, frames as f64 / elapsed, method);
            }

            // Frame pacing
            let elapsed = frame_t0.elapsed().as_millis() as u64;
            if elapsed < target_ms { std::thread::sleep(std::time::Duration::from_millis(target_ms - elapsed)); }
        }
        dlog!("stream: exited after {} frames", frames);
    });

    *STREAM.lock().unwrap() = Some(StreamState {
        running,
        _tcp_thread: Some(tcp_handle),
    });
    Ok("started".into())
}

#[tauri::command]
fn stream_poll() -> String {
    if let Ok(state) = STREAM_FRAME.lock() {
        if !state.0.is_empty() { return state.0.clone(); }
    }
    String::new()
}

#[tauri::command]
fn capture_stream_stop() -> Result<String, String> {
    let mut state = STREAM.lock().unwrap();
    if let Some(ref s) = *state {
        s.running.store(false, Ordering::Relaxed);
        dlog!("stream_stop: signaled stop");
    }
    *state = None;
    Ok("stopped".into())
}

// ── H.264 Capture Stream (GPU H.264 encode → fMP4 → MSE <video>) ──

/// Build MSE codec string from SPS NAL unit.
/// Format: "avc1.<profile_idc_hex><constraint_hex><level_hex>"
fn avc_codec_string(sps: &[u8]) -> String {
    if sps.len() >= 4 {
        format!("avc1.{:02X}{:02X}{:02X}", sps[1], sps[2], sps[3])
    } else {
        "avc1.42C01E".to_string() // Baseline L3.0 fallback
    }
}

/// Extract SPS + PPS NAL units from Annex B H.264 data.
/// Returns (sps, pps) as raw NAL units (without start code).
fn extract_sps_pps(h264_data: &[u8]) -> Option<(Vec<u8>, Vec<u8>)> {
    let mut sps: Option<Vec<u8>> = None;
    let mut pps: Option<Vec<u8>> = None;
    let mut pos = 0;

    while pos + 3 <= h264_data.len() {
        // Find start code
        let sc_len = if h264_data[pos] == 0 && h264_data[pos+1] == 0 && h264_data[pos+2] == 0 && pos+4 <= h264_data.len() && h264_data[pos+3] == 1 {
            4
        } else if h264_data[pos] == 0 && h264_data[pos+1] == 0 && h264_data[pos+2] == 1 {
            3
        } else {
            pos += 1; continue;
        };

        let nal_start = pos + sc_len;
        if nal_start >= h264_data.len() { break; }

        // Find next start code
        let mut nal_end = nal_start;
        while nal_end + 3 <= h264_data.len() {
            if h264_data[nal_end] == 0 && h264_data[nal_end+1] == 0 {
                if h264_data[nal_end+2] == 1 || (h264_data[nal_end+2] == 0 && nal_end+4 <= h264_data.len() && h264_data[nal_end+3] == 1) {
                    break;
                }
            }
            nal_end += 1;
        }

        let nal = &h264_data[nal_start..nal_end];
        if !nal.is_empty() {
            let nal_type = nal[0] & 0x1F;
            match nal_type {
                7 => sps = Some(nal.to_vec()),
                8 => pps = Some(nal.to_vec()),
                _ => {}
            }
        }

        if sps.is_some() && pps.is_some() { break; }
        pos = nal_end;
    }

    match (sps, pps) {
        (Some(s), Some(p)) => Some((s, p)),
        _ => None,
    }
}

struct H264StreamState {
    child: Option<Child>,
}

static H264_STREAM: Mutex<Option<H264StreamState>> = Mutex::new(None);
static H264_FRAME: Mutex<String> = Mutex::new(String::new()); // base64 media segment
static H264_INIT_READY: Mutex<bool> = Mutex::new(false);

#[tauri::command]
fn h264_stream_start(app: tauri::AppHandle, hwnd: u64) -> Result<String, String> {
    let _ = h264_stream_stop();

    let exe = std::env::current_exe()
        .map(|p| p.parent().unwrap_or(&p).join("..").join("..").join("..").join("..")
            .join("capture").join("build").join("capture_h264.exe"))
        .unwrap_or_else(|_| "capture/build/capture_h264.exe".into());
    let exe = std::fs::canonicalize(&exe).unwrap_or(exe);

    dlog!("h264_stream_start: {} {}", exe.display(), hwnd);

    let mut child = Command::new(&exe)
        .arg(hwnd.to_string())
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .creation_flags(0x08000000) // CREATE_NO_WINDOW
        .spawn()
        .map_err(|e| format!("spawn: {}", e))?;

    let stdout = child.stdout.take().ok_or("no stdout")?;
    let stderr = child.stderr.take().ok_or("no stderr")?;

    // Stderr reader
    thread::spawn(move || {
        for line in BufReader::new(stderr).lines() {
            if let Ok(l) = line { if !l.is_empty() { dlog!("  C++ h264 {}", l); } }
        }
    });

    // Frame reader
    let app_handle = app.clone();
    thread::spawn(move || {
        let mut reader = BufReader::new(stdout);

        // Read method line
        let mut method = String::new();
        let _ = reader.read_line(&mut method);
        let method = method.trim().to_string();
        dlog!("h264_stream: method={}", method);

        let mut prev_h264: Vec<u8> = Vec::new();
        let mut init_sent = false;
        let mut frames: u64 = 0;
        let mut seq_num: u32 = 1;
        let fps: u32 = 60;
        let sample_duration: u32 = 1000 / fps; // in timescale units (1000 = 1ms)

        loop {
            // Read header: [size:4 LE]
            let mut hdr = [0u8; 4];
            if reader.read_exact(&mut hdr).is_err() { break; }
            let size = u32::from_le_bytes(hdr) as usize;

            let h264_data = if size == 0 {
                if prev_h264.is_empty() { continue; }
                prev_h264.clone()
            } else {
                let mut data = vec![0u8; size];
                if reader.read_exact(&mut data).is_err() { break; }
                data
            };

            if h264_data.is_empty() { continue; }

            // Build init segment from first frame (contains SPS+PPS)
            if !init_sent {
                if let Some((sps, pps)) = extract_sps_pps(&h264_data) {
                    dlog!("h264_stream: SPS={}B PPS={}B", sps.len(), pps.len());
                    let codec = avc_codec_string(&sps);
                    dlog!("h264_stream: codec={}", codec);
                    let init = fmp4::build_init_segment(640, 360, &sps, &pps);
                    let init_b64 = base64_encode(&init);
                    let _ = app_handle.emit("h264-init", serde_json::json!({
                        "data": init_b64,
                        "codec": codec,
                        "method": method
                    }));
                    *H264_INIT_READY.lock().unwrap() = true;
                    init_sent = true;
                } else {
                    // No SPS/PPS yet — skip this frame, wait for keyframe
                    dlog!("h264_stream: waiting for SPS/PPS...");
                    continue;
                }
            }

            // Build media segment
            let pts: u64 = frames * sample_duration as u64;
            let segment = fmp4::build_media_segment(&h264_data, seq_num, pts, sample_duration);
            let seg_b64 = base64_encode(&segment);

            if let Ok(mut state) = H264_FRAME.lock() {
                *state = seg_b64;
            }
            let _ = app_handle.emit("h264-tick", serde_json::json!({}));
            prev_h264 = h264_data;
            frames += 1;
            seq_num += 1;
        }
        dlog!("h264_stream: exited after {} frames", frames);
    });

    *H264_STREAM.lock().unwrap() = Some(H264StreamState {
        child: Some(child),
    });
    Ok("started".into())
}

#[tauri::command]
fn h264_poll() -> String {
    if let Ok(state) = H264_FRAME.lock() {
        if !state.is_empty() { return state.clone(); }
    }
    String::new()
}

#[tauri::command]
fn h264_init_ready() -> bool {
    *H264_INIT_READY.lock().unwrap()
}

#[tauri::command]
fn h264_stream_stop() -> Result<String, String> {
    let mut state = H264_STREAM.lock().unwrap();
    if let Some(ref mut s) = *state {
        if let Some(ref mut child) = s.child {
            // Try to send quit signal — may fail if process already exited
            if let Some(stdin) = child.stdin.as_mut() {
                let _ = writeln!(stdin, "q");
                let _ = stdin.flush();
            }
            // Don't block forever — wait with timeout equivalent
            match child.try_wait() {
                Ok(Some(status)) => dlog!("h264_stream_stop: process already exited {:?}", status),
                Ok(None) => {
                    // Still running, kill it
                    let _ = child.kill();
                    let _ = child.wait();
                    dlog!("h264_stream_stop: process killed");
                }
                Err(e) => dlog!("h264_stream_stop: try_wait error {}", e),
            }
        }
    }
    *state = None;
    *H264_INIT_READY.lock().unwrap() = false;
    Ok("stopped".into())
}

fn main() {
    init_log(5);  // keep max 5 log files
    dlog!("Starting Tauri application...");

    tauri::Builder::default()
        .invoke_handler(tauri::generate_handler![
            list_windows, list_processes,
            capture_single, capture_window,
            capture_stream_start, capture_stream_stop, stream_poll,
            h264_stream_start, h264_stream_stop, h264_poll, h264_init_ready
        ])
        .setup(|_app| {
            dlog!("Tauri setup complete, window created");
            Ok(())
        })
        .run(tauri::generate_context!())
        .expect("error while running game agent monitor");
}
