// Floating link/decode stats — sits beside the preview, never covers video pixels.
import { useEffect, useRef, useState } from 'react'
import { useTranslation } from 'react-i18next'
import { Activity, GripHorizontal } from 'lucide-react'
import { TEXT, RADIUS, RING } from '../lib/design'

type Stats = {
  active?: boolean
  dims?: string
  fps?: number
  status?: string
  latHint?: string
  encodeHint?: string
  rtt?: number | null
  transport?: string
  codecCfg?: string
  codecStream?: string
  profile?: string
  hasSps?: boolean
  hasPps?: boolean
  idrCount?: number
  keyAgeMs?: number
  needKey?: boolean
  dropDelta?: number
  gapMax?: number
  queue?: number
  decodeErr?: number
  flagKeyMismatch?: number
}

function parseLatHint(hint: string) {
  // tx=lan gap≈48ms jitter≈38ms q=0 rtt=31
  const gap = /gap≈(\d+)ms/.exec(hint)?.[1]
  const jitter = /jitter≈(\d+)ms/.exec(hint)?.[1]
  const q = /q=(\d+)/.exec(hint)?.[1]
  return { gap, jitter, q }
}

function Row({ label, value, warn }: { label: string; value: string; warn?: boolean }) {
  return (
    <div className="flex justify-between gap-2">
      <span className="text-text-muted shrink-0">{label}</span>
      <span className={`truncate text-right ${warn ? 'text-amber-500' : ''}`}>{value}</span>
    </div>
  )
}

