// Peer remote view — WebCodecs H.264 decode + platform pointer + soft keyboard.
import { useEffect, useRef, useState, type CSSProperties } from 'react'
import { useTranslation } from 'react-i18next'
import { Expand, Keyboard, Minimize2 } from 'lucide-react'
import { hostCall, addLog } from '../lib/bridge'
import { isAndroidHost } from '../lib/platform'
import { Tooltip } from './Toolkit'
import { VirtualMouseOverlay } from './VirtualMouseOverlay'
import { AbsolutePointerOverlay } from './AbsolutePointerOverlay'
import { SoftKeyboardOverlay } from './SoftKeyboardOverlay'
import { TEXT } from '../lib/design'

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

/** Largest box of `aspect` that fits inside (cw × ch). */
function fitContain(cw: number, ch: number, aspect: number) {
  if (cw <= 0 || ch <= 0 || aspect <= 0) return { w: 0, h: 0 }
  let w = cw
  let h = cw / aspect
  if (h > ch) {
    h = ch
    w = ch * aspect
  }
  return { w: Math.max(1, Math.floor(w)), h: Math.max(1, Math.floor(h)) }
}

export function PeerRemoteView({
  active,
  humanControl,
  fill: _fill = false,
  encodeHint,
  compact: _compact = false,
  source = 'remote',
}: {
  active: boolean
  humanControl: boolean
  fill?: boolean
  encodeHint?: string
  compact?: boolean
  source?: 'remote' | 'local'
}) {
  const { t } = useTranslation()
  const canvasRef = useRef<HTMLCanvasElement>(null)
  const stageRef = useRef<HTMLDivElement>(null)
  const kbWrapRef = useRef<HTMLDivElement>(null)
  const decoderRef = useRef<VideoDecoder | null>(null)
  const needKeyRef = useRef(true)
  const videoSizeRef = useRef({ w: 0, h: 0 })
  const framesRef = useRef(0)
  const lastFpsTsRef = useRef(performance.now())
  const dropCountRef = useRef(0)
  const lastKeyReqRef = useRef(0)
  const recvCountRef = useRef(0)
  const lastDiagTsRef = useRef(0)
  const lastRecvTsRef = useRef(0)
  const gapSumRef = useRef(0)
  const gapMaxRef = useRef(0)
  const gapNRef = useRef(0)
  const lastTsMsRef = useRef(-1)
  const skewSumRef = useRef(0)
  const skewNRef = useRef(0)
  const transportRef = useRef('none')
  const rttMsRef = useRef<number | null>(null)
  const [fps, setFps] = useState(0)
  const [dims, setDims] = useState('')
  const [status, setStatus] = useState(t('peer.waiting_frames'))
  const [videoAspect, setVideoAspect] = useState(16 / 9)
  const [expanded, setExpanded] = useState(false)
  const [kbOpen, setKbOpen] = useState(false)
  const [kbH, setKbH] = useState(0)
  const [stageBox, setStageBox] = useState({ w: 0, h: 0 })
  const [latHint, setLatHint] = useState('')
  const keyRecvRef = useRef(0)
  const deltaDropRef = useRef(0)

  const requestKeyframe = (reason: string) => {
    const now = performance.now()
    if (now - lastKeyReqRef.current < 150) return
    lastKeyReqRef.current = now
    needKeyRef.current = true
    if (source === 'local') {
      hostCall('local_request_keyframe').catch(() => {})
    } else {
      hostCall('peer_request_keyframe').catch(() => {})
    }
    setStatus(t('peer.waiting_keyframe', { reason }))
    addLog(`[Decode] need_key reason=${reason} source=${source}`)
  }

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
    setVideoAspect(w / h)
    needKeyRef.current = true
    addLog(`[Decode] configure ${w}x${h} codec=avc1.42E028`)
    if (typeof VideoDecoder === 'undefined') {
      setStatus(t('peer.webcodecs_unavailable'))
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
        closeDecoder()
        videoSizeRef.current = { w: 0, h: 0 }
        requestKeyframe('error')
        setStatus(t('peer.decoder_error', { msg: e.message }))
        addLog(`[Decode] VideoDecoder error: ${e.message}`)
      },
    })
    decoderRef.current.configure({
      codec: 'avc1.42E028',
      optimizeForLatency: true,
      hardwareAcceleration: 'prefer-hardware',
    })
  }

  useEffect(() => {
    const onLink = (ev: Event) => {
      const d = (ev as CustomEvent<{ transport?: string; rtt_ms?: number | null }>).detail
      if (!d) return
      if (typeof d.transport === 'string') transportRef.current = d.transport
      if (d.rtt_ms !== undefined) rttMsRef.current = d.rtt_ms
    }
    window.addEventListener('peer-link-stats', onLink)
    return () => window.removeEventListener('peer-link-stats', onLink)
  }, [])

  useEffect(() => {
    if (!active) {
      closeDecoder()
      setStatus(t('peer.idle'))
      setFps(0)
      dropCountRef.current = 0
      recvCountRef.current = 0
      lastRecvTsRef.current = 0
      gapSumRef.current = 0
      gapMaxRef.current = 0
      gapNRef.current = 0
      lastTsMsRef.current = -1
      skewSumRef.current = 0
      skewNRef.current = 0
      setExpanded(false)
      setKbOpen(false)
      setLatHint('')
      return
    }
    const onFrame = (ev: Event) => {
      const d = (ev as CustomEvent<PeerH264Detail>).detail
      if (!d?.bytes || d.bytes.byteLength < 16) return
      const view = new DataView(d.bytes.buffer, d.bytes.byteOffset, d.bytes.byteLength)
      const w = d.w || view.getUint32(0, true)
      const h = d.h || view.getUint32(4, true)
      const flags = d.flags ?? view.getUint32(8, true)
      const tsMs = view.getUint32(12, true)
      const annexb = d.bytes.subarray(16)
      const nowRecv = performance.now()
      let gap = 0
      if (lastRecvTsRef.current > 0) {
        gap = nowRecv - lastRecvTsRef.current
        gapSumRef.current += gap
        gapNRef.current++
        if (gap > gapMaxRef.current) gapMaxRef.current = gap
      }
      if (lastTsMsRef.current >= 0 && gap > 0) {
        const encDelta = (tsMs - lastTsMsRef.current + 0x100000000) % 0x100000000
        if (encDelta > 0 && encDelta < 5000) {
          skewSumRef.current += Math.abs(gap - encDelta)
          skewNRef.current++
        }
      }
      lastRecvTsRef.current = nowRecv
      lastTsMsRef.current = tsMs

      ensureDecoder(w, h)
      const decoder = decoderRef.current
      if (!decoder || decoder.state === 'closed') return
      const key = ((flags & 1) !== 0) || annexbHasIdr(annexb)
      recvCountRef.current++
      if (needKeyRef.current && !key) {
        requestKeyframe('sync')
        deltaDropRef.current++
        return
      }
      if (!key && decoder.decodeQueueSize > 10) {
        dropCountRef.current++
        deltaDropRef.current++
        if (dropCountRef.current >= 4) {
          dropCountRef.current = 0
          try { decoder.flush() } catch { /* */ }
          requestKeyframe('backpressure')
        }
        return
      }
      try {
        decoder.decode(new EncodedVideoChunk({
          type: key ? 'key' : 'delta',
          timestamp: performance.now() * 1000,
          data: annexb,
        }))
        if (key) {
          needKeyRef.current = false
          dropCountRef.current = 0
          keyRecvRef.current++
          setStatus(t('peer.live'))
          if (keyRecvRef.current <= 3 || keyRecvRef.current % 30 === 0) {
            addLog(`[Decode] IDR #${keyRecvRef.current} ${w}x${h} bytes=${annexb.byteLength}`)
          }
        }
      } catch (e: unknown) {
        closeDecoder()
        videoSizeRef.current = { w: 0, h: 0 }
        requestKeyframe('decode')
        const msg = e instanceof Error ? e.message : String(e)
        setStatus(t('peer.decode_error', { msg }))
        addLog(`[Decode] decode() throw: ${msg}`)
      }
      const nowDiag = performance.now()
      if (nowDiag - lastDiagTsRef.current >= 1500) {
        lastDiagTsRef.current = nowDiag
        const avgGap = gapNRef.current > 0 ? Math.round(gapSumRef.current / gapNRef.current) : 0
        const avgSkew = skewNRef.current > 0 ? Math.round(skewSumRef.current / skewNRef.current) : 0
        const rtt = rttMsRef.current
        const rttPart = rtt != null ? ` rtt=${rtt}` : ''
        const hint = `tx=${transportRef.current} gap≈${avgGap}ms jitter≈${avgSkew}ms q=${decoder.decodeQueueSize}${rttPart}`
        setLatHint(hint)
        addLog(
          `[Decode] recv=${recvCountRef.current} idr=${keyRecvRef.current} dropDelta=${deltaDropRef.current} fps≈${framesRef.current} needKey=${needKeyRef.current} ${hint} gapMax=${Math.round(gapMaxRef.current)}`,
        )
        deltaDropRef.current = 0
        gapSumRef.current = 0
        gapMaxRef.current = 0
        gapNRef.current = 0
        skewSumRef.current = 0
        skewNRef.current = 0
      }
    }
    window.addEventListener('peer-h264', onFrame)
    return () => {
      window.removeEventListener('peer-h264', onFrame)
      closeDecoder()
    }
  }, [active, t, source])

  useEffect(() => {
    if (!expanded) {
      setKbOpen(false)
      try {
        const o = screen.orientation as ScreenOrientation & { unlock?: () => void }
        o?.unlock?.()
      } catch { /* */ }
      return
    }
    const tmr = window.setTimeout(() => requestKeyframe('expand'), 80)
    return () => {
      window.clearTimeout(tmr)
      try {
        const o = screen.orientation as ScreenOrientation & { unlock?: () => void }
        o?.unlock?.()
      } catch { /* */ }
    }
  }, [expanded])

  useEffect(() => {
    if (!kbOpen) {
      setKbH(0)
      return
    }
    const el = kbWrapRef.current
    if (!el) return
    const measure = () => setKbH(el.offsetHeight)
    measure()
    const ro = new ResizeObserver(measure)
    ro.observe(el)
    return () => ro.disconnect()
  }, [kbOpen, expanded])

  useEffect(() => {
    const el = stageRef.current
    if (!el) return
    const update = () => setStageBox({ w: el.clientWidth, h: el.clientHeight })
    update()
    const ro = new ResizeObserver(update)
    ro.observe(el)
    return () => ro.disconnect()
  }, [expanded, kbH, videoAspect])

  const send = (action: Record<string, unknown>) => {
    if (!humanControl) return
    hostCall('peer_send_control', action).catch(() => {})
  }

  const sendKey = (type: 'keydown' | 'keyup', key: string, code: string) => {
    if (!humanControl) return
    hostCall('peer_send_control', { type, key, code }).catch(() => {})
  }

  if (!active) return null

  const portraitVideo = videoAspect > 0 && videoAspect < 1
  const landscapeVideo = videoAspect >= 1
  const viewportPortrait =
    typeof window !== 'undefined' && window.innerHeight > window.innerWidth
  const rotated = expanded && landscapeVideo && viewportPortrait
  // Portrait remote (typical Android app): taller preview on PC; landscape: wider/shorter.
  const maxPreviewH = portraitVideo ? 'min(72vh, 720px)' : 'min(42vh, 480px)'
  const androidCtl = isAndroidHost()

  // Rotated: pre-rotate box = (stageH × stageW). Keyboard lives inside when open.
  const contentW = rotated ? stageBox.h : stageBox.w
  const contentHFull = rotated ? stageBox.w : stageBox.h
  const kbReserve = rotated && kbOpen ? kbH : 0
  const videoAreaH = Math.max(1, contentHFull - kbReserve)
  const fit = fitContain(
    contentW > 0 ? contentW : 1,
    videoAreaH > 0 ? videoAreaH : 1,
    videoAspect > 0 ? videoAspect : 16 / 9,
  )

  const shellClass = expanded
    ? 'fixed inset-0 z-[80] bg-black overflow-hidden'
    : 'shrink-0 rounded-t-xl bg-bg-secondary ring-1 ring-inset ring-border overflow-hidden w-full'

  const shellStyle: CSSProperties | undefined = expanded
    ? undefined
    : {
        aspectRatio: `${videoAspect}`,
        width: '100%',
        maxHeight: maxPreviewH,
        maxWidth: portraitVideo ? 'min(100%, 420px)' : '100%',
        marginInline: portraitVideo ? 'auto' : undefined,
      }

  // Non-rotated: push plane up with fixed keyboard. Rotated: keyboard inside rotate box.
  const planeBottom = !rotated && kbOpen ? kbH : 0
  const planeStyle: CSSProperties = expanded
    ? {
        position: 'absolute',
        top: 0,
        left: 0,
        right: 0,
        bottom: planeBottom,
        display: 'flex',
        flexDirection: 'column',
      }
    : { display: 'flex', flexDirection: 'column', width: '100%', height: '100%' }

  const softKb = humanControl && expanded && kbOpen ? (
    <div ref={kbWrapRef} className="shrink-0 w-full pointer-events-auto" data-no-page-swipe>
      <SoftKeyboardOverlay
        open={kbOpen}
        onClose={() => setKbOpen(false)}
        onKey={sendKey}
      />
    </div>
  ) : null

  const controlOverlay = humanControl && expanded ? (
    androidCtl ? (
      <VirtualMouseOverlay
        enabled
        videoAspect={videoAspect}
        rotated={rotated}
        showPanel
        fitWidth={fit.w}
        fitHeight={fit.h}
        onAction={send}
      />
    ) : (
      <AbsolutePointerOverlay
        enabled
        rotated={rotated}
        fitWidth={fit.w}
        fitHeight={fit.h}
        onAction={send}
      />
    )
  ) : null

  const videoStack = (
    <div
      className="relative flex items-center justify-center overflow-hidden flex-1 min-h-0 w-full h-full"
      style={rotated ? { flex: '1 1 0', minHeight: 0 } : undefined}
    >
      <canvas
        ref={canvasRef}
        width={640}
        height={360}
        className="pointer-events-none shrink-0"
        style={
          fit.w >= 8 && fit.h >= 8
            ? { width: fit.w, height: fit.h }
            : { width: '100%', height: '100%' }
        }
      />
      {controlOverlay}
    </div>
  )

  const statusRow = (
    <div className="h-7 px-2 flex items-center gap-2 text-[11px] text-text-tertiary shrink-0">
      <span className="font-medium text-text-secondary">
        {source === 'local' ? t('peer.local_preview') : t('peer.remote_view')}
      </span>
      <span className="tabular-nums">{dims}</span>
      <span className="tabular-nums">{fps} fps</span>
      {encodeHint && (
        <span className="text-amber-500 truncate">{encodeHint}</span>
      )}
      {latHint && (
        <span className="tabular-nums text-text-muted truncate min-w-0">{latHint}</span>
      )}
      <span className="ml-auto truncate min-w-0">
        {status}{!humanControl ? ` · ${t('peer.ai_mode_short')}` : ''}
      </span>
      {!expanded && (
        <Tooltip text={t('peer.expand_view')}>
          <button
            type="button"
            className="h-6 w-6 rounded flex items-center justify-center shrink-0 hover:bg-bg-hover text-text-secondary"
            onClick={() => setExpanded(true)}
          >
            <Expand className="w-3.5 h-3.5" />
          </button>
        </Tooltip>
      )}
    </div>
  )

  return (
    <div className={expanded ? shellClass : 'shrink-0 flex flex-col w-full gap-0'}>
      {expanded ? (
        <div className={shellClass} style={shellStyle}>
          <div
            className="fixed z-[90] flex items-center gap-2 pointer-events-auto"
            style={{
              top: 'max(10px, env(safe-area-inset-top, 0px))',
              right: 'max(10px, env(safe-area-inset-right, 0px))',
            }}
            data-no-page-swipe
          >
            {!rotated && (
              <div className="max-w-[min(50vw,320px)] truncate text-[11px] text-text-muted bg-bg-secondary/90 ring-1 ring-inset ring-border rounded-lg px-2 h-9 flex items-center">
                <span className="tabular-nums mr-2">{fps} fps</span>
                <span className="truncate">{status}</span>
              </div>
            )}
            {humanControl && (
              <Tooltip text={kbOpen ? t('peer.soft_kb_close') : t('peer.soft_kb_open')}>
                <button
                  type="button"
                  className={`h-9 w-9 rounded-lg flex items-center justify-center shrink-0 bg-bg-secondary/90 ring-1 ring-inset ring-border ${
                    kbOpen ? 'text-accent' : 'text-text-secondary'
                  }`}
                  onClick={() => setKbOpen((v) => !v)}
                >
                  <Keyboard className="w-4 h-4" />
                </button>
              </Tooltip>
            )}
            <Tooltip text={t('peer.collapse_view')}>
              <button
                type="button"
                className="h-9 w-9 rounded-lg flex items-center justify-center shrink-0 bg-bg-secondary/90 ring-1 ring-inset ring-border text-text-secondary"
                onClick={() => setExpanded(false)}
              >
                <Minimize2 className="w-4 h-4" />
              </button>
            </Tooltip>
          </div>
          <div className="flex flex-col bg-black" style={planeStyle}>
            <div
              ref={stageRef}
              className="relative bg-black flex items-center justify-center overflow-hidden flex-1 min-h-0 w-full"
              data-no-page-swipe={humanControl ? true : undefined}
            >
              {rotated ? (
                <div
                  className="flex flex-col"
                  style={{
                    position: 'absolute',
                    width: contentW > 0 ? contentW : '100%',
                    height: contentHFull > 0 ? contentHFull : '100%',
                    left: '50%',
                    top: '50%',
                    transform: 'translate(-50%, -50%) rotate(90deg)',
                    transformOrigin: 'center center',
                    flexShrink: 0,
                    minWidth: contentW > 0 ? contentW : undefined,
                    minHeight: contentHFull > 0 ? contentHFull : undefined,
                  }}
                >
                  {videoStack}
                  {softKb}
                </div>
              ) : (
                <div
                  className="flex items-center justify-center"
                  style={{ position: 'absolute', inset: 0, width: '100%', height: '100%' }}
                >
                  {videoStack}
                </div>
              )}
            </div>
            {expanded && !rotated && !kbOpen && (
              <div className={`${TEXT.tiny} text-center text-text-muted py-1 shrink-0`}>
                {t('peer.expand_hint')}
              </div>
            )}
          </div>
          {!rotated && softKb && (
            <div className="fixed inset-x-0 bottom-0 z-[95] pointer-events-auto" data-no-page-swipe>
              {softKb}
            </div>
          )}
        </div>
      ) : (
        <>
          <div className={shellClass} style={{ ...shellStyle, position: 'relative' }}>
            <div
              ref={stageRef}
              className="absolute inset-0 bg-black flex items-center justify-center overflow-hidden"
              data-no-page-swipe={humanControl ? true : undefined}
            >
              {videoStack}
            </div>
          </div>
          {/* Status below preview — does not steal video height */}
          <div className="rounded-b-xl bg-bg-secondary ring-1 ring-inset ring-border border-t-0 -mt-px">
            {statusRow}
          </div>
        </>
      )}
    </div>
  )
}
