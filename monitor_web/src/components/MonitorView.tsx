// ═══ Monitor View — main workspace with large preview + input forwarding ═══
import { useState, useRef, useCallback, useEffect } from 'react'
import { Camera, Play, Square, MousePointer2 } from 'lucide-react'
import { Tooltip } from './Toolkit'
import { METHOD_SHORT, STATE_LABEL, STATE_COLOR } from '../lib/constants'
import { addLog, hostCall } from '../lib/bridge'
import type { WindowInfo } from '../lib/types'

// ── Types ──
interface Ripple { id: number; x: number; y: number }
interface KeyToast { text: string; id: number }
interface PressedKey { key: string; code: string; keyCode: number }

// ── Coordinate helper ──
// Compute normalized (0-1) coords relative to actual image area, accounting for letterbox
function getImageCoords(
  clientX: number, clientY: number,
  containerRect: DOMRect,
  imageW: number, imageH: number,
): { rx: number; ry: number; inImage: boolean } {
  const cw = containerRect.width
  const ch = containerRect.height
  if (cw <= 0 || ch <= 0 || imageW <= 0 || imageH <= 0) {
    return { rx: 0, ry: 0, inImage: false }
  }
  const imgAspect = imageW / imageH
  const containerAspect = cw / ch

  let iw: number, ih: number, ox: number, oy: number
  if (containerAspect > imgAspect) {
    // container wider → letterbox on left/right
    ih = ch
    iw = ch * imgAspect
    ox = (cw - iw) / 2
    oy = 0
  } else {
    // container taller → letterbox on top/bottom
    iw = cw
    ih = cw / imgAspect
    ox = 0
    oy = (ch - ih) / 2
  }

  const rx = (clientX - containerRect.left - ox) / iw
  const ry = (clientY - containerRect.top - oy) / ih
  return {
    rx: Math.max(0, Math.min(1, rx)),
    ry: Math.max(0, Math.min(1, ry)),
    inImage: rx >= 0 && rx <= 1 && ry >= 0 && ry <= 1,
  }
}

