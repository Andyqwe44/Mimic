//! wgc_bench_recv.rs — TCP receive → save frames → report FPS
//!
//! Receives frames sent by wgc_bench_send.cpp via TCP.
//! Protocol: [magic:4][body_size:4][type_tag:4][body: body_size bytes]
//! BGRA body: [w:4][h:4][ch:4][reserved:4][pixels: w*h*ch]
//!
//! Compile: rustc wgc_bench_recv.rs -o wgc_bench_recv.exe
//! Run:     wgc_bench_recv.exe [--port N] [--save-every N] [--out-dir DIR]
//!
//! Output: per-second FPS + MB/s, final summary.
//! Saves every Nth frame as .bgra raw file in out-dir.

use std::net::{TcpListener, TcpStream};
use std::io::{Read, Write};
use std::fs;
use std::time::Instant;
use std::env;

const MAGIC: u32 = 0x4D415246;
const HEADER_SIZE: usize = 12;     // magic(4) + body_size(4) + type_tag(4)
const BGRA_HDR_SIZE: usize = 16;   // w(4) + h(4) + ch(4) + reserved(4)

struct BgraFrame {
    width: u32,
    height: u32,
    channels: u32,
    pixels: Vec<u8>,
}

fn parse_header(buf: &[u8; HEADER_SIZE]) -> Option<(u32, u32)> {
    let magic = u32::from_le_bytes([buf[0], buf[1], buf[2], buf[3]]);
    if magic != MAGIC { return None; }
    let size = u32::from_le_bytes([buf[4], buf[5], buf[6], buf[7]]);
    let tag  = u32::from_le_bytes([buf[8], buf[9], buf[10], buf[11]]);
    Some((size, tag))
}

fn parse_bgra(payload: &[u8]) -> Option<(u32, u32, u32, Vec<u8>)> {
    if payload.len() < BGRA_HDR_SIZE { return None; }
    let w  = u32::from_le_bytes([payload[0],  payload[1],  payload[2],  payload[3]]);
    let h  = u32::from_le_bytes([payload[4],  payload[5],  payload[6],  payload[7]]);
    let ch = u32::from_le_bytes([payload[8],  payload[9],  payload[10], payload[11]]);
    let px_bytes = (w * h * ch) as usize;
    if payload.len() < BGRA_HDR_SIZE + px_bytes { return None; }
    let pixels = payload[BGRA_HDR_SIZE..BGRA_HDR_SIZE + px_bytes].to_vec();
    Some((w, h, ch, pixels))
}

/// Read exactly N bytes (blocking).
fn read_exact(stream: &mut TcpStream, buf: &mut [u8]) -> std::io::Result<()> {
    stream.read_exact(buf)
}

fn save_bgra(path: &str, frame: &BgraFrame) {
    let mut f = match fs::File::create(path) {
        Ok(f) => f,
        Err(e) => { eprintln!("  save error: {}", e); return; }
    };
    let _ = f.write_all(&frame.width.to_le_bytes());
    let _ = f.write_all(&frame.height.to_le_bytes());
    let _ = f.write_all(&frame.channels.to_le_bytes());
    let _ = f.write_all(&frame.pixels);
}

