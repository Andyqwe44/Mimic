//! MJPEG streaming server.
//!
//! Browser <img> natively handles MJPEG (multipart/x-mixed-replace).
//! Zero JS overhead: just <img src="http://127.0.0.1:9998/stream">.
//!
//! Architecture: capture thread pushes raw BGRA → MJPEG server thread
//! encodes JPEG + serves it. Encoding is OFF the hot path.

use std::io::Write;
use std::net::TcpListener;
use std::sync::Mutex;
use std::sync::atomic::{AtomicBool, Ordering};
use std::thread;

struct MjpegState {
    bgra: Vec<u8>,
    width: u32,
    height: u32,
    frame_count: u64,
}

static MJPEG_STATE: Mutex<MjpegState> = Mutex::new(MjpegState {
    bgra: Vec::new(),
    width: 0,
    height: 0,
    frame_count: 0,
});

static MJPEG_RUNNING: AtomicBool = AtomicBool::new(false);

pub fn start_mjpeg_server(port: u16) {
    if MJPEG_RUNNING.swap(true, Ordering::SeqCst) {
        return;
    }
    let addr = format!("127.0.0.1:{}", port);
    thread::spawn(move || {
        let listener = loop {
            match TcpListener::bind(&addr) {
                Ok(l) => { eprintln!("[mjpeg] serving http://{}/stream", addr); break l; }
                Err(_) => {
                    std::thread::sleep(std::time::Duration::from_millis(150));
                    // Give up after ~3s
                }
            }
            if !MJPEG_RUNNING.load(Ordering::SeqCst) { return; }
        };

        for stream in listener.incoming() {
            if !MJPEG_RUNNING.load(Ordering::Relaxed) { break; }
            let mut tcp = match stream { Ok(s) => s, Err(_) => continue };
            let _ = tcp.set_nodelay(true);
            let _ = tcp.set_write_timeout(Some(std::time::Duration::from_secs(2)));

            let boundary = "GAMFRAME";
            let _ = write!(tcp,
                "HTTP/1.0 200 OK\r\n\
                 Cache-Control: no-cache, no-store\r\n\
                 Connection: close\r\n\
                 Content-Type: multipart/x-mixed-replace;boundary={}\r\n\
                 \r\n\
                 --{}\r\n", boundary, boundary);

            let mut last_count: u64 = 0;
            loop {
                if !MJPEG_RUNNING.load(Ordering::Relaxed) { break; }
                let (bgra, w, h, count) = {
                    if let Ok(st) = MJPEG_STATE.lock() {
                        if st.frame_count <= last_count || st.bgra.is_empty() {
                            drop(st);
                            thread::sleep(std::time::Duration::from_millis(5));
                            continue;
                        }
                        last_count = st.frame_count;
                        (st.bgra.clone(), st.width, st.height, st.frame_count)
                    } else {
                        break;
                    }
                };
                let _ = count;

                // JPEG encode on the MJPEG server thread (off the capture hot path)
                let jpeg = encode_jpeg(&bgra, w, h);
                if jpeg.is_empty() { continue; }

                let header = format!(
                    "Content-Type: image/jpeg\r\nContent-Length: {}\r\n\r\n",
                    jpeg.len()
                );
                if tcp.write_all(header.as_bytes()).is_err() { break; }
                if tcp.write_all(&jpeg).is_err() { break; }
                if write!(tcp, "\r\n--{}\r\n", boundary).is_err() { break; }
                let _ = tcp.flush();
            }
        }
        MJPEG_RUNNING.store(false, Ordering::SeqCst);
    });
}

pub fn stop_mjpeg_server() {
    MJPEG_RUNNING.store(false, Ordering::SeqCst);
    // Poke the listener to unblock incoming() so old thread exits quickly
    let _ = std::net::TcpStream::connect_timeout(
        &"127.0.0.1:9998".parse().unwrap(),
        std::time::Duration::from_millis(200),
    );
}

/// Fast: just store raw BGRA, no encoding on capture thread.
pub fn push_mjpeg_frame(bgra: &[u8], w: u32, h: u32) {
    if let Ok(mut st) = MJPEG_STATE.lock() {
        st.bgra.clear();
        st.bgra.extend_from_slice(bgra);
        st.width = w;
        st.height = h;
        st.frame_count += 1;
    }
}

// ── BGRA→JPEG (jpeg-encoder) ──────────────────────────────

fn encode_jpeg(bgra: &[u8], w: u32, h: u32) -> Vec<u8> {
    // BGRA→RGB
    let rgb: Vec<u8> = bgra.chunks(4).filter_map(|px| {
        if px.len() >= 4 { Some([px[2], px[1], px[0]]) } else { None }
    }).flatten().collect();

    let mut buf = Vec::new();
    match jpeg_encoder::Encoder::new(&mut buf, 70)
        .encode(&rgb, w as u16, h as u16, jpeg_encoder::ColorType::Rgb)
    {
        Ok(()) => buf,
        Err(_) => Vec::new(),
    }
}
