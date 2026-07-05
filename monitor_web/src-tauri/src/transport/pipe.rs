//! transport/pipe.rs — send/receive frames over stdin/stdout pipe.
//!
//! Depends on: protocol::protocol (for PayloadType, build_header, parse_header).

use crate::protocol::{self, PayloadType};
use std::io::{self, Read, Write};

/// Write a frame to a writer (stdout, file, etc.)
pub fn send_frame(writer: &mut impl Write, type_tag: PayloadType, payload: &[u8]) -> io::Result<()> {
    let hdr = protocol::build_header(payload.len() as u32, type_tag);
    writer.write_all(&hdr)?;
    if !payload.is_empty() { writer.write_all(payload)?; }
    writer.flush()
}

/// Max sane payload size (256 MiB). Reject larger as likely corrupt header.
const MAX_PAYLOAD_SIZE: u32 = 256 * 1024 * 1024;

/// Read a frame from a reader (stdin, file, etc.). Returns (type_tag, payload) or None on EOF.
pub fn recv_frame(reader: &mut impl Read) -> io::Result<Option<(PayloadType, Vec<u8>)>> {
    let mut hdr = [0u8; protocol::FRAME_HEADER_SIZE];
    if reader.read_exact(&mut hdr).is_err() { return Ok(None); }
    let Some((size, type_tag)) = protocol::parse_header(&hdr) else {
        return Err(io::Error::new(io::ErrorKind::InvalidData, "bad magic"));
    };
    if size > MAX_PAYLOAD_SIZE {
        return Err(io::Error::new(io::ErrorKind::InvalidData,
            format!("payload size {} exceeds max {}", size, MAX_PAYLOAD_SIZE)));
    }
    let mut payload = vec![0u8; size as usize];
    if size > 0 { reader.read_exact(&mut payload)?; }
    Ok(Some((type_tag, payload)))
}