fn main() {
    let args: Vec<String> = env::args().collect();
    let port: u16 = parse_arg(&args, "--port", 9999);
    let save_every: usize = parse_arg(&args, "--save-every", 0);  // 0 = don't save
    let out_dir: String = parse_arg_str(&args, "--out-dir", "wgc_frames");

    println!("=== WGC Bench Recv ===");
    println!("  Port:       {}", port);
    println!("  Save every: {} (0=off)", save_every);
    println!("  Out dir:    {}", out_dir);

    if save_every > 0 {
        let _ = fs::create_dir_all(&out_dir);
    }

    let listener = TcpListener::bind(("127.0.0.1", port))
        .expect("TCP bind failed");
    println!("[recv] listening on 127.0.0.1:{}", port);

    // Accept one connection (blocking)
    let (mut stream, addr) = listener.accept().expect("accept failed");
    let _ = stream.set_nodelay(true);  // disable Nagle
    println!("[recv] connected from {}", addr);

    let mut total_frames: u64 = 0;
    let mut total_bytes: u64 = 0;
    let mut last_report = Instant::now();
    let mut frames_since_report: u64 = 0;
    let mut bytes_since_report: u64 = 0;
    let bench_start = Instant::now();

    let mut hdr_buf = [0u8; HEADER_SIZE];

    loop {
        // Read 12-byte header
        if read_exact(&mut stream, &mut hdr_buf).is_err() {
            println!("[recv] connection closed");
            break;
        }

        let _recv_ts = Instant::now();

        let (body_size, type_tag) = match parse_header(&hdr_buf) {
            Some(v) => v,
            None => {
                eprintln!("[recv] bad magic, stream desynced");
                break;
            }
        };

        if body_size == 0 {
            // heartbeat / unchanged frame
            continue;
        }

        // Read body
        let mut body = vec![0u8; body_size as usize];
        if read_exact(&mut stream, &mut body).is_err() {
            println!("[recv] connection closed mid-body");
            break;
        }

        total_frames += 1;
        total_bytes += (HEADER_SIZE + body_size as usize) as u64;
        frames_since_report += 1;
        bytes_since_report += body_size as u64;

        // Parse BGRA if type matches
        if type_tag == 1 {
            // type_tag 1 = BGRA_FRAME per protocol
        }

        // Save every Nth frame
        if save_every > 0 && total_frames % save_every as u64 == 1 {
            if let Some((w, h, ch, pixels)) = parse_bgra(&body) {
                let frame = BgraFrame { width: w, height: h, channels: ch, pixels };
                let path = format!("{}/frame_{:06}_{}x{}.bgra", out_dir, total_frames, w, h);
                save_bgra(&path, &frame);
            }
        }

        // Per-second report
        let elapsed = last_report.elapsed();
        if elapsed.as_secs_f64() >= 1.0 {
            let fps = frames_since_report as f64 / elapsed.as_secs_f64();
            let mbps = bytes_since_report as f64 / elapsed.as_secs_f64() / 1024.0 / 1024.0;
            println!("[recv] {:>8} frames | {:6.1} FPS | {:6.1} MB/s | total={}",
                total_frames, fps, mbps, total_frames);
            last_report = Instant::now();
            frames_since_report = 0;
            bytes_since_report = 0;
        }
    }

    // Final report
    let total_elapsed = bench_start.elapsed();
    println!("\n=== Final Stats ===");
    println!("Total frames:  {}", total_frames);
    println!("Total data:    {:.1} MB", total_bytes as f64 / 1024.0 / 1024.0);
    println!("Elapsed:       {:.2}s", total_elapsed.as_secs_f64());
    println!("Avg FPS:       {:.1}", total_frames as f64 / total_elapsed.as_secs_f64());
    println!("Avg bandwidth: {:.1} MB/s",
        total_bytes as f64 / total_elapsed.as_secs_f64() / 1024.0 / 1024.0);
    if total_frames > 0 {
        println!("Avg frame size: {:.1} KB",
            (total_bytes as f64 / total_frames as f64) / 1024.0);
    }
}

fn parse_arg<T: std::str::FromStr>(args: &[String], name: &str, default: T) -> T {
    for i in 0..args.len() {
        if args[i] == name && i + 1 < args.len() {
            if let Ok(v) = args[i + 1].parse() { return v; }
        }
    }
    default
}

fn parse_arg_str(args: &[String], name: &str, default: &str) -> String {
    for i in 0..args.len() {
        if args[i] == name && i + 1 < args.len() {
            return args[i + 1].clone();
        }
    }
    default.to_string()
}