export function MonitorView({
  selWin,
  winState,
  capMethod,
  snapMethod,
  streamMethod,
  previewing,
  snapshotLatency,
  onTakeSnapshot,
  onTogglePreview,
  children,
  inputMethod,
  targetDims,
}: {
  selWin: WindowInfo
  winState: string
  capMethod: string
  snapMethod: string
  streamMethod: string
  previewing: boolean
  snapshotLatency: number | null
  onTakeSnapshot: () => void
  onTogglePreview: () => void
  children: React.ReactNode
  inputMethod: string
  targetDims: { w: number; h: number } | null
}) {
  const isDesktop = selWin.hwnd === 0
  const stateLabel = STATE_LABEL[winState] || winState
  const stateColor = STATE_COLOR[winState] || 'text-text-muted'

  // ── Interaction state ──
  const [focused, setFocused] = useState(false)
  const [mouseOn, setMouseOn] = useState(false)
  const [lastClick, setLastClick] = useState<{ x: number; y: number } | null>(null)
  const [dragging, setDragging] = useState(false)
  const dragPathRef = useRef<{ x: number; y: number }[]>([])
  const dragButtonRef = useRef<string>('left')
  const dragStartRef = useRef<{ rx: number; ry: number } | null>(null)
  const dragCurrentRef = useRef<{ rx: number; ry: number } | null>(null)
  const lastSampleRef = useRef<number>(0)
  const pressedKeysRef = useRef<PressedKey[]>([])
  const containerRef = useRef<HTMLDivElement>(null)

  // Visual feedback
  const [ripples, setRipples] = useState<Ripple[]>([])
  const [keyToast, setKeyToast] = useState<KeyToast | null>(null)
  const idCounterRef = useRef(0)
  const nextId = () => { idCounterRef.current += 1; return idCounterRef.current }
  const lastClickTimeRef = useRef(0)
  const pendingClickRef = useRef<ReturnType<typeof setTimeout> | null>(null)

  // Cleanup ripples
  useEffect(() => {
    if (ripples.length === 0) return
    const timer = setTimeout(() => {
      setRipples((prev) => prev.filter((r) => Date.now() - r.id < 400))
    }, 450)
    return () => clearTimeout(timer)
  }, [ripples])

  // Cleanup key toast
  useEffect(() => {
    if (!keyToast) return
    const timer = setTimeout(() => setKeyToast(null), 1000)
    return () => clearTimeout(timer)
  }, [keyToast])

  // Cleanup pending deferred click on unmount
  useEffect(() => {
    return () => {
      if (pendingClickRef.current) {
        clearTimeout(pendingClickRef.current)
        pendingClickRef.current = null
      }
    }
  }, [])

  // ── Auto-release keys on blur ──
  const releaseAllKeys = useCallback(() => {
    const keys = pressedKeysRef.current
    if (keys.length === 0) return
    for (const k of keys) {
      hostCall('send_input', {
        hwnd: selWin.hwnd, type: 'keyup',
        key: k.key, code: k.code, vk: k.keyCode, method: inputMethod,
      }).catch(() => {})
    }
    addLog(`[Input] auto-released ${keys.length} key(s) on blur`)
    keys.length = 0
  }, [selWin.hwnd, inputMethod])

  const handleBlur = useCallback(() => {
    setFocused(false)
    releaseAllKeys()
    if (dragging) {
      // Cancel drag — send mouseup so target window doesn't stay stuck
      setDragging(false)
      const path = dragPathRef.current
      const button = dragButtonRef.current
      if (path.length > 0) {
        hostCall('send_input', {
          hwnd: selWin.hwnd, type: 'click',
          x_norm: path[path.length - 1].x, y_norm: path[path.length - 1].y,
          button, method: inputMethod,
        }).catch(() => {})
        addLog(`[Input] drag cancelled on blur — auto-released mouse button at last position`)
      }
      dragPathRef.current = []
      dragStartRef.current = null
      dragCurrentRef.current = null
    }
  }, [releaseAllKeys, dragging, selWin.hwnd, inputMethod])

  // ── Mouse handlers ──
  const handleMouseDown = useCallback(
    (e: React.MouseEvent<HTMLDivElement>) => {
      if (isDesktop || !previewing) return
      const dims = targetDims
      if (!dims || dims.w <= 0 || dims.h <= 0) return
      const rect = e.currentTarget.getBoundingClientRect()
      const { rx, ry, inImage } = getImageCoords(e.clientX, e.clientY, rect, dims.w, dims.h)
      if (!inImage) return

      // Prevent browser context menu on right-click
      if (e.button === 2) e.preventDefault()

      // Focus canvas for keyboard input
      e.currentTarget.focus()

      const button = e.button === 2 ? 'right' : e.button === 1 ? 'middle' : 'left'
      dragButtonRef.current = button
      dragPathRef.current = [{ x: rx, y: ry }]
      dragStartRef.current = { rx, ry }
      dragCurrentRef.current = { rx, ry }
      lastSampleRef.current = Date.now()
      setDragging(true)
    },
    [isDesktop, previewing, targetDims],
  )

  const handleMouseMove = useCallback(
    (e: React.MouseEvent<HTMLDivElement>) => {
      if (!dragging || !previewing) return
      const dims = targetDims
      if (!dims || dims.w <= 0 || dims.h <= 0) return
      const now = Date.now()
      if (now - lastSampleRef.current < 50) return // 50ms throttle

      const rect = e.currentTarget.getBoundingClientRect()
      const { rx, ry, inImage } = getImageCoords(e.clientX, e.clientY, rect, dims.w, dims.h)
      if (!inImage) return // skip points outside actual image area
      dragPathRef.current.push({ x: rx, y: ry })
      dragCurrentRef.current = { rx, ry }
      lastSampleRef.current = now
    },
    [dragging, previewing, targetDims],
  )

  const handleMouseUp = useCallback(
    (e: React.MouseEvent<HTMLDivElement>) => {
      if (!dragging) return
      setDragging(false)

      const dims = targetDims
      const rect = e.currentTarget.getBoundingClientRect()
      const coords = dims && dims.w > 0
        ? getImageCoords(e.clientX, e.clientY, rect, dims.w, dims.h)
        : null
      const rx = coords?.rx ?? dragStartRef.current?.rx ?? 0
      const ry = coords?.ry ?? dragStartRef.current?.ry ?? 0

      const path = dragPathRef.current
      const button = dragButtonRef.current

      // Determine click vs drag: if only start point + end point (no moves), it's a click
      const movedPoints = path.length > 1

      if (!movedPoints) {
        // Pure click — defer send briefly to suppress if dblclick follows
        setLastClick({ x: Math.round(rx * 100), y: Math.round(ry * 100) })
        const clickTime = Date.now()
        lastClickTimeRef.current = clickTime
        const rippleId = nextId()
        setRipples((prev) => [...prev, { id: rippleId, x: rx * 100, y: ry * 100 }])

        const sendClick = () => {
          hostCall('send_input', {
            hwnd: selWin.hwnd, type: 'click', x_norm: rx, y_norm: ry,
            button, method: inputMethod,
          })
            .then(() => {
              addLog(
                `[Mouse] click → hwnd=0x${selWin.hwnd.toString(16)} (${Math.round(rx * 100)}%,${Math.round(ry * 100)}%) [${inputMethod}]`,
              )
            })
            .catch((err: any) => {
              addLog(`[Mouse] click failed: ${err?.message || err}`)
            })
        }

        // Wait to see if a double-click follows; if so, skip this click
        if (pendingClickRef.current) clearTimeout(pendingClickRef.current)
        pendingClickRef.current = setTimeout(() => {
          pendingClickRef.current = null
          if (lastClickTimeRef.current !== clickTime) return // dblclick happened
          sendClick()
        }, 300) // typical Windows double-click time is 500ms; 300ms is safe
      } else {
        // Drag: add final position
        path.push({ x: rx, y: ry })
        hostCall('send_input', {
          hwnd: selWin.hwnd, type: 'drag', button,
          path, method: inputMethod,
        })
          .then(() => {
            addLog(
              `[Mouse] drag ${path.length} pts → hwnd=0x${selWin.hwnd.toString(16)} [${inputMethod}]`,
            )
          })
          .catch((err: any) => {
            addLog(`[Mouse] drag failed: ${err?.message || err}`)
          })
      }

      dragPathRef.current = []
      dragStartRef.current = null
      dragCurrentRef.current = null
    },
    [dragging, selWin.hwnd, inputMethod, targetDims],
  )

  const handleDoubleClick = useCallback(
    (e: React.MouseEvent<HTMLDivElement>) => {
      if (isDesktop || !previewing) return
      const dims = targetDims
      if (!dims || dims.w <= 0 || dims.h <= 0) return
      const rect = e.currentTarget.getBoundingClientRect()
      const { rx, ry, inImage } = getImageCoords(e.clientX, e.clientY, rect, dims.w, dims.h)
      if (!inImage) return

      const button = e.button === 2 ? 'right' : e.button === 1 ? 'middle' : 'left'
      // Suppress both deferred single-click sends from handleMouseUp
      lastClickTimeRef.current = 0
      if (pendingClickRef.current) { clearTimeout(pendingClickRef.current); pendingClickRef.current = null }
      const rippleId = nextId()
      setRipples((prev) => [...prev, { id: rippleId, x: rx * 100, y: ry * 100 }, { id: rippleId + 1, x: rx * 100, y: ry * 100 }])

      hostCall('send_input', {
        hwnd: selWin.hwnd, type: 'dblclick', x_norm: rx, y_norm: ry,
        button, method: inputMethod,
      })
        .then(() => {
          addLog(
            `[Mouse] dblclick → hwnd=0x${selWin.hwnd.toString(16)} (${Math.round(rx * 100)}%,${Math.round(ry * 100)}%) [${inputMethod}]`,
          )
        })
        .catch((err: any) => {
          addLog(`[Mouse] dblclick failed: ${err?.message || err}`)
        })
    },
    [isDesktop, previewing, selWin.hwnd, inputMethod, targetDims],
  )

  const handleWheel = useCallback(
    (e: React.WheelEvent<HTMLDivElement>) => {
      if (isDesktop || !previewing) return
      const dims = targetDims
      if (!dims || dims.w <= 0 || dims.h <= 0) return
      e.preventDefault()
      const rect = e.currentTarget.getBoundingClientRect()
      const { rx, ry, inImage } = getImageCoords(e.clientX, e.clientY, rect, dims.w, dims.h)
      if (!inImage) return
      // Normalize deltaY to WHEEL_DELTA units based on deltaMode
      let rawDelta = e.deltaY
      if (e.deltaMode === 1) rawDelta *= 120 // line mode → pixels
      else if (e.deltaMode === 2) rawDelta *= 1200 // page mode → ~10 lines
      const delta = Math.round(rawDelta)
      if (delta === 0) return

      hostCall('send_input', {
        hwnd: selWin.hwnd, type: 'wheel', x_norm: rx, y_norm: ry,
        delta, method: inputMethod,
      })
        .then(() => {
          addLog(
            `[Mouse] wheel ${delta > 0 ? 'down' : 'up'} (${delta}) [${inputMethod}]`,
          )
        })
        .catch((err: any) => {
          addLog(`[Mouse] wheel failed: ${err?.message || err}`)
        })
    },
    [isDesktop, previewing, selWin.hwnd, inputMethod, targetDims],
  )

  // ── Keyboard handlers ──
  const handleKeyDown = useCallback(
    (e: React.KeyboardEvent<HTMLDivElement>) => {
      if (isDesktop || !previewing || !focused) return
      e.preventDefault()

      const key = e.key
      const code = e.code
      const vk = e.keyCode

      // Filter IME composition keys (VK_PROCESSKEY = 229)
      if (vk === 229 || key === 'Process' || key === 'Dead') return
      const isModifier = key === 'Control' || key === 'Shift' || key === 'Alt' || key === 'Meta'

      // Track for auto-release
      if (!pressedKeysRef.current.find((k) => k.key === key)) {
        pressedKeysRef.current.push({ key, code, keyCode: vk })
      }

      // All keys use individual keydown/keyup — the system naturally
      // recognizes combos (Ctrl+C) because Ctrl is already held down from
      // a previous keydown. No separate "combo" type needed for user input.
      hostCall('send_input', {
        hwnd: selWin.hwnd, type: 'keydown', key, code, vk, method: inputMethod,
      }).catch((err: any) => addLog(`[Key] keydown failed: ${err?.message || err}`))

      // Visual feedback — accumulate modifiers prefix
      const toastId = nextId()
      const parts: string[] = []
      if (e.ctrlKey) parts.push('Ctrl')
      if (e.altKey) parts.push('Alt')
      if (e.shiftKey && !isModifier) parts.push('Shift')
      if (e.metaKey) parts.push('Win')
      parts.push(key)
      const label = parts.join('+')
      setKeyToast({ text: label, id: toastId })
    },
    [isDesktop, previewing, focused, selWin.hwnd, inputMethod],
  )

  const handleKeyUp = useCallback(
    (e: React.KeyboardEvent<HTMLDivElement>) => {
      if (isDesktop || !previewing || !focused) return
      e.preventDefault()

      // Remove from tracked keys
      pressedKeysRef.current = pressedKeysRef.current.filter((k) => k.key !== e.key)

      hostCall('send_input', {
        hwnd: selWin.hwnd, type: 'keyup',
        key: e.key, code: e.code, vk: e.keyCode, method: inputMethod,
      }).catch((err: any) => addLog(`[Key] keyup failed: ${err?.message || err}`))
    },
    [isDesktop, previewing, focused, selWin.hwnd, inputMethod],
  )

  // ── Visual feedback elements ──
  const dragOverlay = dragging && dragStartRef.current && dragCurrentRef.current && (() => {
    const s = dragStartRef.current!
    const c = dragCurrentRef.current!
    const left = Math.min(s.rx, c.rx) * 100
    const top = Math.min(s.ry, c.ry) * 100
    const width = Math.abs(c.rx - s.rx) * 100
    const height = Math.abs(c.ry - s.ry) * 100
    return (
      <div
        className="absolute inset-0 pointer-events-none"
        style={{ left: `${left}%`, top: `${top}%`, width: `${width}%`, height: `${height}%` }}
      >
        <div className="w-full h-full border-2 border-accent/60 bg-accent/10 rounded-sm" />
      </div>
    )
  })()

  return (
    <div className="flex-1 flex flex-col min-h-0">
      {/* Toolbar */}
      <div className="flex items-center h-11 px-4 bg-bg-secondary border-b border-border shrink-0 gap-4">
        {/* Target */}
        <div className="flex items-center gap-2 min-w-0">
          <span className="text-sm font-medium text-text-primary truncate max-w-[240px]">
            {selWin.title}
          </span>
          <span className={`text-[11px] font-medium shrink-0 ${stateColor}`}>
            {stateLabel}
          </span>
        </div>
        <span className="text-border/40 select-none">│</span>
        {/* Methods */}
        <div className="flex items-center gap-2 text-[11px] shrink-0">
          <span className="inline-flex items-center gap-1 text-text-muted">
            <Camera className="w-3 h-3" /> Snapshot
          </span>
          <span className="font-medium text-accent bg-accent/10 px-1.5 py-0.5 rounded">
            {METHOD_SHORT[snapMethod] || snapMethod.toUpperCase()}
          </span>
        </div>
        <div className="flex items-center gap-2 text-[11px] shrink-0">
          <span className="inline-flex items-center gap-1 text-text-muted">
            <Play className="w-3 h-3" /> Stream
          </span>
          <span className="font-medium text-accent bg-accent/10 px-1.5 py-0.5 rounded">
            {METHOD_SHORT[streamMethod] || streamMethod.toUpperCase()}
          </span>
        </div>
        {capMethod && (
          <span className="text-[11px] text-text-muted shrink-0">
            Last: {METHOD_SHORT[capMethod] || capMethod}
            {snapshotLatency !== null ? ` (${snapshotLatency}ms)` : ''}
          </span>
        )}
        <span className="flex-1" />
        {/* Actions */}
        <Tooltip text="单帧截图">
          <button
            onClick={onTakeSnapshot}
            className="inline-flex items-center gap-1.5 rounded-md px-2.5 h-7 text-xs font-medium bg-accent text-white hover:bg-accent-hover transition-colors"
          >
            <Camera className="w-3.5 h-3.5" />
            Snapshot
          </button>
        </Tooltip>
        {previewing ? (
          <Tooltip text="停止实时预览">
            <button
              onClick={onTogglePreview}
              className="inline-flex items-center gap-1.5 rounded-md px-2.5 h-7 text-xs font-medium bg-error/20 text-error hover:bg-error/30 transition-colors"
            >
              <Square className="w-3.5 h-3.5" />
              Stop
            </button>
          </Tooltip>
        ) : (
          <Tooltip text="开始实时预览">
            <button
              onClick={onTogglePreview}
              className="inline-flex items-center gap-1.5 rounded-md px-2.5 h-7 text-xs font-medium border border-border text-text-secondary hover:bg-bg-hover transition-colors"
            >
              <Play className="w-3.5 h-3.5" />
              Preview
            </button>
          </Tooltip>
        )}
      </div>

      {/* Preview canvas area */}
      <div className="flex-1 overflow-hidden p-4">
        <div
          ref={containerRef}
          tabIndex={isDesktop || !previewing ? undefined : 0}
          className={`w-full h-full rounded-xl bg-bg-secondary ring-1 ring-inset overflow-hidden flex items-center justify-center relative outline-none transition-shadow ${
            !isDesktop && previewing
              ? focused
                ? 'ring-accent shadow-[0_0_0_2px_rgba(38,79,120,0.3)] cursor-crosshair'
                : 'ring-border cursor-crosshair hover:ring-text-muted'
              : 'ring-border'
          }`}
          onDoubleClick={handleDoubleClick}
          onMouseDown={handleMouseDown}
          onMouseMove={handleMouseMove}
          onMouseUp={handleMouseUp}
          onWheel={handleWheel}
          onMouseEnter={() => setMouseOn(true)}
          onMouseLeave={() => {
            setMouseOn(false)
            // Cancel drag on mouse leave — release button in target
            if (dragging) {
              setDragging(false)
              const path = dragPathRef.current
              const button = dragButtonRef.current
              if (path.length > 0) {
                hostCall('send_input', {
                  hwnd: selWin.hwnd, type: 'click',
                  x_norm: path[path.length - 1].x, y_norm: path[path.length - 1].y,
                  button, method: inputMethod,
                }).catch(() => {})
                addLog(`[Input] drag cancelled on mouse leave — auto-released mouse button`)
              }
              dragPathRef.current = []
              dragStartRef.current = null
              dragCurrentRef.current = null
            }
          }}
          onKeyDown={handleKeyDown}
          onKeyUp={handleKeyUp}
          onFocus={() => setFocused(true)}
          onBlur={handleBlur}
        >
          {children}

          {/* Drag selection overlay */}
          {dragOverlay}

          {/* Click ripples */}
          {ripples.map((r) => (
            <div
              key={r.id}
              className="absolute w-5 h-5 -ml-2.5 -mt-2.5 rounded-full border-2 border-accent/60 bg-accent/20 pointer-events-none animate-ping-once"
              style={{ left: `${r.x}%`, top: `${r.y}%` }}
            />
          ))}

          {/* Key toast */}
          {keyToast && (
            <div className="absolute bottom-12 left-1/2 -translate-x-1/2 px-3 py-1.5 rounded-lg bg-bg-tertiary/95 text-xs text-text-primary font-mono shadow-lg backdrop-blur-sm pointer-events-none z-10">
              [{keyToast.text}]
            </div>
          )}

          {/* Mouse forwarding hint */}
          {mouseOn && !isDesktop && previewing && (
            <div className="absolute bottom-3 left-1/2 -translate-x-1/2 px-3 py-1.5 rounded-lg bg-bg-tertiary/90 text-xs text-text-secondary flex items-center gap-1.5 shadow-lg backdrop-blur-sm pointer-events-none z-10">
              <MousePointer2 className="w-3.5 h-3.5 text-accent" />
              {focused
                ? '键盘+鼠标转发中 | Esc 或点击外部释放焦点'
                : `点击捕捉焦点 → ${inputMethod}`}
              {lastClick && !focused && (
                <span className="text-text-muted ml-2">
                  [{lastClick.x}%, {lastClick.y}%]
                </span>
              )}
            </div>
          )}

          {/* Empty state */}
          {!previewing && lastClick === null && (
            <div className="absolute inset-0 flex items-center justify-center pointer-events-none">
              <div className="text-center space-y-1">
                <div className="text-sm text-text-muted">No preview active</div>
                <div className="text-xs text-text-tertiary">
                  点击右上角 Preview 开始实时预览
                </div>
              </div>
            </div>
          )}
        </div>
      </div>
    </div>
  )
}
