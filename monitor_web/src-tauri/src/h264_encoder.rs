//! H.264 GPU encoder via Media Foundation SinkWriter.
//!
//! Spawns a dedicated encoder thread (IMFSinkWriter is not Send).
//! Capture thread sends BGRA frames via mpsc channel → encoder thread
//! encodes and writes MP4. HTTP server serves the growing MP4 file.

use std::io::Write;
use std::net::TcpListener;
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, mpsc};
use std::thread;
use windows::core::*;
use windows::Win32::Media::MediaFoundation::*;

// MFStartup/MFShutdown — manual FFI (not in windows crate 0.58)
#[link(name = "mfplat")]
extern "system" {
    fn MFStartup(Version: u32, dwFlags: u32) -> HRESULT;
    fn MFShutdown() -> HRESULT;
}

const MF_VERSION: u32 = 0x00020070;
const MFSTARTUP_FULL: u32 = 0;

// ── BGRA→NV12 fast integer ────────────────────────────────

fn bgra_to_nv12_fast(bgra: &[u8], w: usize, h: usize) -> Vec<u8> {
    let y_size = w * h;
    let mut nv12 = vec![0u8; y_size + y_size / 2];
    let (y_plane, uv_plane) = nv12.split_at_mut(y_size);
    for y in 0..h {
        let rb = y * w;
        for x in 0..w {
            let si = (rb + x) * 4;
            let (b, g, r) = (bgra[si] as i32, bgra[si+1] as i32, bgra[si+2] as i32);
            y_plane[rb + x] = ((66*r + 129*g + 25*b + 128) >> 8).clamp(0, 255) as u8;
            if (x & 1) == 0 && (y & 1) == 0 {
                let u = (((-38*r - 74*g + 112*b + 128) >> 8) + 128).clamp(0, 255) as u8;
                let v = (((112*r - 94*g - 18*b + 128) >> 8) + 128).clamp(0, 255) as u8;
                let di = (y / 2) * w + (x / 2) * 2;
                uv_plane[di] = u; uv_plane[di + 1] = v;
            }
        }
    }
    nv12
}

// ── Frame message ─────────────────────────────────────────

struct H264Frame {
    data: Vec<u8>,
    width: u32,
    height: u32,
}

// ── Encoder thread handle ─────────────────────────────────

pub struct H264EncoderHandle {
    sender: mpsc::Sender<H264Frame>,
    shutdown: Arc<AtomicBool>,
    output_path: PathBuf,
}

impl H264EncoderHandle {
    pub fn new(output_dir: &std::path::Path, w: u32, h: u32, fps: u32) -> std::result::Result<Self, String> {
        let path = output_dir.join("video.mp4");
        let (tx, rx) = mpsc::channel::<H264Frame>();
        let shutdown = Arc::new(AtomicBool::new(false));
        let shutdown_clone = shutdown.clone();
        let path_clone = path.clone();

        thread::spawn(move || {
            encoder_thread(rx, shutdown_clone, &path_clone, w, h, fps);
        });

        Ok(Self { sender: tx, shutdown, output_path: path })
    }

    pub fn push(&self, bgra: &[u8], w: u32, h: u32) {
        let _ = self.sender.send(H264Frame { data: bgra.to_vec(), width: w, height: h });
    }

    pub fn output_path(&self) -> &std::path::Path { &self.output_path }

    pub fn stop(&self) {
        self.shutdown.store(true, Ordering::SeqCst);
    }
}

// ── Encoder thread (runs IMFSinkWriter on single thread) ──

