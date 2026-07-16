/**
 * h264_recv_bench — TCP receiver for test/h264_hw_bench.cpp
 * Protocol: [w:u32][h:u32][flags:u32][ts_ms:u32][size:u32][annexb...]
 * Prints sustained receive FPS after ~8s (or until peer closes).
 */
import net from 'node:net'

const HOST = '127.0.0.1'
const PORT = 19999

let buf = Buffer.alloc(0)
let frames = 0
let bytes = 0
let lastTs = 0
let latSum = 0
let latN = 0
const t0 = Date.now()

function pump() {
  while (buf.length >= 20) {
    const w = buf.readUInt32LE(0)
    const h = buf.readUInt32LE(4)
    const flags = buf.readUInt32LE(8)
    const ts = buf.readUInt32LE(12)
    const size = buf.readUInt32LE(16)
    if (buf.length < 20 + size) return
    buf = buf.subarray(20 + size)
    frames++
    bytes += size
    // Agent GetTickCount64 low32 vs local Date.now() are different clocks —
    // only meaningful as relative jitter once synced; skip absolute latency here.
    if (ts && lastTs) {
      const d = (ts - lastTs) >>> 0
      if (d < 500) { latSum += d; latN++ }
    }
    lastTs = ts
    if (frames === 1) {
      console.log(`first frame ${w}x${h} key=${(flags & 1) !== 0} size=${size}`)
    }
  }
}

function connectWithRetry(attempt = 0) {
  const sock = net.connect({ host: HOST, port: PORT }, () => {
    console.log(`connected to ${HOST}:${PORT}`)
  })
  sock.on('data', (chunk) => {
    buf = Buffer.concat([buf, chunk])
    pump()
  })
  sock.on('error', (err) => {
    if (attempt < 40 && (err.code === 'ECONNREFUSED' || err.code === 'ECONNRESET')) {
      setTimeout(() => connectWithRetry(attempt + 1), 200)
      return
    }
    console.error('socket error', err.message)
    process.exit(1)
  })
  sock.on('close', () => {
    const sec = Math.max(0.001, (Date.now() - t0) / 1000)
    const fps = frames / sec
    const avgGap = latN ? (latSum / latN) : 0
    console.log(`RESULT frames=${frames} bytes=${bytes} fps=${fps.toFixed(1)} avg_frame_gap_ms=${avgGap.toFixed(1)}`)
    process.exit(fps >= 25 ? 0 : 3)
  })
}

connectWithRetry()
setTimeout(() => {
  // Safety: if sender never closes, report mid-run.
  const sec = Math.max(0.001, (Date.now() - t0) / 1000)
  console.log(`TICK frames=${frames} fps=${(frames / sec).toFixed(1)}`)
}, 8500)
