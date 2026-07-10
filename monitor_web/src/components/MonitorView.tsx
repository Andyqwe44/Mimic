// ═══ Monitor View — live preview + remote-control input forwarding ═══
// Renders ScreenshotPanel in bare mode, adds toolbar + canvas overlay for
// mouse/keyboard input mapping. Supports click/dblclick/drag/wheel/keyboard.
import { useState, useRef, useCallback, useEffect } from 'react'
import { Camera, Play, Square, MousePointer2, Power } from 'lucide-react'
import { ActionBtn, Tooltip } from './Toolkit'
import { STATE_LABEL, codeToName, MOUSE_METHOD, KEY_METHOD } from '../lib/constants'
import { addLog, hostCall } from '../lib/bridge'
import type { WindowInfo, Rect } from '../lib/types'

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
): { rx: number; ry: number; inImage: boolean; px: number; py: number } {
  const cw = containerRect.width
  const ch = containerRect.height
  if (cw <= 0 || ch <= 0 || imageW <= 0 || imageH <= 0) {
    return { rx: 0, ry: 0, inImage: false, px: 0, py: 0 }
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
  const clamped = {
    rx: Math.max(0, Math.min(1, rx)),
    ry: Math.max(0, Math.min(1, ry)),
  }
  return {
    rx: clamped.rx,
    ry: clamped.ry,
    inImage: rx >= 0 && rx <= 1 && ry >= 0 && ry <= 1,
    // Pixel position within container (accounts for letterbox offset)
    px: ox + clamped.rx * iw,
    py: oy + clamped.ry * ih,
  }
}

// ── Sequence-based combo matching ──
// Compares ordered pressed-key sequence (display names) against stored hotkey
// "Ctrl+K" ≠ "K+Ctrl" — press order matters
function seqMatches(pressedCodes: string[], hotkey: string): boolean {
  return pressedCodes.map(codeToName).join('+') === hotkey
}

export function MonitorView({
  selWin,
  winState,
  capMethod: _capMethod,
  snapMethod: _snapMethod,
  streamMethod: _streamMethod,
  previewing,
  snapshotLatency: _snapshotLatency,
  onTakeSnapshot,
  onTogglePreview,
  children,
  inputMethod: _inputMethod,
  mouseMode,
  keyMode,
  mappingEnabled,
  setMappingEnabled,
  mappingHotkey,
  targetDims,
  selfRect,
  screenRect,
  selfTargetMode,
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
  inputMethod?: string
  mouseMode?: 'seize' | 'semi' | 'background'
  keyMode?: 'seize' | 'postmsg' | 'sendmsg'
  mappingEnabled: boolean
  setMappingEnabled: (v: boolean) => void
  mappingHotkey: string
  targetDims: { w: number; h: number } | null
  selfRect?: Rect | null
  screenRect?: Rect | null
  selfTargetMode?: 'warn' | 'exclude'
}) {
  const isDesktop = selWin.hwnd === 0
  const stateLabel = STATE_LABEL[winState] || winState

  // ── Derived input methods from mode ──
  const mM = MOUSE_METHOD[mouseMode ?? 'background']   // click/drag/wheel method
  const kM = KEY_METHOD[keyMode ?? 'postmsg']            // keyboard method
  const sendMove = (mouseMode ?? 'background') === 'seize'   // only seize sends move

  // ═══ Interaction state ═══
  const [focused, setFocused] = useState(false)    // canvas has keyboard focus
  const [mouseOn, setMouseOn] = useState(false)
  const [dragging, setDragging] = useState(false)
  const dragPathRef = useRef<{ x: number; y: number }[]>([])
  const dragButtonRef = useRef<string>('left')     // button held during drag
  const dragStartRef = useRef<{ rx: number; ry: number } | null>(null)
  const dragCurrentRef = useRef<{ rx: number; ry: number } | null>(null)
  const lastSampleRef = useRef<number>(0)           // throttle drag sampling at 50ms
  const pressedKeysRef = useRef<PressedKey[]>([])   // currently held keys (for auto-release on blur)
  const containerRef = useRef<HTMLDivElement>(null)

  // ── Visual feedback refs ──
  const [ripples, setRipples] = useState<Ripple[]>([])
  const [keyToast, setKeyToast] = useState<KeyToast | null>(null)
  const idCounterRef = useRef(0)
  const nextId = () => { idCounterRef.current += 1; return idCounterRef.current }
  const dblclickSuppressRef = useRef(false)   // skip mouseup click when dblclick already sent
  const lastMoveSendRef = useRef(0)          // throttle mouse-move forwarding at 60fps
  const lastCursorRef = useRef(0)            // throttle cursor overlay update at 30fps

  // ── Cursor overlay state (follows mouse on canvas) ──
  const [cursorPos, setCursorPos] = useState<{ rx: number; ry: number; px: number; py: number } | null>(null)

  // ── Self-target detection: is the mapped screen position inside GAM window? ──
  // Only meaningful for desktop capture (hwnd=0) where GAM is visible in the frame.
  // For window capture, input goes to the target window only — no self-target risk.
  const isSelfTarget = (() => {
    if (!cursorPos || !selfRect || !selfRect.w || !selfRect.h) return false
    // Map normalized coords to absolute screen position using capture area rect.
    // Desktop: screenRect = virtual screen. Window: screenRect = target window rect.
    const area = screenRect && screenRect.w > 0 ? screenRect : null
    if (!area) return false
    const absX = area.x + cursorPos.rx * area.w
    const absY = area.y + cursorPos.ry * area.h
    return (
      absX >= selfRect.x &&
      absX <= selfRect.x + selfRect.w &&
      absY >= selfRect.y &&
      absY <= selfRect.y + selfRect.h
    )
  })()

  // ── Clear cursor overlay when mapping or preview toggles off ──
  useEffect(() => {
    if (!mappingEnabled || !previewing) {
      setCursorPos(null)
      hostCall('cursor_overlay', { show: 0 }).catch(() => {})
    }
  }, [mappingEnabled, previewing])

  // ── Cleanup expired ripples ──
  useEffect(() => {
    if (ripples.length === 0) return
    const timer = setTimeout(() => {
      setRipples((prev) => prev.filter((r) => Date.now() - r.id < 400))
    }, 450)
    return () => clearTimeout(timer)
  }, [ripples])

  // ── Cleanup expired key toast ──
  useEffect(() => {
    if (!keyToast) return
    const timer = setTimeout(() => setKeyToast(null), 1000)
    return () => clearTimeout(timer)
  }, [keyToast])

  // ── Global hotkey listener for mapping toggle (sequence-based) ──
  const pressedHotkeyRef = useRef<string[]>([])
  useEffect(() => {
    const onDown = (e: KeyboardEvent) => {
      if (e.target instanceof HTMLInputElement || e.target instanceof HTMLTextAreaElement) return
      if (e.repeat) return
      if (!pressedHotkeyRef.current.includes(e.code)) {
        pressedHotkeyRef.current.push(e.code)
      }
      if (seqMatches(pressedHotkeyRef.current, mappingHotkey)) {
        e.preventDefault()
        e.stopPropagation()
        setMappingEnabled((prev: boolean) => {
          const next = !prev
          addLog(`[Input] mapping ${next ? 'ON' : 'OFF'} (${mappingHotkey})`)
          return next
        })
      }
    }
    const onUp = (e: KeyboardEvent) => {
      const idx = pressedHotkeyRef.current.indexOf(e.code)
      if (idx >= 0) pressedHotkeyRef.current.splice(idx, 1)
    }
    window.addEventListener('keydown', onDown, true)
    window.addEventListener('keyup', onUp, true)
    return () => {
      window.removeEventListener('keydown', onDown, true)
      window.removeEventListener('keyup', onUp, true)
    }
  }, [mappingHotkey, setMappingEnabled])

  // No cleanup needed — immediate clicks have no pending state

  // ── Auto-release all held keys on blur (prevents stuck keys in target) ──
  const releaseAllKeys = useCallback(() => {
    const keys = pressedKeysRef.current
    if (keys.length === 0) return
    for (const k of keys) {
      hostCall('send_input', {
        hwnd: selWin.hwnd, type: 'keyup',
        key: k.key, code: k.code, vk: k.keyCode, method: kM,
      }).catch(() => {})
    }
    addLog(`[Input] auto-released ${keys.length} key(s) on blur`)
    keys.length = 0
  }, [selWin.hwnd, mM, kM])

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
          button, method: mM,
        }).catch(() => {})
        addLog(`[Input] drag cancelled on blur — auto-released mouse button at last position`)
      }
      dragPathRef.current = []
      dragStartRef.current = null
      dragCurrentRef.current = null
    }
  }, [releaseAllKeys, dragging, selWin.hwnd, mM, kM])

  // ── Mouse down → start drag (or click if released without moving) ──
  const handleMouseDown = useCallback(
    (e: React.MouseEvent<HTMLDivElement>) => {
      if (isDesktop || !previewing || !mappingEnabled) return
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
    [isDesktop, previewing, mappingEnabled, targetDims],
  )

  // ── Mouse move → drag sampling OR cursor forwarding ──
  const handleMouseMove = useCallback(
    (e: React.MouseEvent<HTMLDivElement>) => {
      if (isDesktop || !previewing || !mappingEnabled) return
      const dims = targetDims
      if (!dims || dims.w <= 0 || dims.h <= 0) return
      const now = Date.now()

      if (dragging) {
        // Drag path sampling at 50ms
        if (now - lastSampleRef.current < 50) return
        const rect = e.currentTarget.getBoundingClientRect()
        const coords = getImageCoords(e.clientX, e.clientY, rect, dims.w, dims.h)
        if (!coords.inImage) return
        dragPathRef.current.push({ x: coords.rx, y: coords.ry })
        dragCurrentRef.current = { rx: coords.rx, ry: coords.ry }
        lastSampleRef.current = now
        // Update cursor overlay during drag too (canvas dot + real-window circle)
        if (now - lastCursorRef.current > 33) {
          lastCursorRef.current = now
          setCursorPos({ rx: coords.rx, ry: coords.ry, px: coords.px, py: coords.py })
          hostCall('cursor_overlay', {
            hwnd: selWin.hwnd, x_norm: coords.rx, y_norm: coords.ry, show: 1,
          }).catch(() => {})
        }
      } else {
        // Update cursor overlay at ~30fps (canvas dot + real-window circle via C++)
        if (now - lastCursorRef.current > 33) {
          lastCursorRef.current = now
          const rect = e.currentTarget.getBoundingClientRect()
          const coords = getImageCoords(e.clientX, e.clientY, rect, dims.w, dims.h)
          if (coords.inImage) {
            setCursorPos({ rx: coords.rx, ry: coords.ry, px: coords.px, py: coords.py })
            // Send to C++ → real-screen circle overlay at absolute screen position
            hostCall('cursor_overlay', {
              hwnd: selWin.hwnd, x_norm: coords.rx, y_norm: coords.ry, show: 1,
            }).catch(() => {})
          } else {
            setCursorPos(null)
            hostCall('cursor_overlay', { show: 0 }).catch(() => {})
          }
        }
        // Mouse move forwarding: ONLY in seize mode (grabs system cursor).
        // Semi + Background: virtual indicator only, no cursor movement.
        if (!sendMove) return
        // Continuous cursor forwarding at 60fps
        if (now - lastMoveSendRef.current < 16) return
        lastMoveSendRef.current = now
        const rect = e.currentTarget.getBoundingClientRect()
        const { rx, ry, inImage } = getImageCoords(e.clientX, e.clientY, rect, dims.w, dims.h)
        if (!inImage) return
        hostCall('send_input', {
          hwnd: selWin.hwnd, type: 'move', x_norm: rx, y_norm: ry, method: mM,
        }).catch(() => {})
      }
    },
    [dragging, previewing, isDesktop, mappingEnabled, targetDims, selWin.hwnd, mM, kM],
  )

  // ── Mouse up → send click or drag (immediate, no defer) ──
  const handleMouseUp = useCallback(
    (e: React.MouseEvent<HTMLDivElement>) => {
      if (!dragging) return
      setDragging(false)

      // Suppress if dblclick already handled this pair
      if (dblclickSuppressRef.current) {
        dblclickSuppressRef.current = false
        dragPathRef.current = []
        dragStartRef.current = null
        dragCurrentRef.current = null
        return
      }

      const dims = targetDims
      const rect = e.currentTarget.getBoundingClientRect()
      const coords = dims && dims.w > 0
        ? getImageCoords(e.clientX, e.clientY, rect, dims.w, dims.h)
        : null
      const rx = coords?.rx ?? dragStartRef.current?.rx ?? 0
      const ry = coords?.ry ?? dragStartRef.current?.ry ?? 0

      const path = dragPathRef.current
      const button = dragButtonRef.current

      // Determine click vs drag
      const movedPoints = path.length > 1

      if (!movedPoints) {
        // Immediate click — no defer for remote-control feel
        const rippleId = nextId()
        setRipples((prev) => [...prev, { id: rippleId, x: rx * 100, y: ry * 100 }])

        hostCall('send_input', {
          hwnd: selWin.hwnd, type: 'click', x_norm: rx, y_norm: ry,
          button, method: mM,
        })
          .then(() => {
            addLog(
              `[Mouse] click → hwnd=0x${selWin.hwnd.toString(16)} (${Math.round(rx * 100)}%,${Math.round(ry * 100)}%) [${inputMethod}]`,
            )
          })
          .catch((err: any) => {
            addLog(`[Mouse] click failed: ${err?.message || err}`)
          })
      } else {
        // Drag: add final position
        path.push({ x: rx, y: ry })
        hostCall('send_input', {
          hwnd: selWin.hwnd, type: 'drag', button,
          path, method: mM,
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
    [dragging, selWin.hwnd, mM, kM, targetDims],
  )

  // ── Double click — suppresses second mouseup click, sends dblclick immediately ──
  const handleDoubleClick = useCallback(
    (e: React.MouseEvent<HTMLDivElement>) => {
      if (isDesktop || !previewing || !mappingEnabled) return
      const dims = targetDims
      if (!dims || dims.w <= 0 || dims.h <= 0) return
      const rect = e.currentTarget.getBoundingClientRect()
      const { rx, ry, inImage } = getImageCoords(e.clientX, e.clientY, rect, dims.w, dims.h)
      if (!inImage) return

      const button = e.button === 2 ? 'right' : e.button === 1 ? 'middle' : 'left'
      // Suppress the second mouseup click — dblclick handles the full pair
      dblclickSuppressRef.current = true
      const rippleId = nextId()
      setRipples((prev) => [...prev, { id: rippleId, x: rx * 100, y: ry * 100 }, { id: rippleId + 1, x: rx * 100, y: ry * 100 }])

      hostCall('send_input', {
        hwnd: selWin.hwnd, type: 'dblclick', x_norm: rx, y_norm: ry,
        button, method: mM,
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
    [isDesktop, previewing, mappingEnabled, selWin.hwnd, mM, kM, targetDims],
  )

  // ── Mouse wheel → normalized delta (deltaMode-aware) ──
  const handleWheel = useCallback(
    (e: React.WheelEvent<HTMLDivElement>) => {
      if (isDesktop || !previewing || !mappingEnabled) return
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
        delta, method: mM,
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
    [isDesktop, previewing, mappingEnabled, selWin.hwnd, mM, kM, targetDims],
  )

  // ── Keyboard handlers — all keys use individual keydown/keyup ──
  // System naturally recognizes combos (Ctrl+C) because Ctrl is already held
  // from a previous keydown. No separate "combo" type needed.
  const handleKeyDown = useCallback(
    (e: React.KeyboardEvent<HTMLDivElement>) => {
      if (isDesktop || !previewing || !focused || !mappingEnabled) return
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
        hwnd: selWin.hwnd, type: 'keydown', key, code, vk, method: kM,
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
    [isDesktop, previewing, focused, mappingEnabled, selWin.hwnd, mM, kM],
  )

  const handleKeyUp = useCallback(
    (e: React.KeyboardEvent<HTMLDivElement>) => {
      if (isDesktop || !previewing || !focused || !mappingEnabled) return
      e.preventDefault()

      // Remove from tracked keys
      pressedKeysRef.current = pressedKeysRef.current.filter((k) => k.key !== e.key)

      hostCall('send_input', {
        hwnd: selWin.hwnd, type: 'keyup',
        key: e.key, code: e.code, vk: e.keyCode, method: kM,
      }).catch((err: any) => addLog(`[Key] keyup failed: ${err?.message || err}`))
    },
    [isDesktop, previewing, focused, mappingEnabled, selWin.hwnd, mM, kM],
  )

  // ── Drag selection overlay (lazy-evaluated to avoid stale closure) ──
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
      <div className="flex items-center h-11 px-4 bg-bg-secondary border-b border-border shrink-0 gap-3">
        {/* Left: target + state (Connection style) */}
        <div className="flex items-center gap-2 min-w-0">
          <span className="text-sm font-medium text-text-primary truncate w-[144px]">
            {selWin.title}
          </span>
          <span className="text-[11px] font-medium text-accent bg-accent/10 px-1.5 py-0.5 rounded shrink-0">
            {stateLabel}
          </span>
        </div>
        {/* Middle: spacer */}
        <span className="flex-1" />
        {/* Input mapping toggle */}
        <Tooltip text={`输入映射 (${mappingHotkey})`}>
          <button
            onClick={() => {
              setMappingEnabled(!mappingEnabled)
              addLog(`[Input] mapping ${!mappingEnabled ? 'ON' : 'OFF'}`)
            }}
            className={`inline-flex items-center gap-1.5 h-7 px-2 rounded-md text-xs font-medium transition-all duration-150 ${
              mappingEnabled
                ? 'bg-accent/10 text-accent border border-accent/30'
                : 'border border-border text-text-secondary hover:bg-bg-hover'
            }`}
          >
            <Power className={`w-3 h-3 ${mappingEnabled ? 'text-accent' : 'text-text-muted'}`} />
            <span>映射</span>
          </button>
        </Tooltip>
        {/* Right: Snapshot → Preview/Stop (right-aligned) */}
        <ActionBtn
          icon={<Camera className="w-3.5 h-3.5" />}
          label="Snapshot"
          title="单帧截图"
          variant="primary"
          onClick={onTakeSnapshot}
        />
        {previewing ? (
          <ActionBtn
            icon={<Square className="w-3.5 h-3.5" />}
            label="Stop"
            title="停止实时预览"
            variant="danger"
            onClick={onTogglePreview}
          />
        ) : (
          <ActionBtn
            icon={<Play className="w-3.5 h-3.5" />}
            label="Preview"
            title="开始实时预览"
            variant="outline-accent"
            onClick={onTogglePreview}
          />
        )}
      </div>

      {/* ── Preview canvas area — contains ScreenshotPanel children + overlays ── */}
      <div className="flex-1 overflow-hidden p-4">
        <div
          ref={containerRef}
          tabIndex={isDesktop || !previewing || !mappingEnabled ? undefined : 0}
          className={`w-full h-full rounded-xl bg-bg-secondary ring-1 ring-inset overflow-hidden flex items-center justify-center relative outline-none transition-shadow ${
            !isDesktop && previewing && mappingEnabled
              ? focused
                ? 'ring-accent shadow-[0_0_0_2px_rgba(38,79,120,0.3)] cursor-crosshair'
                : 'ring-accent/40 cursor-crosshair hover:ring-accent/70'
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
            setCursorPos(null)
            hostCall('cursor_overlay', { show: 0 }).catch(() => {})
            // Cancel drag on mouse leave — release button in target
            if (dragging) {
              setDragging(false)
              const path = dragPathRef.current
              const button = dragButtonRef.current
              if (path.length > 0) {
                hostCall('send_input', {
                  hwnd: selWin.hwnd, type: 'click',
                  x_norm: path[path.length - 1].x, y_norm: path[path.length - 1].y,
                  button, method: mM,
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
          {previewing && children}

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

          {/* Remote-control hint */}
          {mouseOn && !isDesktop && previewing && mappingEnabled && (
            <div className="absolute bottom-3 left-1/2 -translate-x-1/2 px-3 py-1.5 rounded-lg bg-bg-tertiary/90 text-xs text-text-secondary flex items-center gap-1.5 shadow-lg backdrop-blur-sm pointer-events-none z-10">
              <MousePointer2 className={`w-3.5 h-3.5 ${focused ? 'text-accent animate-pulse' : 'text-text-muted'}`} />
              {focused
                ? `远程控制中 · 鼠标${mouseMode === 'seize' ? 'Seize' : mouseMode === 'semi' ? 'Semi' : 'Bg'} · 键盘${keyMode === 'seize' ? 'Seize' : keyMode === 'sendmsg' ? 'SendMsg' : 'PostMsg'} · Esc 释放`
                : `悬停移动光标 · 点击控制 · 鼠标${mouseMode === 'seize' ? 'Seize' : mouseMode === 'semi' ? 'Semi' : 'Bg'}`}
            </div>
          )}
          {mouseOn && isDesktop && previewing && mappingEnabled && (
            <div className="absolute bottom-3 left-1/2 -translate-x-1/2 px-3 py-1.5 rounded-lg bg-bg-tertiary/90 text-xs text-text-muted flex items-center gap-1.5 shadow-lg backdrop-blur-sm pointer-events-none z-10">
              <MousePointer2 className="w-3.5 h-3.5 text-text-muted" />
              桌面预览 · 输入映射已禁用（请选择窗口）
            </div>
          )}
          {mouseOn && isDesktop && previewing && mappingEnabled && isSelfTarget && selfTargetMode === 'warn' && (
            <div className="absolute top-12 left-1/2 -translate-x-1/2 px-3 py-1.5 rounded-lg bg-error/15 text-xs text-error flex items-center gap-1.5 shadow-lg backdrop-blur-sm pointer-events-none z-10">
              ⚠ 映射目标为 GAM 自身窗口 — 可在 Settings 中切换为「排除窗口」模式
            </div>
          )}
          {mouseOn && !isDesktop && previewing && !mappingEnabled && (
            <div className="absolute bottom-3 left-1/2 -translate-x-1/2 px-3 py-1.5 rounded-lg bg-bg-tertiary/90 text-xs text-text-muted flex items-center gap-1.5 shadow-lg backdrop-blur-sm pointer-events-none z-10">
              <Power className="w-3.5 h-3.5" />
              预览中 · 按 {mappingHotkey} 或点击「映射」开启控制
            </div>
          )}

          {/* Empty state */}
          {!previewing && (
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
