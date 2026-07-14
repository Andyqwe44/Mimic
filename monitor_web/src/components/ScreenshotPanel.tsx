// ═══ Screenshot Panel — canvas + snapshot/stream controls ═══
// Two modes: bare (canvas-only, used in MonitorView) and full (sidebar card).
// SharedBuffer handler receives BGRA→RGBA frames from C++ and renders to canvas.
import { useState, useEffect, useRef } from 'react'
import { Camera, Play, Square, ChevronDown, Pin } from 'lucide-react'
import { Tooltip } from './Toolkit'
import { useTranslation } from 'react-i18next'
import { COLLAPSIBLE_HEADER, METHOD_SHORT } from '../lib/constants'
import { addLog } from '../lib/bridge'
import type { WindowInfo } from '../lib/types'

export function ScreenshotPanel({
  screenRatio,
  expanded,
  onToggle,
  previewing,
  previewingRef,
  snapshotRef,
  snapshotStartRef,
  capMethod,
  onTakeSnapshot,
  onTogglePreview,
  pinned,
  onTogglePin,
  hasContentRef,
  bare,
  onFps,
  onDims,
}: {
  selWin?: WindowInfo
  screenRatio: number
  snapMethod: string
  streamMethod: string
  renderMethod: string
  winState: string
  expanded: boolean
  onToggle: () => void
  previewing: boolean
  previewingRef: React.MutableRefObject<boolean>
  snapshotRef: React.MutableRefObject<boolean>
  snapshotStartRef: React.MutableRefObject<number>
  capMethod: string
  onTakeSnapshot: () => void
  onTogglePreview: () => void
  pinned: boolean
  onTogglePin: () => void
  hasContentRef: React.MutableRefObject<boolean>
  bare?: boolean
  onFps?: (fps: number) => void
  onDims?: (w: number, h: number) => void
}) {
  const { t } = useTranslation()

  // ── Canvas state ──
  const [hasContent, setHasContent] = useState(false)
  useEffect(() => {
    hasContentRef.current = hasContent
  }, [hasContent])
  const [snapshotLatency, setSnapshotLatency] = useState<number | null>(null)
  const [fps, setFps] = useState(0)
  const framesRef = useRef(0)           // frame counter for FPS calculation
  const lastFpsRef = useRef(Date.now())  // last FPS sample timestamp
  const canvasRef = useRef<HTMLCanvasElement>(null)
  const [canvasDims, setCanvasDims] = useState({ w: 0, h: 0 })

  // ── Reset FPS when preview toggles ──
  useEffect(() => {
    setFps(0)
    framesRef.current = 0
    lastFpsRef.current = Date.now()
    if (previewing) setSnapshotLatency(null)
  }, [previewing])

  // Report FPS to parent
  useEffect(() => {
    if (onFps) onFps(fps)
  }, [fps, onFps])

  // Report dims to parent
  useEffect(() => {
    if (onDims && canvasDims.w > 0 && canvasDims.h > 0) {
      onDims(canvasDims.w, canvasDims.h)
    }
  }, [canvasDims, onDims])

  // ── SharedBuffer → Canvas render handler ──
  // C++ pushes BGRA frame via PostSharedBufferToScript → 'sharedbufferreceived' event.
  // Converts to ImageData → putImageData. Counts frames for FPS. Detects snapshot latency.
  useEffect(() => {
    const wv = (window as any).chrome?.webview
    if (!wv) {
      addLog('[Screenshot] SharedBuffer not available')
      return
    }
    const handler = (e: any) => {
      const active = previewingRef.current || snapshotRef.current
      if (!active) return
      try {
        const buf: ArrayBuffer = e.getBuffer()
        const metaRaw = e.additionalData
        const meta: { w: number; h: number } =
          typeof metaRaw === 'string' ? JSON.parse(metaRaw) : metaRaw
        if (!meta.w || !meta.h || meta.w <= 0 || meta.h <= 0) return
        const pixelCount = meta.w * meta.h * 4
        if (buf.byteLength < pixelCount) return
        // Create ImageData from SharedBuffer — BGRA already swapped to RGBA by C++
        const imgData = new ImageData(
          new Uint8ClampedArray(buf, 0, pixelCount),
          meta.w,
          meta.h,
        )
        const c = canvasRef.current
        if (c) {
          c.width = meta.w
          c.height = meta.h
          setCanvasDims({ w: meta.w, h: meta.h })
          const ctx = c.getContext('2d')
          if (ctx) ctx.putImageData(imgData, 0, 0)
        }
        setHasContent(true)
        // ── FPS counter: sample every 1s ──
        framesRef.current++
        const now = Date.now()
        const elapsed = now - lastFpsRef.current
        if (elapsed >= 1000) {
          setFps(Math.round((framesRef.current * 1000) / elapsed))
          framesRef.current = 0
          lastFpsRef.current = now
        }
        if (snapshotRef.current) {
          snapshotRef.current = false
          // ── Snapshot latency: button press → frame rendered ──
          const latency = Date.now() - snapshotStartRef.current
          setSnapshotLatency(latency)
        }
      } catch (ex: any) {
        addLog(`[SB] EXCEPTION: ${ex?.message || ex}`)
      }
    }
    wv.addEventListener('sharedbufferreceived', handler)
    return () => {
      wv.removeEventListener('sharedbufferreceived', handler)
    }
  }, [])

  // ── Bare mode: canvas only, no chrome ──
  const canvasEl = (
    <canvas
      ref={canvasRef}
      className={`max-w-full max-h-full object-contain ${hasContent ? '' : 'hidden'}`}
      style={{
        aspectRatio:
          canvasDims.w && canvasDims.h ? `${canvasDims.w}/${canvasDims.h}` : '16/9',
      }}
    />
  )

  if (bare) {
    return (
      <div className="w-full h-full rounded-lg bg-bg-primary overflow-hidden flex items-center justify-center">
        {canvasEl}
      </div>
    )
  }

  // ── Full panel mode (sidebar) ──
  return (
    <div className="bg-bg-secondary rounded-xl ring-1 ring-inset ring-border overflow-hidden">
      <div
        role="button"
        tabIndex={0}
        onClick={() => {
          onToggle()
          addLog(`[Screenshot] ${!expanded ? 'expanded' : 'collapsed'}`)
        }}
        onKeyDown={(e) => {
          if (e.key === 'Enter' || e.key === ' ') {
            ;(e.currentTarget as HTMLElement).click()
          }
        }}
        className={COLLAPSIBLE_HEADER}
      >
        <div className="flex items-center gap-2 min-w-0">
          <span className="w-5 h-5 rounded bg-violet-400/15 flex items-center justify-center shrink-0">
            <Camera className="w-3 h-3 text-violet-400" />
          </span>
          <span className="text-sm font-medium text-text-primary shrink-0">{t('screenshot.title')}</span>
          {capMethod && (
            <span className="text-[11px] font-medium text-accent bg-accent/10 px-1.5 py-0.5 rounded shrink-0">
              {METHOD_SHORT[capMethod] || capMethod.toUpperCase()}
            </span>
          )}
          {previewing && (
            <span className="text-xs text-text-muted shrink-0">{fps} FPS</span>
          )}
          {snapshotLatency !== null && !previewing && (
            <span className="text-xs text-text-muted shrink-0">{snapshotLatency}ms</span>
          )}
        </div>
        <div className="flex items-center gap-0.5 shrink-0">
          <Tooltip text={t('screenshot.snapshot_tip')}>
            <button
              onClick={(e) => {
                e.stopPropagation()
                onTakeSnapshot()
              }}
              className="p-1 rounded-md text-text-secondary hover:text-text-primary hover:bg-bg-tertiary transition-colors"
            >
              <Camera className="w-3.5 h-3.5" />
            </button>
          </Tooltip>
          {previewing ? (
            <Tooltip text={t('screenshot.stop_preview_tip')}>
              <button
                onClick={(e) => {
                  e.stopPropagation()
                  onTogglePreview()
                }}
                className="p-1 rounded-md text-success hover:bg-bg-tertiary transition-colors"
              >
                <Square className="w-3.5 h-3.5" />
              </button>
            </Tooltip>
          ) : (
            <Tooltip text={t('screenshot.start_preview_tip')}>
              <button
                onClick={(e) => {
                  e.stopPropagation()
                  onTogglePreview()
                }}
                className="p-1 rounded-md text-text-secondary hover:text-text-primary hover:bg-bg-tertiary transition-colors"
              >
                <Play className="w-3.5 h-3.5" />
              </button>
            </Tooltip>
          )}
          <Tooltip text={pinned ? t('screenshot.unpin_tip') : t('screenshot.pin_tip')}>
            <button
              onClick={(e) => {
                e.stopPropagation()
                onTogglePin()
              }}
              className={`p-1 rounded-md transition-colors ${pinned ? 'text-accent hover:bg-bg-tertiary' : 'text-text-secondary hover:text-text-primary hover:bg-bg-tertiary'}`}
            >
              <Pin className={`w-3.5 h-3.5 ${pinned ? 'fill-current' : ''}`} />
            </button>
          </Tooltip>
          <ChevronDown
            className={`w-4 h-4 text-text-muted transition-transform duration-150 ${expanded ? 'rotate-180' : ''}`}
          />
        </div>
      </div>
      <div
        className="grid transition-[grid-template-rows] duration-150 ease-out"
        style={{ gridTemplateRows: expanded ? '1fr' : '0fr' }}
      >
        <div className="overflow-hidden min-h-0" data-layout-measure="">
          <div className="border-t border-border" />
          <div className="p-3">
            <div
              className="w-full rounded-lg bg-bg-primary overflow-hidden flex items-center justify-center"
              style={{ aspectRatio: screenRatio }}
            >
              {canvasEl}
              {!hasContent && (
                <span className="text-sm text-text-muted">
                  {t('screenshot.empty_hint')}
                </span>
              )}
            </div>
          </div>
        </div>
      </div>
    </div>
  )
}