export function LinkStatsFloat({ visible }: { visible: boolean }) {
  const { t } = useTranslation()
  const [st, setSt] = useState<Stats | null>(null)
  const [pos, setPos] = useState({ x: 12, y: 72 })
  const dragRef = useRef<{ pointerId: number; dx: number; dy: number } | null>(null)
  const gripRef = useRef<HTMLDivElement>(null)

  useEffect(() => {
    const onStats = (ev: Event) => {
      const d = (ev as CustomEvent<Stats>).detail
      if (!d) return
      if (d.active === false) {
        setSt(null)
        return
      }
      setSt(d)
    }
    window.addEventListener('peer-decode-stats', onStats)
    return () => window.removeEventListener('peer-decode-stats', onStats)
  }, [])

  // Android WebView: window-level pointermove is unreliable after leave-grip.
  // Capture on the grip element (same pattern as AbsolutePointerOverlay / PagePager).
  useEffect(() => {
    const el = gripRef.current
    if (!el) return
    const onMove = (e: PointerEvent) => {
      const d = dragRef.current
      if (!d || e.pointerId !== d.pointerId) return
      setPos({
        x: Math.max(4, Math.min(window.innerWidth - 180, e.clientX - d.dx)),
        y: Math.max(4, Math.min(window.innerHeight - 200, e.clientY - d.dy)),
      })
    }
    const onUp = (e: PointerEvent) => {
      const d = dragRef.current
      if (!d || e.pointerId !== d.pointerId) return
      dragRef.current = null
      try { el.releasePointerCapture(e.pointerId) } catch { /* */ }
    }
    el.addEventListener('pointermove', onMove)
    el.addEventListener('pointerup', onUp)
    el.addEventListener('pointercancel', onUp)
    return () => {
      el.removeEventListener('pointermove', onMove)
      el.removeEventListener('pointerup', onUp)
      el.removeEventListener('pointercancel', onUp)
    }
  }, [visible, st])

  if (!visible || !st) return null

  const parsed = parseLatHint(st.latHint || '')
  const rtt = st.rtt != null ? `${Math.round(st.rtt)}` : '—'
  const tx = st.transport && st.transport !== 'none' ? st.transport : '—'
  const codecStream = st.codecStream || '—'
  const codecCfg = st.codecCfg || '—'
  const codecMismatch =
    !!st.codecStream && !!st.codecCfg
    && st.codecStream.toUpperCase() !== st.codecCfg.toUpperCase()
  const keyAge = st.keyAgeMs != null && st.keyAgeMs >= 0 ? `${st.keyAgeMs}` : '—'
  const keyAgeWarn = (st.keyAgeMs != null && st.keyAgeMs > 2500) || !!st.needKey
  const spsPps = `${st.hasSps ? 'SPS' : '—'}+${st.hasPps ? 'PPS' : '—'}`
  const spsWarn = st.hasSps === false || st.hasPps === false
  const q = st.queue != null ? String(st.queue) : (parsed.q ?? '—')
  const drop = st.dropDelta != null ? String(st.dropDelta) : '—'
  const gapMax = st.gapMax != null ? `${st.gapMax}` : '—'
  const err = st.decodeErr != null ? String(st.decodeErr) : '0'
  const errWarn = (st.decodeErr ?? 0) > 0

  return (
    <div
      className={`fixed z-[60] ${RADIUS.lg} bg-bg-secondary ${RING} shadow-lg w-[168px] select-none`}
      style={{ left: pos.x, top: pos.y }}
      data-no-page-swipe
    >
      <div
        ref={gripRef}
        className="flex items-center gap-1 px-2 h-7 border-b border-border cursor-grab active:cursor-grabbing touch-none"
        onPointerDown={(e) => {
          e.preventDefault()
          e.stopPropagation()
          dragRef.current = { pointerId: e.pointerId, dx: e.clientX - pos.x, dy: e.clientY - pos.y }
          try { e.currentTarget.setPointerCapture(e.pointerId) } catch { /* */ }
        }}
      >
        <Activity className="w-3.5 h-3.5 text-accent shrink-0" />
        <span className={`${TEXT.tiny} font-medium text-text-secondary truncate`}>
          {t('peer.link_stats_title')}
        </span>
        <GripHorizontal className="w-3.5 h-3.5 text-text-muted ml-auto shrink-0" />
      </div>
      <div className={`px-2 py-1.5 space-y-0.5 ${TEXT.tiny} tabular-nums text-text-secondary`}>
        <Row label={t('peer.link_rtt')} value={`${rtt} ms`} />
        <Row label={t('peer.link_gap')} value={parsed.gap != null ? `${parsed.gap} ms` : '—'} />
        <Row label={t('peer.link_gap_max')} value={`${gapMax} ms`} warn={(st.gapMax ?? 0) > 120} />
        <Row label={t('peer.link_jitter')} value={parsed.jitter != null ? `${parsed.jitter} ms` : '—'} />
        <Row label="FPS" value={String(st.fps ?? 0)} />
        <Row label={t('peer.link_tx')} value={tx} />
        <Row label={t('peer.link_codec')} value={codecStream} warn={codecMismatch} />
        <Row label={t('peer.link_codec_cfg')} value={codecCfg} />
        {st.profile ? <Row label={t('peer.link_profile')} value={st.profile} /> : null}
        <Row label={t('peer.link_sps_pps')} value={spsPps} warn={spsWarn} />
        <Row
          label={t('peer.link_idr')}
          value={`${st.idrCount ?? 0} · ${keyAge}ms`}
          warn={keyAgeWarn}
        />
        <Row label={t('peer.link_drop')} value={drop} warn={(st.dropDelta ?? 0) > 0} />
        <Row label={t('peer.link_queue')} value={q} warn={(st.queue ?? 0) > 6} />
        <Row label={t('peer.link_err')} value={err} warn={errWarn} />
        {(st.flagKeyMismatch ?? 0) > 0 ? (
          <Row label={t('peer.link_flag_mis')} value={String(st.flagKeyMismatch)} warn />
        ) : null}
        {st.encodeHint ? (
          <div className="text-amber-500 truncate pt-0.5">{st.encodeHint}</div>
        ) : null}
      </div>
    </div>
  )
}
