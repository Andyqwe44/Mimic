// Peer remote view — WebCodecs H.264 decode + platform pointer + soft keyboard.
// Canvas stays mounted across expand/collapse so VideoDecoder never paints a detached context.
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
import {
  annexbHasIdr,
  annexbHasPps,
  annexbHasSps,
  scanAnnexB,
  summarizeSps,
} from '../lib/h264Diag'

/** WebCodecs config — must match encoder Baseline L4.0 for 1080p. */
const DECODER_CODEC = 'avc1.42E028'

type PeerH264Detail = {
  w?: number
  h?: number
  flags?: number
  bytes: Uint8Array
  source?: 'remote' | 'local'
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
  const decodeErrRef = useRef(0)
  const lastKeyTsRef = useRef(0)
  const spsSeenRef = useRef(false)
  const ppsSeenRef = useRef(false)
  const streamCodecRef = useRef('')
  const streamProfileRef = useRef('')
  const flagKeyMismatchRef = useRef(0)
  const lastGapMaxRef = useRef(0)
  const lastDropDeltaRef = useRef(0)
  const lastQueueRef = useRef(0)
  const [diagTick, setDiagTick] = useState(0)

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

  const paintFrame = (frame: VideoFrame) => {
    const canvas = canvasRef.current
    if (!canvas) {
      frame.close()
      return
    }
    const { w, h } = videoSizeRef.current
    if (w > 0 && h > 0 && (canvas.width !== w || canvas.height !== h)) {
      canvas.width = w
      canvas.height = h
    }
    const ctx = canvas.getContext('2d')
    ctx?.drawImage(frame, 0, 0, canvas.width, canvas.height)
    frame.close()
    framesRef.current++
    const now = performance.now()
    if (now - lastFpsTsRef.current >= 1000) {
      setFps(framesRef.current)
      framesRef.current = 0
      lastFpsTsRef.current = now
    }
  }

  const ensureDecoder = (w: number, h: number) => {
    const canvas = canvasRef.current
    if (!canvas || w <= 0 || h <= 0) return
    if (decoderRef.current && videoSizeRef.current.w === w && videoSizeRef.current.h === h) {
      if (canvas.width !== w || canvas.height !== h) {
        canvas.width = w
        canvas.height = h
      }
      return
    }
    closeDecoder()
    videoSizeRef.current = { w, h }
    canvas.width = w
    canvas.height = h
    setDims(`${w}×${h}`)
    setVideoAspect(w / h)
    needKeyRef.current = true
    addLog(`[Decode] configure ${w}x${h} codec=${DECODER_CODEC}`)
    if (typeof VideoDecoder === 'undefined') {
      setStatus(t('peer.webcodecs_unavailable'))
      return
    }
    decoderRef.current = new VideoDecoder({
      output: paintFrame,
      error: (e) => {
        decodeErrRef.current++
        closeDecoder()
        videoSizeRef.current = { w: 0, h: 0 }
        requestKeyframe('error')
        setStatus(t('peer.decoder_error', { msg: e.message }))
        addLog(`[Decode] VideoDecoder error #${decodeErrRef.current}: ${e.message}`)
        setDiagTick((n) => n + 1)
      },
    })
    decoderRef.current.configure({
      codec: DECODER_CODEC,
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
      decodeErrRef.current = 0
      lastKeyTsRef.current = 0
      spsSeenRef.current = false
      ppsSeenRef.current = false
      streamCodecRef.current = ''
      streamProfileRef.current = ''
      flagKeyMismatchRef.current = 0
      lastGapMaxRef.current = 0
      lastDropDeltaRef.current = 0
      lastQueueRef.current = 0
      setExpanded(false)
      setKbOpen(false)
      setLatHint('')
      setDiagTick((n) => n + 1)
      return
    }
    const onFrame = (ev: Event) => {
      const d = (ev as CustomEvent<PeerH264Detail>).detail
      if (!d?.bytes || d.bytes.byteLength < 16) return
      // Global peer-h264 bus — ignore the other path (local preview vs remote).
      const src = d.source || 'remote'
      if (src !== source) return
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

      // Parameter sets / profile — once per stream (or when SPS changes).
      if (annexbHasSps(annexb)) {
        spsSeenRef.current = true
        const sum = summarizeSps(annexb)
        if (sum && sum.codec !== streamCodecRef.current) {
          streamCodecRef.current = sum.codec
          streamProfileRef.current = `${sum.profile}@L${sum.level}`
          addLog(
            `[Decode] SPS ${sum.codec} ${sum.profile} level=${sum.level} ` +
              `cfg=${DECODER_CODEC} match=${sum.codec.toUpperCase() === DECODER_CODEC.toUpperCase()}`,
          )
          setDiagTick((n) => n + 1)
        }
      }
      if (annexbHasPps(annexb)) ppsSeenRef.current = true

      ensureDecoder(w, h)
      const decoder = decoderRef.current
      if (!decoder || decoder.state === 'closed') return
      const flagKey = (flags & 1) !== 0
      const nalKey = annexbHasIdr(annexb)
      if (flagKey !== nalKey) flagKeyMismatchRef.current++
      const key = flagKey || nalKey
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
          lastKeyTsRef.current = nowRecv
          setStatus(t('peer.live'))
          if (keyRecvRef.current <= 3 || keyRecvRef.current % 30 === 0) {
            const nals = scanAnnexB(annexb).map((n) => n.type).join(',')
            addLog(
              `[Decode] IDR #${keyRecvRef.current} ${w}x${h} bytes=${annexb.byteLength} ` +
                `flagKey=${flagKey ? 1 : 0} nalKey=${nalKey ? 1 : 0} ` +
                `sps=${spsSeenRef.current ? 1 : 0} pps=${ppsSeenRef.current ? 1 : 0} nals=[${nals}]`,
            )
          }
        }
      } catch (e: unknown) {
        decodeErrRef.current++
        closeDecoder()
        videoSizeRef.current = { w: 0, h: 0 }
        requestKeyframe('decode')
        const msg = e instanceof Error ? e.message : String(e)
        setStatus(t('peer.decode_error', { msg }))
        addLog(`[Decode] decode() throw #${decodeErrRef.current}: ${msg}`)
        setDiagTick((n) => n + 1)
      }
      const nowDiag = performance.now()
      if (nowDiag - lastDiagTsRef.current >= 1500) {
        lastDiagTsRef.current = nowDiag
        const avgGap = gapNRef.current > 0 ? Math.round(gapSumRef.current / gapNRef.current) : 0
        const avgSkew = skewNRef.current > 0 ? Math.round(skewSumRef.current / skewNRef.current) : 0
        const gapMax = Math.round(gapMaxRef.current)
        const rtt = rttMsRef.current
        const rttPart = rtt != null ? ` rtt=${rtt}` : ''
        const keyAge = lastKeyTsRef.current > 0 ? Math.round(nowDiag - lastKeyTsRef.current) : -1
        const hint =
          `tx=${transportRef.current} gap≈${avgGap}ms jitter≈${avgSkew}ms ` +
          `q=${decoder.decodeQueueSize}${rttPart}`
        setLatHint(hint)
        lastGapMaxRef.current = gapMax
        lastDropDeltaRef.current = deltaDropRef.current
        lastQueueRef.current = decoder.decodeQueueSize
        addLog(
          `[Decode] recv=${recvCountRef.current} idr=${keyRecvRef.current} ` +
            `dropDelta=${deltaDropRef.current} err=${decodeErrRef.current} ` +
            `fps≈${framesRef.current} needKey=${needKeyRef.current} ` +
            `keyAge=${keyAge}ms sps=${spsSeenRef.current ? 1 : 0} pps=${ppsSeenRef.current ? 1 : 0} ` +
            `codec=${streamCodecRef.current || '?'} cfg=${DECODER_CODEC} ` +
            `flagMis=${flagKeyMismatchRef.current} ${hint} gapMax=${gapMax}`,
        )
        deltaDropRef.current = 0
        gapSumRef.current = 0
        gapMaxRef.current = 0
        gapNRef.current = 0
        skewSumRef.current = 0
        skewNRef.current = 0
        setDiagTick((n) => n + 1)
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
    // Canvas stays mounted — no keyframe storm on expand.
    return () => {
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

  // Publish decode/link stats for the external float (never overlays video).
  useEffect(() => {
    if (!active) {
      window.dispatchEvent(new CustomEvent('peer-decode-stats', { detail: { active: false } }))
      return
    }
    const keyAgeMs = lastKeyTsRef.current > 0
      ? Math.round(performance.now() - lastKeyTsRef.current)
      : -1
    window.dispatchEvent(new CustomEvent('peer-decode-stats', {
      detail: {
        active: true,
        source,
        dims,
        fps,
        status,
        latHint,
        encodeHint: encodeHint || '',
        rtt: rttMsRef.current,
        transport: transportRef.current,
        humanControl,
        // Structured H.264 diagnostics (tearing / missing IDR).
        codecCfg: DECODER_CODEC,
        codecStream: streamCodecRef.current || '',
        profile: streamProfileRef.current || '',
        hasSps: spsSeenRef.current,
        hasPps: ppsSeenRef.current,
        idrCount: keyRecvRef.current,
        keyAgeMs,
        needKey: needKeyRef.current,
        dropDelta: lastDropDeltaRef.current,
        gapMax: lastGapMaxRef.current,
        queue: lastQueueRef.current,
        decodeErr: decodeErrRef.current,
        flagKeyMismatch: flagKeyMismatchRef.current,
      },
    }))
  }, [active, source, dims, fps, status, latHint, encodeHint, humanControl, diagTick])

  if (!active) return null

  const portraitVideo = videoAspect > 0 && videoAspect < 1
  const landscapeVideo = videoAspect >= 1
  const viewportPortrait =
    typeof window !== 'undefined' && window.innerHeight > window.innerWidth
  const rotated = expanded && landscapeVideo && viewportPortrait
  const maxPreviewH = portraitVideo ? 'min(72vh, 720px)' : 'min(42vh, 480px)'
  const androidCtl = isAndroidHost()

  const contentW = rotated ? stageBox.h : stageBox.w
  const contentHFull = rotated ? stageBox.w : stageBox.h
  const kbReserve = rotated && kbOpen ? kbH : 0
  const videoAreaH = Math.max(1, contentHFull - kbReserve)
  const fit = fitContain(
    contentW > 0 ? contentW : 1,
    videoAreaH > 0 ? videoAreaH : 1,
    videoAspect > 0 ? videoAspect : 16 / 9,
  )

  const shellStyle: CSSProperties | undefined = expanded
    ? undefined
    : {
        aspectRatio: `${videoAspect}`,
        width: '100%',
        maxHeight: maxPreviewH,
        maxWidth: portraitVideo ? 'min(100%, 420px)' : '100%',
        marginInline: portraitVideo ? 'auto' : undefined,
      }

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

  const titleLabel = source === 'local' ? t('peer.local_preview') : t('peer.remote_view')
  const hudPrimary = [dims || '—', `${fps} fps`].filter(Boolean).join(' · ')

  return (
    <div
      className={
        expanded
          ? 'fixed inset-0 z-[80] bg-black overflow-hidden'
          : 'shrink-0 flex flex-col w-full gap-0'
      }
    >
      <div
        className={
          expanded
            ? 'absolute inset-0 bg-black overflow-hidden'
            : 'relative shrink-0 rounded-t-xl bg-bg-secondary ring-1 ring-inset ring-border overflow-hidden w-full'
        }
        style={shellStyle}
      >
        {expanded && (
          <div
            className="fixed z-[90] flex items-center gap-2 pointer-events-auto"
            style={{
              top: 'max(10px, env(safe-area-inset-top, 0px))',
              right: 'max(10px, env(safe-area-inset-right, 0px))',
            }}
            data-no-page-swipe
          >
            {!rotated && (
              <div className="max-w-[min(55vw,360px)] truncate text-[11px] text-text-muted bg-bg-secondary ring-1 ring-inset ring-border rounded-lg px-2 h-9 flex items-center gap-2">
                <span className="font-medium text-text-secondary shrink-0">{titleLabel}</span>
                <span className="tabular-nums shrink-0">{hudPrimary}</span>
                <span className="truncate">{status}</span>
              </div>
            )}
            {humanControl && (
              <Tooltip text={kbOpen ? t('peer.soft_kb_close') : t('peer.soft_kb_open')}>
                <button
                  type="button"
                  className={`h-9 w-9 rounded-lg flex items-center justify-center shrink-0 bg-bg-secondary ring-1 ring-inset ring-border ${
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
                className="h-9 w-9 rounded-lg flex items-center justify-center shrink-0 bg-bg-secondary ring-1 ring-inset ring-border text-text-secondary"
                onClick={() => setExpanded(false)}
              >
                <Minimize2 className="w-4 h-4" />
              </button>
            </Tooltip>
          </div>
        )}

        <div className="flex flex-col bg-black" style={planeStyle}>
          <div
            ref={stageRef}
            className={
              expanded
                ? 'relative bg-black flex items-center justify-center overflow-hidden flex-1 min-h-0 w-full'
                : 'absolute inset-0 bg-black flex items-center justify-center overflow-hidden'
            }
            data-no-page-swipe={humanControl ? true : undefined}
          >
            {/* Single wrapper — never remount canvas when rotate/expand toggles */}
            <div
              className="flex flex-col items-center justify-center"
              style={
                rotated
                  ? {
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
                    }
                  : {
                      position: 'absolute',
                      inset: 0,
                      width: '100%',
                      height: '100%',
                    }
              }
            >
              {videoStack}
              {rotated ? softKb : null}
            </div>
          </div>
          {expanded && !rotated && !kbOpen && (
            <div className={`${TEXT.tiny} text-center text-text-muted py-1 shrink-0`}>
              {t('peer.expand_hint')}
            </div>
          )}
        </div>

        {expanded && !rotated && softKb && (
          <div className="fixed inset-x-0 bottom-0 z-[95] pointer-events-auto" data-no-page-swipe>
            {softKb}
          </div>
        )}
      </div>

      {/* Status OUTSIDE the video — no overlay on pixels (Android WebView-safe solid tokens) */}
      {!expanded && (
        <div className="rounded-b-xl bg-bg-secondary ring-1 ring-inset ring-border -mt-px shrink-0">
          <div className="h-8 px-2 flex items-center gap-2 text-[11px] text-text-tertiary">
            <span className="font-medium text-text-secondary shrink-0">{titleLabel}</span>
            <span className="tabular-nums text-text-secondary shrink-0">{hudPrimary || '—'}</span>
            <span className="truncate min-w-0 text-text-muted">
              {status}{!humanControl && source !== 'local' ? ` · ${t('peer.ai_mode_short')}` : ''}
            </span>
            <Tooltip text={t('peer.expand_view')}>
              <button
                type="button"
                className="ml-auto h-6 w-6 rounded flex items-center justify-center shrink-0 hover:bg-bg-hover text-text-secondary"
                onClick={() => setExpanded(true)}
              >
                <Expand className="w-3.5 h-3.5" />
              </button>
            </Tooltip>
          </div>
        </div>
      )}
    </div>
  )
}
