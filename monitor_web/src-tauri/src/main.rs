#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

use std::process::Command;
use serde::Serialize;

#[derive(Clone, Serialize)]
struct WindowInfo { title: String, category: String }

#[tauri::command]
fn list_windows() -> Vec<WindowInfo> {
    let exe = std::env::current_dir()
        .map(|d| d.join("..").join("capture").join("build").join("window_list.exe"))
        .unwrap_or_else(|_| "../capture/build/window_list.exe".into());
    match Command::new(&exe).output() {
        Ok(out) => {
            String::from_utf8_lossy(&out.stdout).lines().filter_map(|line| {
                let line = line.trim(); if line.is_empty() { return None; }
                let cat = extract_json_str(line, "category").unwrap_or("window");
                let title = extract_json_str(line, "title").unwrap_or("Unknown");
                Some(WindowInfo { title: title.to_string(), category: cat.to_string() })
            }).collect()
        }
        Err(_) => vec![WindowInfo { title: " Entire Desktop".into(), category: "desktop".into() }]
    }
}

fn extract_json_str<'a>(line: &'a str, key: &str) -> Option<&'a str> {
    let search = format!("\"{}\":\"", key);
    let start = line.find(&search)? + search.len();
    let chars: Vec<char> = line[start..].chars().collect();
    let mut i = 0;
    while i < chars.len() {
        if chars[i] == '\\' { i += 2; continue; }
        if chars[i] == '"' { return Some(&line[start..start + i]); }
        i += 1;
    }
    Some(&line[start..])
}

// ── Single-frame screenshot via GDI → base64 PNG ──
#[tauri::command]
fn capture_single() -> String {
    unsafe { capture_screen_to_base64() }.unwrap_or_default()
}

#[cfg(target_os = "windows")]
unsafe fn capture_screen_to_base64() -> Option<String> {
    use std::mem;
    extern "system" {
        fn GetDC(hwnd: isize) -> isize;
        fn ReleaseDC(hwnd: isize, hdc: isize) -> i32;
        fn GetSystemMetrics(idx: i32) -> i32;
        fn CreateCompatibleDC(hdc: isize) -> isize;
        fn CreateCompatibleBitmap(hdc: isize, w: i32, h: i32) -> isize;
        fn SelectObject(hdc: isize, obj: isize) -> isize;
        fn DeleteDC(hdc: isize) -> i32;
        fn DeleteObject(obj: isize) -> i32;
        fn BitBlt(hdc: isize, x: i32, y: i32, w: i32, h: i32, src: isize, sx: i32, sy: i32, op: u32) -> i32;
        fn GetDIBits(hdc: isize, bmp: isize, start: u32, lines: u32, bits: *mut u8, bmi: *const u8, usage: u32) -> i32;
    }
    const SRCCOPY: u32 = 0x00CC0020;
    const SM_CXSCREEN: i32 = 0; const SM_CYSCREEN: i32 = 1;

    let hdc_screen = GetDC(0);
    if hdc_screen == 0 { return None; }
    let w = GetSystemMetrics(SM_CXSCREEN);
    let h = GetSystemMetrics(SM_CYSCREEN);
    let hdc_mem = CreateCompatibleDC(hdc_screen);
    let hbmp = CreateCompatibleBitmap(hdc_screen, w, h);
    let old = SelectObject(hdc_mem, hbmp);
    BitBlt(hdc_mem, 0, 0, w, h, hdc_screen, 0, 0, SRCCOPY);

    // Scale down to max 640px wide for reasonable size
    let scale = (640.0 / w as f32).min(1.0);
    let sw = (w as f32 * scale) as i32;
    let sh = (h as f32 * scale) as i32;

    let mut bmi = [0u8; 44]; // BITMAPINFOHEADER
    bmi[0..4].copy_from_slice(&44u32.to_le_bytes());
    bmi[4..8].copy_from_slice(&(sw as u32).to_le_bytes());
    bmi[8..12].copy_from_slice(&(-sh as i32).to_le_bytes());
    bmi[12..14].copy_from_slice(&1u16.to_le_bytes());
    bmi[14..16].copy_from_slice(&32u16.to_le_bytes());

    let row_bytes = ((sw * 4 + 3) / 4) * 4;
    let buf_size = (row_bytes * sh) as usize;
    let mut pixels: Vec<u8> = vec![0; buf_size];
    GetDIBits(hdc_mem, hbmp, 0, sh as u32, pixels.as_mut_ptr(), bmi.as_ptr(), 0);

    SelectObject(hdc_mem, old); DeleteObject(hbmp); DeleteDC(hdc_mem); ReleaseDC(0, hdc_screen);

    // BGRA → RGBA, resize to sw×sh
    let mut rgba = vec![0u8; (sw * sh * 4) as usize];
    for y in 0..sh {
        for x in 0..sw {
            let src_y = (y as f32 / scale) as usize;
            let src_x = (x as f32 / scale) as usize;
            let src_idx = (src_y * w as usize + src_x) * 4;
            let dst_idx = (y * sw + x) as usize * 4;
            rgba[dst_idx] = pixels[src_idx + 2];     // R
            rgba[dst_idx + 1] = pixels[src_idx + 1]; // G
            rgba[dst_idx + 2] = pixels[src_idx];     // B
            rgba[dst_idx + 3] = 255;                  // A
        }
    }

    // Encode PNG manually (minimal valid PNG)
    let mut out = Vec::new();
    // PNG signature
    out.extend_from_slice(&[137, 80, 78, 71, 13, 10, 26, 10]);
    // IHDR
    let mut ihdr_data = Vec::new();
    ihdr_data.extend_from_slice(&(sw as u32).to_be_bytes());
    ihdr_data.extend_from_slice(&(sh as u32).to_be_bytes());
    ihdr_data.extend_from_slice(&[8, 6, 0, 0, 0]); // 8bit RGBA
    write_png_chunk(&mut out, b"IHDR", &ihdr_data);
    // IDAT
    let mut raw = Vec::new();
    for y in 0..sh {
        raw.push(0); // filter: none
        let row_start = (y * sw) as usize * 4;
        raw.extend_from_slice(&rgba[row_start..row_start + sw as usize * 4]);
    }
    let compressed = miniz_oxide::deflate::compress_to_vec_zlib(&raw, 6);
    write_png_chunk(&mut out, b"IDAT", &compressed);
    // IEND
    write_png_chunk(&mut out, b"IEND", &[]);

    Some(base64_encode(&out))
}

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

#[cfg(not(target_os = "windows"))]
unsafe fn capture_screen_to_base64() -> Option<String> { None }

fn main() {
    tauri::Builder::default()
        .invoke_handler(tauri::generate_handler![list_windows, capture_single])
        .run(tauri::generate_context!())
        .expect("error while running game agent monitor");
}