fn encoder_thread(
    rx: mpsc::Receiver<H264Frame>,
    shutdown: Arc<AtomicBool>,
    path: &std::path::Path,
    w: u32, h: u32, fps: u32,
) {
    // Init MF on this thread
    unsafe { let _ = MFStartup(MF_VERSION, MFSTARTUP_FULL); }

    let path_str: Vec<u16> = path.to_string_lossy()
        .encode_utf16().chain(std::iter::once(0)).collect();

    let sw: IMFSinkWriter = match unsafe {
        let pwsz = PCWSTR(path_str.as_ptr());
        MFCreateSinkWriterFromURL(pwsz, None, None)
    } {
        Ok(x) => x,
        Err(e) => { eprintln!("[h264] SinkWriter: {e:?}"); unsafe { let _ = MFShutdown(); } return; }
    };

    // Configure NV12 input
    let mt: IMFMediaType = match unsafe { MFCreateMediaType() } {
        Ok(x) => x, Err(_) => { unsafe { let _ = MFShutdown(); } return; }
    };
    unsafe {
        mt.SetGUID(&MF_MT_MAJOR_TYPE, &MFMediaType_Video).ok();
        mt.SetGUID(&MF_MT_SUBTYPE, &MFVideoFormat_NV12).ok();
        let fs: u64 = ((w as u64) << 32) | (h as u64);
        mt.SetUINT64(&MF_MT_FRAME_SIZE, fs).ok();
        let fr: u64 = ((fps as u64) << 32) | 1;
        mt.SetUINT64(&MF_MT_FRAME_RATE, fr).ok();
        mt.SetUINT32(&MF_MT_AVG_BITRATE, 8_000_000).ok();
    };

    let si = match unsafe { sw.AddStream(&mt) } {
        Ok(x) => x,
        Err(e) => { eprintln!("[h264] AddStream: {e:?}"); unsafe { let _ = MFShutdown(); } return; }
    };
    unsafe { sw.SetInputMediaType(si, &mt, None).ok(); }
    unsafe { sw.BeginWriting().ok(); }

    let mut frame_num: u64 = 0;
    let dur = 10_000_000u64 / fps as u64;

    while !shutdown.load(Ordering::Relaxed) {
        let frame: H264Frame = match rx.recv_timeout(std::time::Duration::from_millis(100)) {
            Ok(f) => f,
            Err(mpsc::RecvTimeoutError::Timeout) => continue,
            Err(mpsc::RecvTimeoutError::Disconnected) => break,
        };

        let nv12 = bgra_to_nv12_fast(&frame.data, frame.width as usize, frame.height as usize);

        // Create buffer
        let buf: IMFMediaBuffer = match unsafe { MFCreateMemoryBuffer(nv12.len() as u32) } {
            Ok(x) => x, Err(_) => continue,
        };
        let ok = unsafe {
            let mut data: *mut u8 = std::ptr::null_mut();
            let mut max_len: u32 = 0;
            buf.Lock(&mut data, Some(&mut max_len), None).is_ok()
                && {
                    let copy = nv12.len().min(max_len as usize);
                    std::ptr::copy_nonoverlapping(nv12.as_ptr(), data, copy);
                    buf.SetCurrentLength(copy as u32).is_ok()
                }
                && buf.Unlock().is_ok()
        };
        if !ok { continue; }

        // Create sample
        let sample: IMFSample = match unsafe { MFCreateSample() } {
            Ok(x) => x, Err(_) => continue,
        };
        unsafe { sample.AddBuffer(&buf).ok(); }
        unsafe { sample.SetSampleDuration(dur as i64).ok(); }
        unsafe { sample.SetSampleTime((frame_num * dur) as i64).ok(); }

        let _ = unsafe { sw.WriteSample(si, &sample) };
        frame_num += 1;
    }

    eprintln!("[h264] finalizing {} frames", frame_num);
    unsafe { let _ = sw.Finalize(); }
    unsafe { let _ = MFShutdown(); }
}

// ── HTTP server for MP4 progressive download ──────────────

pub fn start_video_server(port: u16, file_path: PathBuf, running: Arc<AtomicBool>) {
    thread::spawn(move || {
        let addr = format!("127.0.0.1:{}", port);
        let listener = match TcpListener::bind(&addr) {
            Ok(l) => l,
            Err(e) => { eprintln!("[h264] bind {}: {}", addr, e); return; }
        };
        eprintln!("[h264] serving http://{}/video.mp4", addr);
        for stream in listener.incoming() {
            if !running.load(Ordering::Relaxed) { break; }
            let mut tcp = match stream { Ok(s) => s, Err(_) => continue };
            let _ = tcp.set_nodelay(true);
            let mut buf = [0u8; 1024];
            let _ = tcp.peek(&mut buf);
            if let Ok(data) = std::fs::read(&file_path) {
                let hdr = format!(
                    "HTTP/1.1 200 OK\r\nContent-Type: video/mp4\r\nContent-Length: {}\r\n\
                     Accept-Ranges: bytes\r\nAccess-Control-Allow-Origin: *\r\n\
                     Cache-Control: no-cache\r\nConnection: close\r\n\r\n", data.len());
                let _ = tcp.write_all(hdr.as_bytes());
                let _ = tcp.write_all(&data);
            }
        }
    });
}
