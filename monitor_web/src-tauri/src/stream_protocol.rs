/// Stream Protocol — shared constants across C++, Rust, Python.
///
/// Frame format (binary, little-endian):
///   [magic:4 "FRAM"][w:4][h:4][ch:4][size:4][pixels: size bytes]
///
/// Keep in sync with:
///   common/include/stream_protocol.hpp
///   model/stream_protocol.py

// ── Network ─────────────────────────────────────────────
pub const DEFAULT_TCP_PORT: u16 = 9999;
pub const DEFAULT_HOST: &str = "127.0.0.1";

// ── Pipe ────────────────────────────────────────────────
pub const DEFAULT_PIPE_NAME: &str = "tictactoe_stream";

// ── Frame format ────────────────────────────────────────
pub const FRAME_MAGIC: u32 = 0x4D415246; // "FRAM" LE
pub const FRAME_HEADER_SIZE: usize = 20; // magic(4) + w(4) + h(4) + ch(4) + size(4)
pub const FRAME_CH_BGRA: u32 = 4;
pub const MAX_FRAME_DIM: i32 = 640;

// ── Capabilities (bitmask) ──────────────────────────────
pub const CAP_BGRA_RAW: u32 = 1 << 0;
pub const CAP_H264_STREAM: u32 = 1 << 1;
pub const CAP_DESKTOP: u32 = 1 << 2;
pub const CAP_WINDOW: u32 = 1 << 3;

/// Build frame header into a [u8; 20] buffer.
pub fn build_frame_header(w: u32, h: u32, ch: u32, size: u32) -> [u8; 20] {
    let mut hdr = [0u8; 20];
    hdr[0..4].copy_from_slice(&FRAME_MAGIC.to_le_bytes());
    hdr[4..8].copy_from_slice(&w.to_le_bytes());
    hdr[8..12].copy_from_slice(&h.to_le_bytes());
    hdr[12..16].copy_from_slice(&ch.to_le_bytes());
    hdr[16..20].copy_from_slice(&size.to_le_bytes());
    hdr
}

/// Parse frame header from bytes. Returns (w, h, ch, size) or None if magic mismatch.
pub fn parse_frame_header(data: &[u8; 20]) -> Option<(u32, u32, u32, u32)> {
    let magic = u32::from_le_bytes([data[0], data[1], data[2], data[3]]);
    if magic != FRAME_MAGIC { return None; }
    let w = u32::from_le_bytes([data[4], data[5], data[6], data[7]]);
    let h = u32::from_le_bytes([data[8], data[9], data[10], data[11]]);
    let ch = u32::from_le_bytes([data[12], data[13], data[14], data[15]]);
    let size = u32::from_le_bytes([data[16], data[17], data[18], data[19]]);
    Some((w, h, ch, size))
}

/// Read a complete frame from a reader. Returns (w, h, ch, pixels).
pub fn read_frame(reader: &mut impl std::io::Read) -> std::io::Result<(u32, u32, u32, Vec<u8>)> {
    let mut hdr = [0u8; 20];
    reader.read_exact(&mut hdr)?;
    let (w, h, ch, size) = parse_frame_header(&hdr)
        .ok_or_else(|| std::io::Error::new(std::io::ErrorKind::InvalidData, "bad magic"))?;
    let mut pixels = vec![0u8; size as usize];
    reader.read_exact(&mut pixels)?;
    Ok((w, h, ch, pixels))
}

/// Write a frame to a writer.
pub fn write_frame(writer: &mut impl std::io::Write, w: u32, h: u32, ch: u32, pixels: &[u8]) -> std::io::Result<()> {
    let size = pixels.len() as u32;
    let hdr = build_frame_header(w, h, ch, size);
    writer.write_all(&hdr)?;
    writer.write_all(pixels)?;
    writer.flush()
}
