/**
 * GAM Controller — React UI matching Game Agent Monitor chrome.
 * WebSocket → agent :9997 · WebCodecs H.264 · atomic input JSON.
 */
import { useCallback, useEffect, useRef, useState } from 'react'
import { Cable, Moon, Sun, Unplug } from 'lucide-react'

function defaultWsUrl() {
  const host = window.location.hostname || '127.0.0.1'
  const proto = window.location.protocol === 'https:' ? 'wss' : 'ws'
  // Served from agent :9997 → same origin WS. Vite :8080 → agent :9997.
  if (window.location.port === '9997') return `${proto}://${host}:9997`
  if (window.location.port === '8080' || window.location.port === '')
    return `${proto}://${host}:9997`
  return `${proto}://${host}:9997`
}

function annexbHasIdr(u8: Uint8Array) {
  for (let i = 0; i + 4 < u8.length; i++) {
    if (u8[i] === 0 && u8[i + 1] === 0 && u8[i + 2] === 0 && u8[i + 3] === 1) {
      if ((u8[i + 4] & 0x1f) === 5) return true
      i += 3
    } else if (u8[i] === 0 && u8[i + 1] === 0 && u8[i + 2] === 1) {
      if ((u8[i + 3] & 0x1f) === 5) return true
      i += 2
    }
  }
  return false
}

