//! 示例: Rust 从 stdin 读取 C++ pipe 发来的帧
//!
//! 运行:
//!   rustc rust_pipe_recv.rs -o rust_pipe_recv
//!   ./cpp_pipe_send.exe | ./rust_pipe_recv
//!
//! 或直接使用项目内的 stream_protocol 模块:
//!   cargo run --example rust_pipe_recv < pipe.dat

use std::io::{self, Read, Write};

// ── stream_protocol 的核心逻辑（复制自 src-tauri/src/stream_protocol.rs）────

const FRAME_MAGIC: u32 = 0x4D415246; // "FRAM" LE
const FRAME_HEADER_SIZE: usize = 20;

fn parse_header(data: &[u8; 20]) -> Option<(u32, u32, u32, u32)> {
    let magic = u32::from_le_bytes([data[0], data[1], data[2], data[3]]);
    if magic != FRAME_MAGIC { return None; }
    let w = u32::from_le_bytes([data[4], data[5], data[6], data[7]]);
    let h = u32::from_le_bytes([data[8], data[9], data[10], data[11]]);
    let ch = u32::from_le_bytes([data[12], data[13], data[14], data[15]]);
    let size = u32::from_le_bytes([data[16], data[17], data[18], data[19]]);
    Some((w, h, ch, size))
}

fn read_frame(reader: &mut impl Read) -> io::Result<Option<(u32, u32, u32, Vec<u8>)>> {
    let mut hdr = [0u8; FRAME_HEADER_SIZE];
    if reader.read_exact(&mut hdr).is_err() { return Ok(None); }

    // 全是 0 的帧头 = 发送端发来的"无变化"信号
    if hdr.iter().all(|&b| b == 0) {
        return Ok(None); // unchanged signal
    }

    let Some((w, h, ch, size)) = parse_header(&hdr) else {
        return Err(io::Error::new(io::ErrorKind::InvalidData, "bad magic"));
    };
    let mut pixels = vec![0u8; size as usize];
    reader.read_exact(&mut pixels)?;
    Ok(Some((w, h, ch, pixels)))
}

// ── 主程序 ──────────────────────────────────────────────

fn main() {
    let stdin = io::stdin();
    let mut reader = stdin.lock();
    let mut frames = 0u64;
    let mut skipped = 0u64;

    while let Ok(Some((w, h, ch, pixels))) = read_frame(&mut reader) {
        frames += 1;
        eprintln!(
            "[rust] frame {}: {}x{} ch={} {}KB",
            frames,
            w, h, ch,
            pixels.len() / 1024
        );
    }

    eprintln!("[rust] done: {} frames received, {} skipped (unchanged)", frames, skipped);
}
