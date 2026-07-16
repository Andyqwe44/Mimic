/**
 * GAM Web Controller — decode H.264 (WebCodecs) + send atomic input JSON.
 * Connects to ControllerBridge WebSocket (scripts/controller_bridge.py),
 * which speaks the agent TCP FRAM protocol on :9999.
 */
const hostEl = document.getElementById('host')
const btn = document.getElementById('btn')
const statusEl = document.getElementById('status')
const canvas = document.getElementById('cv')
const ctx = canvas.getContext('2d')

let ws = null
let decoder = null
let videoW = 0
let videoH = 0
let buttonDown = false

function setStatus(s) { statusEl.textContent = s }

function sendAction(obj) {
  if (!ws || ws.readyState !== WebSocket.OPEN) return
  ws.send(JSON.stringify(obj))
}

function normFromEvent(e) {
  const r = canvas.getBoundingClientRect()
  const x = (e.clientX - r.left) / r.width
  const y = (e.clientY - r.top) / r.height
  return {
    x_norm: Math.min(1, Math.max(0, x)),
    y_norm: Math.min(1, Math.max(0, y)),
    in: x >= 0 && x <= 1 && y >= 0 && y <= 1,
  }
}

function ensureDecoder(w, h) {
  if (decoder && videoW === w && videoH === h) return
  if (decoder) { try { decoder.close() } catch (_) {} }
  videoW = w
  videoH = h
  canvas.width = w
  canvas.height = h
  if (typeof VideoDecoder === 'undefined') {
    setStatus('WebCodecs VideoDecoder not available')
    return
  }
  decoder = new VideoDecoder({
    output: (frame) => {
      ctx.drawImage(frame, 0, 0, canvas.width, canvas.height)
      frame.close()
    },
    error: (e) => setStatus('Decoder error: ' + e.message),
  })
  decoder.configure({
    codec: 'avc1.42E01E', // Baseline; may be overridden by SPS in bitstream on some browsers
    optimizeForLatency: true,
  })
}

function connect() {
  if (ws) { try { ws.close() } catch (_) {} ws = null }
  const url = hostEl.value.trim()
  setStatus('Connecting…')
  ws = new WebSocket(url)
  ws.binaryType = 'arraybuffer'
  ws.onopen = () => {
    setStatus('Connected')
    btn.textContent = 'Disconnect'
    btn.classList.add('off')
  }
  ws.onclose = () => {
    setStatus('Disconnected')
    btn.textContent = 'Connect'
    btn.classList.remove('off')
  }
  ws.onerror = () => setStatus('WebSocket error')
  ws.onmessage = async (ev) => {
    if (typeof ev.data === 'string') {
      try {
        const m = JSON.parse(ev.data)
        if (m.type === 'status') setStatus(m.msg || 'OK')
      } catch (_) {}
      return
    }
    const buf = new Uint8Array(ev.data)
    if (buf.byteLength < 16) return
    const view = new DataView(buf.buffer, buf.byteOffset, buf.byteLength)
    const w = view.getUint32(0, true)
    const h = view.getUint32(4, true)
    const flags = view.getUint32(8, true)
    const annexb = buf.subarray(16)
    ensureDecoder(w, h)
    if (!decoder || decoder.state === 'closed') return
    const key = (flags & 1) !== 0
    try {
      decoder.decode(new EncodedVideoChunk({
        type: key ? 'key' : 'delta',
        timestamp: performance.now() * 1000,
        data: annexb,
      }))
    } catch (e) {
      setStatus('decode: ' + e.message)
    }
  }
}

btn.onclick = () => {
  if (ws && ws.readyState === WebSocket.OPEN) ws.close()
  else connect()
}

canvas.addEventListener('pointerdown', (e) => {
  canvas.setPointerCapture(e.pointerId)
  const c = normFromEvent(e)
  if (!c.in) return
  buttonDown = true
  const button = e.button === 2 ? 'right' : e.button === 1 ? 'middle' : 'left'
  sendAction({ type: 'mousedown', button, x_norm: c.x_norm, y_norm: c.y_norm })
})
canvas.addEventListener('pointermove', (e) => {
  if (!buttonDown) return
  const c = normFromEvent(e)
  if (!c.in) return
  sendAction({ type: 'move', held: true, button: 'left', x_norm: c.x_norm, y_norm: c.y_norm })
})
canvas.addEventListener('pointerup', (e) => {
  if (!buttonDown) return
  buttonDown = false
  const c = normFromEvent(e)
  const button = e.button === 2 ? 'right' : e.button === 1 ? 'middle' : 'left'
  sendAction({ type: 'mouseup', button, x_norm: c.x_norm, y_norm: c.y_norm })
})
canvas.addEventListener('contextmenu', (e) => e.preventDefault())

window.addEventListener('keydown', (e) => {
  if (e.repeat) return
  sendAction({ type: 'keydown', key: e.key, code: e.code })
})
window.addEventListener('keyup', (e) => {
  sendAction({ type: 'keyup', key: e.key, code: e.code })
})
