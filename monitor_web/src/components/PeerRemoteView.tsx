// Peer remote view — WebCodecs H.264 decode + human pointer/keyboard → peer_send_control
import { useEffect, useRef, useState } from 'react'
import { hostCall } from '../lib/bridge'

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

type PeerH264Detail = {
  w?: number
  h?: number
  flags?: number
  bytes: Uint8Array
}

export function PeerRemoteView({
  active,
  humanControl,
}: {
  active: boolean
  humanControl: boolean
}) {
  const canvasRef = useRef<HTMLCanvasElement>(null)
  const decoderRef = useRef<VideoDecoder | null>(null)
  const needKeyRef = useRef(true)
  const videoSizeRef = useRef({ w: 0, h: 0 })
  const buttonDownRef = useRef(false)
  const framesRef = useRef(0)
  const lastFpsTsRef = useRef(performance.now())
  const [fps, setFps] = useState(0)
  const [dims, setDims] = useState('')
  const [status, setStatus] = useState('Waiting for frames…')

  const closeDecoder = () => {
    try { decoderRef.current?.close() } catch { /* */ }
    decoderRef.current = null
  }

  const ensureDecoder = (w: number, h: number) => {
    const canvas = canvasRef.current
    if (!canvas || w <= 0 || h <= 0) return
    if (decoderRef.current && videoSizeRef.current.w === w && videoSizeRef.current.h === h) return
    closeDecoder()
    videoSizeRef.current = { w, h }
    canvas.width = w
    canvas.height = h
    setDims(`${w}×${h}`)
    needKeyRef.current = true
    if (typeof VideoDecoder === 'undefined') {
      setStatus('WebCodecs unavailable')
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
        setStatus('Decoder: ' + e.message)
      },
    })
    decoderRef.current.configure({
      codec: 'avc1.42E028',
      optimizeForLatency: true,
      hardwareAcceleration: 'prefer-hardware',
    })
  }

  useEffect(() => {
    if (!active) {
      closeDecoder()
      setStatus('Idle')
      setFps(0)
      return
    }
    const onFrame = (ev: Event) => {
      const d = (ev as CustomEvent<PeerH264Detail>).detail
      if (!d?.bytes || d.bytes.byteLength < 16) return
      const view = new DataView(d.bytes.buffer, d.bytes.byteOffset, d.bytes.byteLength)
      const w = d.w || view.getUint32(0, true)
      const h = d.h || view.getUint32(4, true)
      const flags = d.flags ?? view.getUint32(8, true)
      const annexb = d.bytes.subarray(16)
      ensureDecoder(w, h)
      const decoder = decoderRef.current
      if (!decoder || decoder.state === 'closed') return
      const key = ((flags & 1) !== 0) || annexbHasIdr(annexb)
      if (needKeyRef.current && !key) {
        hostCall('peer_request_keyframe').catch(() => {})
        return
      }
      if (decoder.decodeQueueSize > 0) return
      try {
        decoder.decode(new EncodedVideoChunk({
          type: key ? 'key' : 'delta',
          timestamp: performance.now() * 1000,
          data: annexb,
        }))
        if (key) {
          needKeyRef.current = false
          setStatus('Live')
        }
      } catch (e: unknown) {
        needKeyRef.current = true
        setStatus('decode: ' + (e instanceof Error ? e.message : String(e)))
      }
    }
    window.addEventListener('peer-h264', onFrame)
    return () => {
      window.removeEventListener('peer-h264', onFrame)
      closeDecoder()
    }
  }, [active])

  useEffect(() => {
    if (!active || !humanControl) return
    const onKeyDown = (e: KeyboardEvent) => {
      if (e.repeat) return
      hostCall('peer_send_control', { type: 'keydown', key: e.key, code: e.code }).catch(() => {})
    }
    const onKeyUp = (e: KeyboardEvent) => {
      hostCall('peer_send_control', { type: 'keyup', key: e.key, code: e.code }).catch(() => {})
    }
    window.addEventListener('keydown', onKeyDown)
    window.addEventListener('keyup', onKeyUp)
    return () => {
      window.removeEventListener('keydown', onKeyDown)
      window.removeEventListener('keyup', onKeyUp)
    }
  }, [active, humanControl])

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

  const send = (action: Record<string, unknown>) => {
    if (!humanControl) return
    hostCall('peer_send_control', action).catch(() => {})
  }

  if (!active) return null

  return (
    <div className="mt-2 rounded-xl bg-bg-secondary ring-1 ring-inset ring-border overflow-hidden">
      <div className="h-7 px-2 flex items-center gap-2 text-[11px] text-text-tertiary border-b border-border">
        <span className="font-medium text-text-secondary">Remote</span>
        <span className="tabular-nums">{dims}</span>
        <span className="tabular-nums">{fps} fps</span>
        <span className="ml-auto truncate">{status}{!humanControl ? ' · AI mode' : ''}</span>
      </div>
      <div className="bg-black flex items-center justify-center min-h-[160px] max-h-[280px]">
        <canvas
          ref={canvasRef}
          width={640}
          height={360}
          className={`max-w-full max-h-[280px] ${humanControl ? 'cursor-crosshair' : 'cursor-default'} touch-none`}
          onContextMenu={(e) => e.preventDefault()}
          onPointerDown={(e) => {
            if (!humanControl) return
            ;(e.target as HTMLCanvasElement).setPointerCapture(e.pointerId)
            const c = normFromEvent(e)
            if (!c.in) return
            buttonDownRef.current = true
            const button = e.button === 2 ? 'right' : e.button === 1 ? 'middle' : 'left'
            send({ type: 'mousedown', button, x_norm: c.x_norm, y_norm: c.y_norm })
          }}
          onPointerMove={(e) => {
            if (!humanControl || !buttonDownRef.current) return
            const c = normFromEvent(e)
            if (!c.in) return
            send({ type: 'move', held: true, button: 'left', x_norm: c.x_norm, y_norm: c.y_norm })
          }}
          onPointerUp={(e) => {
            if (!humanControl) return
            const c = normFromEvent(e)
            buttonDownRef.current = false
            const button = e.button === 2 ? 'right' : e.button === 1 ? 'middle' : 'left'
            send({ type: 'mouseup', button, x_norm: c.x_norm, y_norm: c.y_norm })
          }}
        />
      </div>
    </div>
  )
}
