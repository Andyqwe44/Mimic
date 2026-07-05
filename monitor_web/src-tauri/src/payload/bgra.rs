//! payload/bgra.rs — BGRA pixel frame payload (PayloadType::BgraFrame).
//!
//! Depends on: protocol::protocol (for PayloadType).
//! Does NOT depend on transport.
//!
//! Payload body: [w:4 LE][h:4 LE][ch:4 LE][reserved:4][pixels: w*h*ch bytes]

pub struct BgraFrame {
    pub width: u32,
    pub height: u32,
    pub channels: u32,
    pub pixels: Vec<u8>,
}

pub const HEADER_SIZE: usize = 16; // w(4)+h(4)+ch(4)+reserved(4)

/// Pack BGRA pixels → payload bytes.
pub fn pack(pixels: &[u8], w: u32, h: u32, ch: u32) -> Vec<u8> {
    let mut out = Vec::with_capacity(HEADER_SIZE + pixels.len());
    out.extend_from_slice(&w.to_le_bytes());
    out.extend_from_slice(&h.to_le_bytes());
    out.extend_from_slice(&ch.to_le_bytes());
    out.extend_from_slice(&0u32.to_le_bytes()); // reserved
    out.extend_from_slice(pixels);
    out
}

/// Unpack payload bytes → BgraFrame.
pub fn unpack(payload: &[u8]) -> Option<BgraFrame> {
    if payload.len() < HEADER_SIZE { return None; }
    let w  = u32::from_le_bytes([payload[0], payload[1], payload[2], payload[3]]);
    let h  = u32::from_le_bytes([payload[4], payload[5], payload[6], payload[7]]);
    let ch = u32::from_le_bytes([payload[8], payload[9], payload[10], payload[11]]);
    // Validate: non-zero, reasonable dimensions, no overflow
    if w == 0 || h == 0 || ch == 0 { return None; }
    if w > 16384 || h > 16384 || ch > 16 { return None; }
    let px_size = (w as u64 * h as u64 * ch as u64) as usize;
    // Overflow check: if the cast truncates, the value was too large
    if px_size as u64 != w as u64 * h as u64 * ch as u64 { return None; }
    if px_size > 1024 * 1024 * 1024 { return None; } // 1 GiB sanity cap
    if payload.len() < HEADER_SIZE + px_size { return None; }
    Some(BgraFrame {
        width: w, height: h, channels: ch,
        pixels: payload[HEADER_SIZE..HEADER_SIZE + px_size].to_vec(),
    })
}
