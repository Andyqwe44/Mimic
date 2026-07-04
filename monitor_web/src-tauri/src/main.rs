#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

use std::process::Command;
use serde::Serialize;

#[derive(Clone, Serialize)]
struct WindowInfo {
    title: String,
    category: String,
}

#[tauri::command]
fn list_windows() -> Vec<WindowInfo> {
    let exe = std::env::current_dir()
        .map(|d| d.join("..").join("capture").join("build").join("window_list.exe"))
        .unwrap_or_else(|_| "../capture/build/window_list.exe".into());

    match Command::new(&exe).output() {
        Ok(out) => {
            let text = String::from_utf8_lossy(&out.stdout);
            text.lines()
                .filter_map(|line| {
                    let line = line.trim();
                    if line.is_empty() { return None; }
                    // Simple JSON parse: extract "category" and "title"
                    let cat = extract_json_str(line, "category").unwrap_or("window");
                    let title = extract_json_str(line, "title").unwrap_or("Unknown");
                    Some(WindowInfo { title: title.to_string(), category: cat.to_string() })
                })
                .collect()
        }
        Err(_) => vec![WindowInfo { title: " Entire Desktop".into(), category: "desktop".into() }]
    }
}

fn extract_json_str<'a>(line: &'a str, key: &str) -> Option<&'a str> {
    let search = format!("\"{}\":\"", key);
    let start = line.find(&search)? + search.len();
    let mut end = start;
    let chars: Vec<char> = line[start..].chars().collect();
    let mut i = 0;
    while i < chars.len() {
        if chars[i] == '\\' { i += 2; continue; }
        if chars[i] == '"' { end = start + i; break; }
        i += 1;
    }
    Some(&line[start..end])
}

fn main() {
    tauri::Builder::default()
        .invoke_handler(tauri::generate_handler![list_windows])
        .run(tauri::generate_context!())
        .expect("error while running game agent monitor");
}