export function App() {
  const [dark, setDark] = useState(() =>
    window.matchMedia('(prefers-color-scheme: dark)').matches)
  const [host, setHost] = useState(defaultWsUrl)
  const [status, setStatus] = useState('Disconnected')
  const [connected, setConnected] = useState(false)
  const [fps, setFps] = useState(0)
  const [dims, setDims] = useState('')
  const [latencyMs, setLatencyMs] = useState<number | null>(null)

  const canvasRef = useRef<HTMLCanvasElement>(null)
  const wsRef = useRef<WebSocket | null>(null)
  const decoderRef = useRef<VideoDecoder | null>(null)
  const needKeyRef = useRef(true)
  const videoSizeRef = useRef({ w: 0, h: 0 })
  const framesRef = useRef(0)
  const lastFpsTsRef = useRef(performance.now())
  const buttonDownRef = useRef(false)
  /** Map agent GetTickCount64-low32 → performance.now() origin (first keyframe). */
  const tickOriginRef = useRef<{ agent: number; local: number } | null>(null)
  const latSumRef = useRef(0)
  const latNRef = useRef(0)

  useEffect(() => {
    document.documentElement.classList.toggle('dark', dark)
  }, [dark])

  const sendAction = useCallback((obj: Record<string, unknown>) => {
    const ws = wsRef.current
    if (!ws || ws.readyState !== WebSocket.OPEN) return
    ws.send(JSON.stringify(obj))
  }, [])

  const closeDecoder = () => {
    try { decoderRef.current?.close() } catch { /* */ }
    decoderRef.current = null
  }

  const ensureDecoder = useCallback((w: number, h: number) => {
    const canvas = canvasRef.current
    if (!canvas) return
    if (decoderRef.current && videoSizeRef.current.w === w && videoSizeRef.current.h === h) return
    closeDecoder()
    videoSizeRef.current = { w, h }
    canvas.width = w
    canvas.height = h
    setDims(`${w}×${h}`)
    needKeyRef.current = true
    if (typeof VideoDecoder === 'undefined') {
      setStatus('WebCodecs VideoDecoder not available')
      return
    }
    const ctx = canvas.getContext('2d')
    decoderRef.current = new VideoDecoder({
      output: (frame) => {
        ctx?.drawImage(frame, 0, 0, canvas.width, canvas.height)
        frame.close()
        framesRef.current++
        const now = performance.now()
        if (now - lastFpsTsRef.current >= 1000) {
          setFps(framesRef.current)
          framesRef.current = 0
          lastFpsTsRef.current = now
        }
      },
      error: (e) => {
        needKeyRef.current = true
        setStatus('Decoder error: ' + e.message)
      },
    })
    // Baseline Level 4.0 — required for 1080p30 (avc1.42E01E Level 3.0 cannot).
    decoderRef.current.configure({
      codec: 'avc1.42E028',
      optimizeForLatency: true,
      hardwareAcceleration: 'prefer-hardware',
    })
    needKeyRef.current = true
  }, [])

  const disconnect = useCallback(() => {
    try { wsRef.current?.close() } catch { /* */ }
    wsRef.current = null
    closeDecoder()
    setConnected(false)
    setStatus('Disconnected')
    setFps(0)
    setLatencyMs(null)
    tickOriginRef.current = null
    latSumRef.current = 0
    latNRef.current = 0
  }, [])

  const connect = useCallback(() => {
    disconnect()
    const url = host.trim()
    setStatus('Connecting…')
    needKeyRef.current = true
    tickOriginRef.current = null
    const ws = new WebSocket(url)
    ws.binaryType = 'arraybuffer'
    wsRef.current = ws
    ws.onopen = () => {
      setConnected(true)
      setStatus('Connected · waiting for IDR…')
    }
    ws.onclose = () => {
      setConnected(false)
      setStatus('Disconnected')
      needKeyRef.current = true
    }
    ws.onerror = () => setStatus('WebSocket error')
    ws.onmessage = (ev) => {
      if (typeof ev.data === 'string') return
      const buf = new Uint8Array(ev.data as ArrayBuffer)
      if (buf.byteLength < 16) return
      const view = new DataView(buf.buffer, buf.byteOffset, buf.byteLength)
      const w = view.getUint32(0, true)
      const h = view.getUint32(4, true)
      const flags = view.getUint32(8, true)
      const agentTs = view.getUint32(12, true)
      const annexb = buf.subarray(16)
      ensureDecoder(w, h)
      const decoder = decoderRef.current
      if (!decoder || decoder.state === 'closed') return
      const key = ((flags & 1) !== 0) || annexbHasIdr(annexb)
      if (needKeyRef.current && !key) return
      // Drop if decoder already has work — keep glass-to-glass tight.
      if (decoder.decodeQueueSize > 0) return
      const now = performance.now()
      if (agentTs) {
        if (!tickOriginRef.current) {
          tickOriginRef.current = { agent: agentTs, local: now }
        } else {
          const elapsedAgent = (agentTs - tickOriginRef.current.agent) >>> 0
          const elapsedLocal = now - tickOriginRef.current.local
          const lat = elapsedLocal - elapsedAgent
          if (lat >= 0 && lat < 2000) {
            latSumRef.current += lat
            latNRef.current++
            if (latNRef.current % 15 === 0) {
              setLatencyMs(Math.round(latSumRef.current / latNRef.current))
              latSumRef.current = 0
              latNRef.current = 0
            }
          }
        }
      }
      try {
        decoder.decode(new EncodedVideoChunk({
          type: key ? 'key' : 'delta',
          timestamp: now * 1000,
          data: annexb,
        }))
        if (key) needKeyRef.current = false
      } catch (e: unknown) {
        needKeyRef.current = true
        setStatus('decode: ' + (e instanceof Error ? e.message : String(e)))
      }
    }
  }, [host, disconnect, ensureDecoder])

  useEffect(() => () => disconnect(), [disconnect])

  useEffect(() => {
    const onKeyDown = (e: KeyboardEvent) => {
      if (e.repeat) return
      sendAction({ type: 'keydown', key: e.key, code: e.code })
    }
    const onKeyUp = (e: KeyboardEvent) => {
      sendAction({ type: 'keyup', key: e.key, code: e.code })
    }
    window.addEventListener('keydown', onKeyDown)
    window.addEventListener('keyup', onKeyUp)
    return () => {
      window.removeEventListener('keydown', onKeyDown)
      window.removeEventListener('keyup', onKeyUp)
    }
  }, [sendAction])

  const normFromEvent = (e: React.PointerEvent) => {
    const canvas = canvasRef.current
    if (!canvas) return { x_norm: 0, y_norm: 0, in: false }
    const r = canvas.getBoundingClientRect()
    const x = (e.clientX - r.left) / r.width
    const y = (e.clientY - r.top) / r.height
    return {
      x_norm: Math.min(1, Math.max(0, x)),
      y_norm: Math.min(1, Math.max(0, y)),
      in: x >= 0 && x <= 1 && y >= 0 && y <= 1,
    }
  }

  return (
    <div className="h-full flex flex-col bg-bg-primary">
      {/* Top bar — mirrors GAM TopBar chrome */}
      <header className="shrink-0 h-11 px-3 flex items-center gap-2 border-b border-border bg-bg-secondary">
        <div className="text-sm font-semibold text-text-primary tracking-tight mr-1">
          GAM <span className="text-accent">Controller</span>
        </div>
        <input
          className="flex-1 min-w-0 h-8 px-2.5 rounded-md text-xs bg-bg-primary border border-border text-text-primary outline-none focus:ring-1 focus:ring-accent/50"
          value={host}
          onChange={(e) => setHost(e.target.value)}
          spellCheck={false}
        />
        <button
          type="button"
          onClick={() => (connected ? disconnect() : connect())}
          className={`h-8 px-3 rounded-md text-xs font-semibold flex items-center gap-1.5 transition-colors ${
            connected
              ? 'bg-bg-tertiary text-text-secondary hover:bg-bg-hover'
              : 'bg-accent text-white hover:bg-accent-hover'
          }`}
        >
          {connected ? <Unplug className="w-3.5 h-3.5" /> : <Cable className="w-3.5 h-3.5" />}
          {connected ? 'Disconnect' : 'Connect'}
        </button>
        <button
          type="button"
          onClick={() => setDark((d) => !d)}
          className="h-8 w-8 rounded-md flex items-center justify-center text-text-secondary hover:bg-bg-hover"
          aria-label="Toggle theme"
        >
          {dark ? <Sun className="w-4 h-4" /> : <Moon className="w-4 h-4" />}
        </button>
      </header>

      {/* Stage */}
      <main className="flex-1 min-h-0 p-3">
        <div className="h-full rounded-xl bg-bg-secondary ring-1 ring-inset ring-border overflow-hidden flex items-center justify-center relative">
          <canvas
            ref={canvasRef}
            width={640}
            height={360}
            className="max-w-full max-h-full bg-black cursor-crosshair touch-none"
            onContextMenu={(e) => e.preventDefault()}
            onPointerDown={(e) => {
              ;(e.target as HTMLCanvasElement).setPointerCapture(e.pointerId)
              const c = normFromEvent(e)
              if (!c.in) return
              buttonDownRef.current = true
              const button = e.button === 2 ? 'right' : e.button === 1 ? 'middle' : 'left'
              sendAction({ type: 'mousedown', button, x_norm: c.x_norm, y_norm: c.y_norm })
            }}
            onPointerMove={(e) => {
              if (!buttonDownRef.current) return
              const c = normFromEvent(e)
              if (!c.in) return
              sendAction({ type: 'move', held: true, button: 'left', x_norm: c.x_norm, y_norm: c.y_norm })
            }}
            onPointerUp={(e) => {
              if (!buttonDownRef.current) return
              buttonDownRef.current = false
              const c = normFromEvent(e)
              const button = e.button === 2 ? 'right' : e.button === 1 ? 'middle' : 'left'
              sendAction({ type: 'mouseup', button, x_norm: c.x_norm, y_norm: c.y_norm })
            }}
          />
          {!connected && (
            <div className="absolute inset-0 flex items-center justify-center pointer-events-none">
              <div className="text-center space-y-1 px-4">
                <div className="text-sm text-text-muted">Not connected</div>
                <div className="text-xs text-text-tertiary">
                  Open stream gate on agent, then Connect
                </div>
              </div>
            </div>
          )}
        </div>
      </main>

      {/* Bottom status — mirrors GAM BottomBar tone */}
      <footer className="shrink-0 h-8 px-3 flex items-center justify-between border-t border-border bg-bg-secondary text-[11px] text-text-tertiary">
        <span className="truncate">{status}{dims && connected ? '' : ''}</span>
        <span className="shrink-0 tabular-nums">
          {connected ? `${fps} fps` : '—'}
          {latencyMs != null ? ` · ~${latencyMs} ms` : ''}
          {dims ? ` · ${dims}` : ''}
        </span>
      </footer>
    </div>
  )
}
