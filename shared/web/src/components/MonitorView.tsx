// ═══ Monitor View — live preview + remote-control input forwarding ═══
// Canvas keeps the real OS cursor only. Click ripples / drag rect / hover
// circle are drawn on the REAL capture target via C++ layered overlays.
//
// Preview mapping = test harness only (production = remote model → send_input).
// Same atomic path as agent: mousedown/up/move(+held)/keydown/up/text(Unicode).
// While mapping+previewing, host pins GAM TOPMOST (set_mapping_controller).
//
// IME dock: compose here; on commit flush whole string as background WM_CHAR
// (no SetFocus / no target activate — must not steal the user's foreground).
import { useState, useRef, useCallback, useEffect } from 'react'
import { useTranslation } from 'react-i18next'
import { Camera, Play, Square, Power, Bot, User, RefreshCw } from 'lucide-react'
import { ActionBtn, Tooltip } from './Toolkit'
import { PeerRemoteView } from './PeerRemoteView'
import { SessionPanicBar } from './SessionPanicBar'
import { STATE_LABEL, codeToName, resolveInputMethods } from '../lib/constants'
import { THIN_CLIENT } from '../lib/features'
import { addLog, hostCall, onNativePush } from '../lib/bridge'
import { RADIUS, RING, SHELL_PAD, TEXT, PEER_WORKSPACE } from '../lib/design'
import type { WindowInfo, Rect } from '../lib/types'

// ── Types ──
interface KeyToast { text: string; id: number }
interface PressedKey { key: string; code: string; keyCode: number }

// Imperative API exposed to the Self-Test orchestrator (App).
// Every send* drives the EXACT same hostCall('send_input') path a real user uses.
export interface MonitorApi {
  sendClick: (rx: number, ry: number, button?: string) => Promise<any>
  sendWheel: (rx: number, ry: number, delta: number) => Promise<any>
  sendDrag: (path: { x: number; y: number }[], button?: string) => Promise<any>
  sendText: (text: string) => Promise<any>
  sendKey: (type: 'keydown' | 'keyup', key: string, code: string, vk: number) => Promise<any>
  ready: () => boolean   // preview + mapping active and target dims known
}

// ── Coordinate helpers ──
// Letterboxed image layout inside the preview container (object-fit: contain).
function imageLayout(cw: number, ch: number, imageW: number, imageH: number) {
  if (cw <= 0 || ch <= 0 || imageW <= 0 || imageH <= 0) {
    return { iw: 0, ih: 0, ox: 0, oy: 0 }
  }
  const imgAspect = imageW / imageH
  const containerAspect = cw / ch
  if (containerAspect > imgAspect) {
    const ih = ch
    const iw = ch * imgAspect
    return { iw, ih, ox: (cw - iw) / 2, oy: 0 }
  }
  const iw = cw
  const ih = cw / imgAspect
  return { iw, ih, ox: 0, oy: (ch - ih) / 2 }
}

// Compute normalized (0-1) coords relative to actual image area, accounting for letterbox
function getImageCoords(
  clientX: number, clientY: number,
  containerRect: DOMRect,
  imageW: number, imageH: number,
): { rx: number; ry: number; inImage: boolean; px: number; py: number } {
  const cw = containerRect.width
  const ch = containerRect.height
  const { iw, ih, ox, oy } = imageLayout(cw, ch, imageW, imageH)
  if (iw <= 0 || ih <= 0) {
    return { rx: 0, ry: 0, inImage: false, px: 0, py: 0 }
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
  acceptControl = false,
  snapshotLatency: _snapshotLatency,
  onTakeSnapshot,
  onTogglePreview,
  onToggleAcceptControl,
  children,
  inputMethod: _inputMethod,
  mouseMode: _mouseMode,
  keyMode: _keyMode,
  mappingEnabled,
  setMappingEnabled,
  mappingHotkey,
  targetDims,
  selfRect,
  screenRect,
  selfTargetMode,
  apiRef,
  peerRole = 'idle',
  peerTransport = 'none',
  peerControlMode = 'human',
  setPeerControlMode,
  remotePeerWindows = [],
  setRemotePeerWindows: _setRemotePeerWindows,
  encodeHint,
}: {
  selWin: WindowInfo
  winState: string
  capMethod: string
  snapMethod: string
  streamMethod: string
  previewing: boolean
  acceptControl?: boolean
  snapshotLatency: number | null
  onTakeSnapshot: () => void
  onTogglePreview: () => void
  onToggleAcceptControl?: () => void
  children: React.ReactNode
  inputMethod?: string
  mouseMode?: 'seize' | 'semi' | 'background'
  keyMode?: 'seize' | 'postmsg' | 'sendmsg'
  mappingEnabled: boolean
  setMappingEnabled: React.Dispatch<React.SetStateAction<boolean>>
  mappingHotkey: string
  targetDims: { w: number; h: number } | null
  selfRect?: Rect | null
  screenRect?: Rect | null
  selfTargetMode?: 'warn' | 'exclude'
  apiRef?: React.MutableRefObject<MonitorApi | null>
  peerRole?: string
  peerTransport?: string
  peerControlMode?: 'human' | 'ai'
  setPeerControlMode?: (m: 'human' | 'ai') => void
  remotePeerWindows?: Array<{ title: string; hwnd: number; id?: string }>
  setRemotePeerWindows?: (w: Array<{ title: string; hwnd: number; id?: string }>) => void
  encodeHint?: string
}) {
  const { t } = useTranslation()
  const isDesktop = selWin.hwnd === 0
  const stateLabel = t(STATE_LABEL[winState] || winState)

  // Target-driven policy (Settings mouse/key modes ignored):
  // desktop → foreground SendInput; window → background SendMessage + [0,1] clip.
  const { mouseMethod: mM, keyMethod: kM, sendMove, policy: inputPolicy } =
    resolveInputMethods(isDesktop)

  // ═══ Interaction state ═══
  const [focused, setFocused] = useState(false)    // IME dock has keyboard focus
  const [composing, setComposing] = useState(false)
  const [mouseOn, setMouseOn] = useState(false)
  const [dragging, setDragging] = useState(false)
  const dragPathRef = useRef<{ x: number; y: number }[]>([])
  const dragButtonRef = useRef<string>('left')     // button held during drag
  const dragStartRef = useRef<{ rx: number; ry: number } | null>(null)
  const dragCurrentRef = useRef<{ rx: number; ry: number } | null>(null)
  const buttonHeldRef = useRef(false)              // target button is physically down
  const lastSampleRef = useRef<number>(0)           // throttle drag sampling at 50ms
  const pressedKeysRef = useRef<PressedKey[]>([])   // currently held keys (for auto-release on blur)
  const stageRef = useRef<HTMLDivElement>(null)     // preview stage (for blur containment)
  const containerRef = useRef<HTMLDivElement>(null)
  const imeInputRef = useRef<HTMLInputElement>(null)
  const composingRef = useRef(false)
  const imeFlushingRef = useRef(false) // avoid re-entrant dock → target maps
  const wheelRemainderRef = useRef(0)

  // ── Visual feedback (key toast stays on canvas; click/drag drawn on real target) ──
  const [keyToast, setKeyToast] = useState<KeyToast | null>(null)
  const idCounterRef = useRef(0)
  const nextId = () => { idCounterRef.current += 1; return idCounterRef.current }
  const lastMoveSendRef = useRef(0)          // throttle mouse-move forwarding at 60fps
  const lastCursorRef = useRef(0)            // throttle cursor overlay update at 30fps
  const lastDragOverlayRef = useRef(0)

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

  // ── Clear real-target overlays when mapping or preview toggles off ──
  useEffect(() => {
    if (!mappingEnabled || !previewing) {
      setCursorPos(null)
      hostCall('target_overlays_hide').catch(() => {})
    }
  }, [mappingEnabled, previewing])

  // Thin client: local preview-mapping disabled (controller is external Web/TCP).
  useEffect(() => {
    if (THIN_CLIENT && mappingEnabled) setMappingEnabled(false)
  }, [mappingEnabled, setMappingEnabled])

  // Test harness only: pin GAM above targets while preview-mapping (same-box
  // z-order). Production agent never needs this — it has no preview canvas.
  useEffect(() => {
    if (THIN_CLIENT) return
    const on = mappingEnabled && previewing
    hostCall('set_mapping_controller', { on: on ? 1 : 0 }).catch((err: any) => {
      addLog(`[Input] set_mapping_controller failed: ${err?.message || err}`)
    })
    return () => {
      hostCall('set_mapping_controller', { on: 0 }).catch(() => {})
    }
  }, [mappingEnabled, previewing])

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
      const editingField = e.target instanceof HTMLInputElement || e.target instanceof HTMLTextAreaElement
      if (editingField && e.target !== imeInputRef.current) return
      if (e.repeat) return
      if (!pressedHotkeyRef.current.includes(e.code)) {
        pressedHotkeyRef.current.push(e.code)
      }
      if (seqMatches(pressedHotkeyRef.current, mappingHotkey)) {
        e.preventDefault()
        e.stopPropagation()
        setMappingEnabled((current) => {
          const next = !current
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
    pressedKeysRef.current = []
    for (const k of keys) {
      hostCall('send_input', {
        hwnd: selWin.hwnd, type: 'keyup',
        key: k.key, code: k.code, vk: k.keyCode, method: kM,
      }).catch((err: any) => {
        if (!pressedKeysRef.current.find((held) => held.code === k.code)) {
          pressedKeysRef.current.push(k)
        }
        addLog(`[Input] auto-release failed for ${k.code}: ${err?.message || err}`)
      })
    }
    addLog(`[Input] auto-release requested for ${keys.length} key(s)`)
  }, [selWin.hwnd, kM])

  useEffect(() => {
    if (!mappingEnabled || !previewing) {
      releaseAllKeys()
      composingRef.current = false
      setComposing(false)
      imeInputRef.current?.blur()
      if (imeInputRef.current) imeInputRef.current.value = ''
      return
    }
    // Keyboard focus lives on the IME strip (toolbar), not the target.
    const t = window.setTimeout(() => {
      imeInputRef.current?.focus({ preventScroll: true })
    }, 0)
    return () => clearTimeout(t)
  }, [mappingEnabled, previewing, releaseAllKeys])

  useEffect(() => {
    if (mappingEnabled && previewing) return
    if (buttonHeldRef.current) {
      // releaseHeldButton needs selWin/mM — call inline to avoid stale closure order
      const pos = dragCurrentRef.current ?? dragStartRef.current
      const button = dragButtonRef.current
      buttonHeldRef.current = false
      setDragging(false)
      hostCall('target_drag', { show: 0 }).catch(() => {})
      if (pos) {
        hostCall('send_input', {
          hwnd: selWin.hwnd, type: 'mouseup', button,
          x_norm: pos.rx, y_norm: pos.ry, method: mM,
        }).catch(() => {})
      }
      dragPathRef.current = []
      dragStartRef.current = null
      dragCurrentRef.current = null
    }
  }, [mappingEnabled, previewing, selWin.hwnd, mM])

  // Emergency mouseup if gesture is cancelled (leave / blur / mapping off).
  const releaseHeldButton = useCallback(
    (reason: string) => {
      if (!buttonHeldRef.current) return
      const pos = dragCurrentRef.current ?? dragStartRef.current
      const button = dragButtonRef.current
      buttonHeldRef.current = false
      setDragging(false)
      hostCall('target_drag', { show: 0 }).catch(() => {})
      if (pos) {
        hostCall('send_input', {
          hwnd: selWin.hwnd, type: 'mouseup', button,
          x_norm: pos.rx, y_norm: pos.ry, method: mM,
        }).catch((err: any) => {
          addLog(`[Mouse] emergency mouseup failed: ${err?.message || err}`)
        })
      }
      dragPathRef.current = []
      dragStartRef.current = null
      dragCurrentRef.current = null
      addLog(`[Mouse] press cancelled (${reason})`)
    },
    [selWin.hwnd, mM],
  )

  const handleImeBlur = useCallback((e: React.FocusEvent<HTMLInputElement>) => {
    if (e.relatedTarget instanceof Node && stageRef.current?.contains(e.relatedTarget)) return
    setFocused(false)
    releaseAllKeys()
    releaseHeldButton('ime blur')
  }, [releaseAllKeys, releaseHeldButton])

  // ── Mouse down → IMMEDIATE mousedown on target (real press-hold, not batch) ──
  const handleMouseDown = useCallback(
    (e: React.MouseEvent<HTMLDivElement>) => {
      if (!previewing || !mappingEnabled) return
      const dims = targetDims
      if (!dims || dims.w <= 0 || dims.h <= 0) return
      const rect = e.currentTarget.getBoundingClientRect()
      const { rx, ry, inImage } = getImageCoords(e.clientX, e.clientY, rect, dims.w, dims.h)
      if (!inImage) return

      // Keep IME strip focused — do not let the canvas steal keyboard focus.
      e.preventDefault()
      imeInputRef.current?.focus({ preventScroll: true })

      const button = e.button === 2 ? 'right' : e.button === 1 ? 'middle' : 'left'
      dragButtonRef.current = button
      dragPathRef.current = [{ x: rx, y: ry }]
      dragStartRef.current = { rx, ry }
      dragCurrentRef.current = { rx, ry }
      lastSampleRef.current = Date.now()
      buttonHeldRef.current = true
      setDragging(true)

      hostCall('send_input', {
        hwnd: selWin.hwnd, type: 'mousedown', button,
        x_norm: rx, y_norm: ry, method: mM,
      }).catch((err: any) => {
        buttonHeldRef.current = false
        setDragging(false)
        addLog(`[Mouse] mousedown failed: ${err?.message || err}`)
      })
    },
    [previewing, mappingEnabled, targetDims, selWin.hwnd, mM],
  )

  // ── Mouse move → live held-move (selection) OR hover forwarding ──
  const handleMouseMove = useCallback(
    (e: React.MouseEvent<HTMLDivElement>) => {
      if (!previewing || !mappingEnabled) return
      const dims = targetDims
      if (!dims || dims.w <= 0 || dims.h <= 0) return
      const now = Date.now()
      const rect = e.currentTarget.getBoundingClientRect()

      if (dragging && buttonHeldRef.current) {
        // Stream moves while button is down — required for text selection.
        if (now - lastSampleRef.current < 16) return
        const coords = getImageCoords(e.clientX, e.clientY, rect, dims.w, dims.h)
        if (!coords.inImage) return
        dragPathRef.current.push({ x: coords.rx, y: coords.ry })
        dragCurrentRef.current = { rx: coords.rx, ry: coords.ry }
        lastSampleRef.current = now
        hostCall('send_input', {
          hwnd: selWin.hwnd, type: 'move', button: dragButtonRef.current,
          held: true, x_norm: coords.rx, y_norm: coords.ry, method: mM,
        }).catch(() => {})
        if (now - lastCursorRef.current > 33) {
          lastCursorRef.current = now
          setCursorPos({ rx: coords.rx, ry: coords.ry, px: coords.px, py: coords.py })
          hostCall('cursor_overlay', {
            hwnd: selWin.hwnd, x_norm: coords.rx, y_norm: coords.ry, show: 1,
          }).catch(() => {})
        }
        if (dragStartRef.current && now - lastDragOverlayRef.current > 33) {
          lastDragOverlayRef.current = now
          hostCall('target_drag', {
            show: 1,
            hwnd: selWin.hwnd,
            x0: dragStartRef.current.rx,
            y0: dragStartRef.current.ry,
            x1: coords.rx,
            y1: coords.ry,
          }).catch(() => {})
        }
        return
      }

      if (now - lastCursorRef.current > 33) {
        lastCursorRef.current = now
        const coords = getImageCoords(e.clientX, e.clientY, rect, dims.w, dims.h)
        if (coords.inImage) {
          setCursorPos({ rx: coords.rx, ry: coords.ry, px: coords.px, py: coords.py })
          hostCall('cursor_overlay', {
            hwnd: selWin.hwnd, x_norm: coords.rx, y_norm: coords.ry, show: 1,
          }).catch(() => {})
        } else {
          setCursorPos(null)
          hostCall('cursor_overlay', { show: 0 }).catch(() => {})
        }
      }
      if (!sendMove) return
      if (now - lastMoveSendRef.current < 16) return
      lastMoveSendRef.current = now
      const { rx, ry, inImage } = getImageCoords(e.clientX, e.clientY, rect, dims.w, dims.h)
      if (!inImage) return
      hostCall('send_input', {
        hwnd: selWin.hwnd, type: 'move', x_norm: rx, y_norm: ry, method: mM,
      }).catch(() => {})
    },
    [dragging, previewing, mappingEnabled, targetDims, selWin.hwnd, mM, sendMove],
  )

  // Flash a ripple on the REAL capture target (Monitor canvas keeps only the OS cursor).
  const flashTargetRipple = useCallback(
    (rx: number, ry: number, button = 'left') => {
      hostCall('target_ripple', {
        hwnd: selWin.hwnd, x_norm: rx, y_norm: ry, button,
      }).catch(() => {})
    },
    [selWin.hwnd],
  )

  // Primitive click = mousedown + mouseup (same path as a real user gesture).
  const sendMappedClick = useCallback(
    async (rx: number, ry: number, button = 'left') => {
      flashTargetRipple(rx, ry, button)
      await hostCall('send_input', {
        hwnd: selWin.hwnd, type: 'mousedown', button,
        x_norm: rx, y_norm: ry, method: mM,
      })
      await hostCall('send_input', {
        hwnd: selWin.hwnd, type: 'mouseup', button,
        x_norm: rx, y_norm: ry, method: mM,
      })
      addLog(
        `[Mouse] down+up → hwnd=0x${selWin.hwnd.toString(16)} (${Math.round(rx * 100)}%,${Math.round(ry * 100)}%) [${mM}]`,
      )
    },
    [selWin.hwnd, mM, flashTargetRipple],
  )

  // Primitive drag = mousedown → held moves → mouseup.
  const sendMappedDrag = useCallback(
    async (path: { x: number; y: number }[], button = 'left') => {
      if (!path.length) return
      const first = path[0]
      const last = path[path.length - 1]
      flashTargetRipple(last.x, last.y, button)
      await hostCall('send_input', {
        hwnd: selWin.hwnd, type: 'mousedown', button,
        x_norm: first.x, y_norm: first.y, method: mM,
      })
      for (let i = 1; i < path.length; i++) {
        await hostCall('send_input', {
          hwnd: selWin.hwnd, type: 'move', button, held: true,
          x_norm: path[i].x, y_norm: path[i].y, method: mM,
        })
      }
      await hostCall('send_input', {
        hwnd: selWin.hwnd, type: 'mouseup', button,
        x_norm: last.x, y_norm: last.y, method: mM,
      })
      addLog(
        `[Mouse] drag ${path.length} pts (down/move/up) → hwnd=0x${selWin.hwnd.toString(16)} [${mM}]`,
      )
    },
    [selWin.hwnd, mM, flashTargetRipple],
  )

  // ── Expose imperative API to parent (Self-Test orchestrator) ──
  useEffect(() => {
    if (!apiRef) return
    apiRef.current = {
      sendClick: (rx, ry, b = 'left') =>
        sendMappedClick(rx, ry, b).catch((err: any) => {
          addLog(`[Mouse] click failed: ${err?.message || err}`)
        }),
      sendWheel: (rx, ry, delta) =>
        hostCall('send_input', {
          hwnd: selWin.hwnd, type: 'wheel', x_norm: rx, y_norm: ry, delta, method: mM,
        }),
      sendDrag: (path, button = 'left') =>
        sendMappedDrag(path, button).catch((err: any) => {
          addLog(`[Mouse] drag failed: ${err?.message || err}`)
        }),
      sendText: (text) =>
        hostCall('send_input', {
          hwnd: selWin.hwnd, type: 'text', text, method: kM,
        }),
      sendKey: (type, key, code, vk) =>
        hostCall('send_input', {
          hwnd: selWin.hwnd, type, key, code, vk, method: kM,
        }),
      ready: () => !!previewing && !!mappingEnabled && !!targetDims && targetDims.w > 0,
    }
    return () => { if (apiRef) apiRef.current = null }
  }, [apiRef, sendMappedClick, sendMappedDrag, previewing, mappingEnabled, targetDims, selWin.hwnd, mM, kM])

  // ── Mouse up → IMMEDIATE mouseup (completes press-hold-move like a real user) ──
  const handleMouseUp = useCallback(
    (e: React.MouseEvent<HTMLDivElement>) => {
      if (!buttonHeldRef.current && !dragging) return
      setDragging(false)
      hostCall('target_drag', { show: 0 }).catch(() => {})

      const dims = targetDims
      const rect = e.currentTarget.getBoundingClientRect()
      const coords = dims && dims.w > 0
        ? getImageCoords(e.clientX, e.clientY, rect, dims.w, dims.h)
        : null
      const rx = coords?.rx ?? dragCurrentRef.current?.rx ?? dragStartRef.current?.rx ?? 0
      const ry = coords?.ry ?? dragCurrentRef.current?.ry ?? dragStartRef.current?.ry ?? 0
      const button = dragButtonRef.current
      const start = dragStartRef.current
      const movedPx = start && dims
        ? Math.hypot((rx - start.rx) * dims.w, (ry - start.ry) * dims.h)
        : 0

      if (buttonHeldRef.current) {
        buttonHeldRef.current = false
        if (mappingEnabled && previewing) {
          // Double-click is just two natural down/up pairs from the browser —
          // no composite dblclick packet.
          hostCall('send_input', {
            hwnd: selWin.hwnd, type: 'mouseup', button,
            x_norm: rx, y_norm: ry, method: mM,
          })
            .then(() => {
              flashTargetRipple(rx, ry, button)
              addLog(
                `[Mouse] mouseup${movedPx >= 3 ? ' (after drag)' : ''}${e.detail === 2 ? ' detail=2' : ''} → hwnd=0x${selWin.hwnd.toString(16)} (${Math.round(rx * 100)}%,${Math.round(ry * 100)}%) [${mM}]`,
              )
            })
            .catch((err: any) => {
              addLog(`[Mouse] mouseup failed: ${err?.message || err}`)
            })
        }
      }

      dragPathRef.current = []
      dragStartRef.current = null
      dragCurrentRef.current = null
    },
    [dragging, mappingEnabled, previewing, selWin.hwnd, mM, targetDims, flashTargetRipple],
  )

  // ── Mouse wheel → normalized delta (deltaMode-aware) ──
  const handleWheel = useCallback(
    (e: React.WheelEvent<HTMLDivElement>) => {
      if (!previewing || !mappingEnabled) return
      const dims = targetDims
      if (!dims || dims.w <= 0 || dims.h <= 0) return
      e.preventDefault()
      const rect = e.currentTarget.getBoundingClientRect()
      const { rx, ry, inImage } = getImageCoords(e.clientX, e.clientY, rect, dims.w, dims.h)
      if (!inImage) return
      // Accumulate high-resolution wheel/trackpad input and emit complete
      // Win32 WHEEL_DELTA units. A typical Chromium mouse notch is ~100 px.
      const wheelUnits = e.deltaMode === 0
        ? e.deltaY * 1.2
        : e.deltaMode === 1
          ? e.deltaY * 120
          : e.deltaY * 1200
      wheelRemainderRef.current += wheelUnits
      const steps = wheelRemainderRef.current > 0
        ? Math.floor(wheelRemainderRef.current / 120)
        : Math.ceil(wheelRemainderRef.current / 120)
      const delta = steps * 120
      if (delta === 0) return
      wheelRemainderRef.current -= delta

      hostCall('send_input', {
        hwnd: selWin.hwnd, type: 'wheel', x_norm: rx, y_norm: ry,
        delta, method: mM,
      })
        .then(() => {
          addLog(
            `[Mouse] wheel ${delta > 0 ? 'down' : 'up'} (${delta}) [${mM}]`,
          )
        })
        .catch((err: any) => {
          addLog(`[Mouse] wheel failed: ${err?.message || err}`)
        })
    },
    [previewing, mappingEnabled, selWin.hwnd, mM, targetDims],
  )

  // ── Keyboard on IME dock — English per-key; Chinese only after commit ──
  // While composing, keys stay local so the system IME candidate UI can work
  // on the Monitor dock. After 上屏, committed text is mapped to the target.
  const handleKeyDown = useCallback(
    (e: React.KeyboardEvent<HTMLInputElement>) => {
      if (!previewing || !mappingEnabled) return
      // Chinese IME: swallow forwarding; let the dock host composition.
      if (composingRef.current || e.nativeEvent.isComposing || e.keyCode === 229) return

      // English / non-IME: do not insert into the dock; forward the real key.
      e.preventDefault()

      const key = e.key
      const code = e.code
      const vk = e.keyCode
      const isModifier = key === 'Control' || key === 'Shift' || key === 'Alt' || key === 'Meta'

      if (!pressedKeysRef.current.find((k) => k.code === code)) {
        pressedKeysRef.current.push({ key, code, keyCode: vk })
      }

      hostCall('send_input', {
        hwnd: selWin.hwnd, type: 'keydown', key, code, vk, method: kM,
      }).catch((err: any) => addLog(`[Key] keydown failed: ${err?.message || err}`))

      const parts: string[] = []
      if (e.ctrlKey) parts.push('Ctrl')
      if (e.altKey) parts.push('Alt')
      if (e.shiftKey && !isModifier) parts.push('Shift')
      if (e.metaKey) parts.push('Win')
      parts.push(key)
      setKeyToast({ text: parts.join('+'), id: nextId() })
    },
    [previewing, mappingEnabled, selWin.hwnd, kM],
  )

  const handleKeyUp = useCallback(
    (e: React.KeyboardEvent<HTMLInputElement>) => {
      if (!previewing || !mappingEnabled) return
      if (composingRef.current || e.nativeEvent.isComposing || e.keyCode === 229) return
      e.preventDefault()

      hostCall('send_input', {
        hwnd: selWin.hwnd, type: 'keyup',
        key: e.key, code: e.code, vk: e.keyCode, method: kM,
      })
        .then(() => {
          pressedKeysRef.current = pressedKeysRef.current.filter((k) => k.code !== e.code)
        })
        .catch((err: any) => addLog(`[Key] keyup failed: ${err?.message || err}`))
    },
    [previewing, mappingEnabled, selWin.hwnd, kM],
  )

  const handleCompositionStart = useCallback(() => {
    composingRef.current = true
    setComposing(true)
  }, [])

  // Dock = IME 上屏落点. Detect new content → map whole string as Unicode to
  // the last-clicked target caret, then clear the dock (focus stays here).
  const flushImeDock = useCallback(
    async (el: HTMLInputElement) => {
      if (!previewing || !mappingEnabled) return
      if (composingRef.current || imeFlushingRef.current) return
      const text = el.value
      if (!text) return

      // Clear immediately so a concurrent compositionend+input flush cannot double-send.
      el.value = ''
      imeFlushingRef.current = true
      try {
        await hostCall('send_input', {
          hwnd: selWin.hwnd,
          type: 'text',
          text,
          method: kM,
        })
        setKeyToast({ text, id: nextId() })
        addLog(`[Key] IME dock → target Unicode (${Array.from(text).length} char(s)) [${kM}]`)
      } catch (err: any) {
        addLog(`[Key] IME dock map failed: ${err?.message || err}`)
        // Put text back so the user can retry / see what failed to map.
        if (!el.value) el.value = text
      } finally {
        imeFlushingRef.current = false
        // Do NOT steal focus back onto the dock — OS caret must stay on the
        // target control (SetFocus there). User clicks the dock when they need
        // another IME composition session.
      }
    },
    [previewing, mappingEnabled, selWin.hwnd, kM],
  )

  const handleCompositionEnd = useCallback(
    (e: React.CompositionEvent<HTMLInputElement>) => {
      // Committed glyphs land in the dock; flush as Unicode on next tick.
      composingRef.current = false
      setComposing(false)
      const el = e.currentTarget
      window.setTimeout(() => { void flushImeDock(el) }, 0)
    },
    [flushImeDock],
  )

  const handleImeInput = useCallback(
    (e: React.FormEvent<HTMLInputElement>) => {
      if (composingRef.current || (e.nativeEvent instanceof InputEvent && e.nativeEvent.isComposing)) {
        return
      }
      const el = e.currentTarget
      window.setTimeout(() => { void flushImeDock(el) }, 0)
    },
    [flushImeDock],
  )

  // ── Thin client: peer workspace (remote view / controlled panic) ──
  const [remoteTargetId, setRemoteTargetId] = useState('')
  const autoPickedRef = useRef(false)
  const isController = peerRole === 'controller'
  const isControlled = peerRole === 'controlled'
  const lanReady = peerTransport !== 'none'

  const pickRemoteTarget = useCallback((w: { title: string; hwnd: number; id?: string }) => {
    const id = w.id || undefined
    setRemoteTargetId(id || String(w.hwnd))
    hostCall('peer_set_target', { hwnd: w.hwnd, title: w.title, id })
      .then((res: { ok?: boolean; error?: string }) => {
        if (res?.ok === false) {
          addLog(`[Peer] set_target failed: ${res.error || 'unknown'}`)
          return
        }
        addLog(`[Peer] set_target ${w.title}${id ? ` (${id})` : ''}`)
      })
      .catch((e) => addLog(`[Peer] set_target failed: ${e}`))
  }, [])

  /** Prefer Main Display / Entire Desktop as default remote target. */
  const pickDefaultRemoteTarget = useCallback((wins: Array<{ title: string; hwnd: number; id?: string }>) => {
    if (!wins.length) return null
    const byDisplay = wins.find((w) => (w.id || '') === 'display:0')
    if (byDisplay) return byDisplay
    const byDesktopId = wins.find((w) => (w.id || '').startsWith('desktop:'))
    if (byDesktopId) return byDesktopId
    const byHwnd0 = wins.find((w) => w.hwnd === 0)
    if (byHwnd0) return byHwnd0
    return wins[0]
  }, [])

  // LAN down → clear selection so next session auto-picks again.
  useEffect(() => {
    if (!lanReady) {
      setRemoteTargetId('')
      autoPickedRef.current = false
    }
  }, [lanReady])

  // Auto set_target once when remote target list first arrives.
  useEffect(() => {
    if (!lanReady || !isController || autoPickedRef.current) return
    if (!remotePeerWindows.length || remoteTargetId) return
    const def = pickDefaultRemoteTarget(remotePeerWindows)
    if (!def) return
    autoPickedRef.current = true
    pickRemoteTarget(def)
    addLog(`[Peer] auto set_target ${def.title}${def.id ? ` (${def.id})` : ''}`)
  }, [lanReady, isController, remotePeerWindows, remoteTargetId, pickDefaultRemoteTarget, pickRemoteTarget])

  // Controlled: mirror outbound H.264 into the same WebCodecs path as remote view.
  useEffect(() => {
    if (!isControlled) return
    return onNativePush((d: Record<string, unknown>) => {
      if (d.type !== 'local_preview') return
      hostCall('local_get_frame').then((fr: {
        ok?: boolean; w?: number; h?: number; flags?: number; b64?: string
      }) => {
        if (!fr?.ok || !fr.b64) return
        const bin = Uint8Array.from(atob(fr.b64), (c) => c.charCodeAt(0))
        window.dispatchEvent(new CustomEvent('peer-h264', { detail: { ...fr, bytes: bin } }))
      }).catch(() => {})
    })
  }, [isControlled])

  const hangupSession = useCallback(async () => {
    // Hangup = end video + control (not just signaling).
    setRemoteTargetId('')
    autoPickedRef.current = false
    try { await hostCall('set_stream_gate', { enabled: false }) } catch { /* */ }
    try { await hostCall('set_control_gate', { enabled: false }) } catch { /* */ }
    try {
      await hostCall('peer_hangup')
      addLog('[Peer] hangup')
    } catch (e) {
      addLog(`[Peer] hangup failed: ${e}`)
    }
  }, [])

  if (THIN_CLIENT) {
    return (
      <div className="flex-1 flex flex-col min-h-0 h-full">
        {isController && (
          <>
            <div className="flex items-center h-11 px-3 bg-bg-secondary border-b border-border shrink-0 gap-2">
              <span className={`${TEXT.sm} font-medium text-text-primary truncate`}>
                {t('peer.remote_workspace')}
              </span>
              <div className="flex gap-1 ml-auto shrink-0 items-center">
                <span className={`${TEXT.tiny} text-text-muted hidden sm:inline mr-0.5`}>
                  {t('peer.control_mode')}
                </span>
                <Tooltip text={t('peer.human_tip')}>
                  <button
                    type="button"
                    onClick={() => {
                      setPeerControlMode?.('human')
                      hostCall('peer_set_control_mode', { mode: 'human' })
                    }}
                    className={`h-7 px-2 rounded-md inline-flex items-center gap-1 ${TEXT.xs} ${
                      peerControlMode === 'human' ? 'bg-accent-soft-mid text-accent' : 'text-text-muted hover:bg-bg-hover'
                    }`}
                  >
                    <User className="w-3.5 h-3.5" />
                    <span>{t('peer.human_short')}</span>
                  </button>
                </Tooltip>
                <Tooltip text={t('peer.ai_tip')}>
                  <button
                    type="button"
                    onClick={() => {
                      setPeerControlMode?.('ai')
                      hostCall('peer_set_control_mode', { mode: 'ai' })
                    }}
                    className={`h-7 px-2 rounded-md inline-flex items-center gap-1 ${TEXT.xs} ${
                      peerControlMode === 'ai' ? 'bg-accent-soft-mid text-accent' : 'text-text-muted hover:bg-bg-hover'
                    }`}
                  >
                    <Bot className="w-3.5 h-3.5" />
                    <span>{t('peer.ai_short')}</span>
                  </button>
                </Tooltip>
              </div>
            </div>
            <div className={`flex-1 flex flex-col min-h-0 ${SHELL_PAD.page} gap-2`}>
              <div className={`${PEER_WORKSPACE.previewWeight}`}>
                <PeerRemoteView
                  active={lanReady}
                  humanControl={peerControlMode === 'human'}
                  encodeHint={encodeHint}
                />
              </div>
              {peerControlMode === 'ai' && lanReady && (
                <div className={`${TEXT.smallMono} text-amber-500 bg-warn-soft ${RADIUS.lg} px-2 py-1.5 shrink-0`}>
                  {t('peer.ai_mode_hint')}
                </div>
              )}
              <div className={`${RADIUS.xl} bg-bg-secondary ${RING} p-2 space-y-1 ${PEER_WORKSPACE.panelWeight} overflow-y-auto`}>
                <div className={`${TEXT.smallMono} font-medium text-text-secondary px-1 flex items-center justify-between gap-2`}>
                  <span>{t('peer.remote_targets')}</span>
                  {lanReady && (
                    <button
                      type="button"
                      className={`${TEXT.xs} text-accent shrink-0 inline-flex items-center gap-1`}
                      onClick={() => {
                        hostCall('peer_request_windows')
                          .then(() => addLog('[Peer] refresh remote targets'))
                          .catch((e) => addLog(`[Peer] request_windows failed: ${e}`))
                      }}
                    >
                      <RefreshCw className="w-3 h-3" />
                      {t('peer.refresh_targets')}
                    </button>
                  )}
                </div>
                {!lanReady && (
                  <div className={`${TEXT.xs} text-text-muted px-1`}>{t('peer.wait_lan')}</div>
                )}
                {lanReady && remotePeerWindows.length === 0 && (
                  <div className={`${TEXT.xs} text-text-muted px-1`}>{t('peer.no_remote_targets')}</div>
                )}
                {remotePeerWindows.map((w) => {
                  const key = w.id || String(w.hwnd)
                  const selected = remoteTargetId === key
                  const needsPriv = (w.id || '').startsWith('app:')
                  return (
                    <button
                      key={key}
                      type="button"
                      className={`w-full text-left ${TEXT.xs} px-2 py-2 min-h-11 ${RADIUS.md} truncate ${
                        selected ? 'bg-accent-soft text-accent' : 'hover:bg-bg-hover'
                      }`}
                      onClick={() => pickRemoteTarget(w)}
                    >
                      {w.title || w.id || `(hwnd ${w.hwnd})`}
                      {needsPriv ? ` · ${t('peer.app_needs_shizuku')}` : ''}
                    </button>
                  )
                })}
                <p className={`${TEXT.tiny} text-text-muted px-1 pt-0.5`}>
                  {t('peer.controller_picks_target')}
                </p>
              </div>
            </div>
          </>
        )}

        {isControlled && (
          <div className={`flex-1 flex flex-col min-h-0 ${SHELL_PAD.page} gap-2`}>
            <div className={`${PEER_WORKSPACE.previewWeight}`}>
              <PeerRemoteView
                active={lanReady}
                humanControl={false}
                source="local"
              />
            </div>
            <div className="text-center space-y-1 max-w-md mx-auto shrink-0">
              <div className={`${TEXT.sm} text-text-primary`}>{t('peer.controlled_title')}</div>
              <div className={`${TEXT.xs} text-text-muted leading-relaxed`}>
                {t('peer.controlled_preview_hint')}
              </div>
            </div>
            <SessionPanicBar
              controlOn={acceptControl}
              onToggleControl={() => onToggleAcceptControl?.()}
              onHangup={hangupSession}
            />
          </div>
        )}

        {!isController && !isControlled && (
          <div className={`flex-1 flex items-center justify-center ${SHELL_PAD.page}`}>
            <div className="text-center space-y-2 max-w-md">
              <div className={`${TEXT.sm} text-text-muted`}>{t('peer.monitor_idle_title')}</div>
              <div className={`${TEXT.xs} text-text-tertiary leading-relaxed`}>
                {t('peer.monitor_idle_body')}
              </div>
            </div>
          </div>
        )}
      </div>
    )
  }

  return (
    <div ref={stageRef} className="flex-1 flex flex-col min-h-0">
      {/* Toolbar — IME strip lives here so preview keeps full width */}
      <div className="flex items-center h-11 px-4 bg-bg-secondary border-b border-border shrink-0 gap-2">
        <div className="flex items-center gap-2 min-w-0 shrink-0">
          <span className="text-sm font-medium text-text-primary truncate max-w-[120px]">
            {selWin.title}
          </span>
          <span className="text-[11px] font-medium text-accent bg-accent-soft px-1.5 py-0.5 rounded shrink-0">
            {stateLabel}
          </span>
        </div>

        {/* Compact IME strip — hidden in thin client (controller is external Web/TCP). */}
        {!THIN_CLIENT && previewing && mappingEnabled ? (
          <div className="flex-1 min-w-0 flex items-center gap-1.5 max-w-[300px]">
            <Tooltip text={composing ? t('monitor.ime_dock_composing') : t('monitor.ime_dock_hint')}>
              <span className={`text-[10px] font-medium shrink-0 ${composing ? 'text-accent' : 'text-text-muted'}`}>
                {t('monitor.ime_dock_title')}
              </span>
            </Tooltip>
            <input
              ref={imeInputRef}
              type="text"
              aria-label={t('monitor.ime_proxy')}
              spellCheck={false}
              autoComplete="off"
              autoCorrect="off"
              autoCapitalize="off"
              defaultValue=""
              onFocus={() => setFocused(true)}
              onBlur={handleImeBlur}
              onKeyDown={handleKeyDown}
              onKeyUp={handleKeyUp}
              onCompositionStart={handleCompositionStart}
              onCompositionEnd={handleCompositionEnd}
              onInput={handleImeInput}
              placeholder={t('monitor.ime_dock_placeholder')}
              className={`flex-1 min-w-0 h-7 px-2 rounded-md text-xs outline-none border transition-colors ${
                composing
                  ? 'border-accent bg-accent-soft text-text-primary'
                  : focused
                    ? 'border-accent-ring bg-bg-primary text-text-primary'
                    : 'border-border bg-bg-primary text-text-primary'
              } placeholder:text-text-tertiary`}
            />
          </div>
        ) : (
          <span className="flex-1" />
        )}

        {!THIN_CLIENT && (
        <Tooltip text={t('monitor.mapping_tip', { hotkey: mappingHotkey })}>
          <button
            onClick={() => {
              setMappingEnabled(!mappingEnabled)
              addLog(`[Input] mapping ${!mappingEnabled ? 'ON' : 'OFF'}`)
            }}
            className={`inline-flex items-center gap-1.5 h-7 px-2 rounded-md text-xs font-medium transition-all duration-150 shrink-0 ${
              mappingEnabled
                ? 'bg-accent-soft text-accent border border-accent-ring'
                : 'border border-border text-text-secondary hover:bg-bg-hover'
            }`}
          >
            <Power className={`w-3 h-3 ${mappingEnabled ? 'text-accent' : 'text-text-muted'}`} />
            <span>{t('monitor.mapping')}</span>
          </button>
        </Tooltip>
        )}
        {THIN_CLIENT && (
          <span className="text-[10px] text-text-muted shrink-0 px-1.5 py-0.5 rounded bg-bg-tertiary">
            {inputPolicy === 'foreground' ? t('monitor.policy_foreground') : t('monitor.policy_background')}
          </span>
        )}
        {!THIN_CLIENT && (
          <ActionBtn
            icon={<Camera className="w-3.5 h-3.5" />}
            label={t('monitor.snapshot')}
            title={t('monitor.snapshot_tip')}
            variant="primary"
            onClick={onTakeSnapshot}
          />
        )}
        {/* Thin client: gates live in the right-rail StreamGatesPanel */}
        {!THIN_CLIENT && (previewing ? (
          <ActionBtn
            icon={<Square className="w-3.5 h-3.5" />}
            label={t('monitor.stop')}
            title={t('monitor.stop_tip')}
            variant="danger"
            onClick={onTogglePreview}
          />
        ) : (
          <ActionBtn
            icon={<Play className="w-3.5 h-3.5" />}
            label={t('monitor.preview')}
            title={t('monitor.preview_tip')}
            variant="outline-accent"
            onClick={onTogglePreview}
          />
        ))}
      </div>

      {/* ── Preview canvas (full width) ── */}
      <div className={`flex-1 overflow-hidden ${SHELL_PAD.page} min-h-0`}>
        <div
          ref={containerRef}
          data-no-page-swipe
          className={`w-full h-full rounded-xl bg-bg-secondary ring-1 ring-inset overflow-hidden flex items-center justify-center relative outline-none transition-shadow ${
            !isDesktop && previewing && mappingEnabled
              ? focused
                ? 'ring-accent shadow-[0_0_0_2px_rgba(38,79,120,0.3)]'
                : 'ring-accent-ring hover:ring-accent-ring'
              : 'ring-border'
          }`}
          onContextMenu={(e) => {
            if (previewing && mappingEnabled) e.preventDefault()
          }}
          onMouseDown={handleMouseDown}
          onMouseMove={handleMouseMove}
          onMouseUp={handleMouseUp}
          onWheel={handleWheel}
          onMouseEnter={() => setMouseOn(true)}
          onMouseLeave={() => {
            setMouseOn(false)
            setCursorPos(null)
            hostCall('cursor_overlay', { show: 0 }).catch(() => {})
            releaseHeldButton('mouse leave')
          }}
        >
          {previewing && children}

          {keyToast && (
            <div className="absolute bottom-12 left-1/2 -translate-x-1/2 px-3 py-1.5 rounded-lg bg-bg-tertiary text-xs text-text-primary font-mono shadow-lg pointer-events-none z-10">
              [{keyToast.text}]
            </div>
          )}

          {/* Mapping-on: no bottom status chip — it occludes the preview. */}
          {mouseOn && isDesktop && previewing && mappingEnabled && isSelfTarget && selfTargetMode === 'warn' && (
            <div className="absolute top-3 left-1/2 -translate-x-1/2 px-3 py-1.5 rounded-lg bg-error-soft text-xs text-error flex items-center gap-1.5 shadow-lg pointer-events-none z-10">
              {t('monitor.self_target_warn')}
            </div>
          )}
          {mouseOn && previewing && !mappingEnabled && (
            <div className="absolute bottom-3 left-1/2 -translate-x-1/2 px-3 py-1.5 rounded-lg bg-bg-tertiary text-xs text-text-muted flex items-center gap-1.5 shadow-lg pointer-events-none z-10">
              <Power className="w-3.5 h-3.5" />
              {t('monitor.preview_no_mapping', { hotkey: mappingHotkey })}
            </div>
          )}

          {THIN_CLIENT ? (
            <div className="absolute inset-0 flex items-center justify-center pointer-events-none p-6">
              <div className="text-center space-y-2 max-w-md">
                <div className="text-sm text-text-muted">{t('monitor.thin_status_title')}</div>
                <div className="text-xs text-text-tertiary leading-relaxed">
                  {t('monitor.thin_status_body', {
                    stream: previewing ? t('monitor.gate_open') : t('monitor.gate_closed'),
                    control: acceptControl ? t('monitor.gate_open') : t('monitor.gate_closed'),
                  })}
                </div>
                <div className="text-[11px] text-text-tertiary font-mono pt-1">
                  http://&lt;server-ip&gt;:9997
                  (controller_server)
                </div>
              </div>
            </div>
          ) : !previewing ? (
            <div className="absolute inset-0 flex items-center justify-center pointer-events-none">
              <div className="text-center space-y-1">
                <div className="text-sm text-text-muted">{t('monitor.no_preview')}</div>
                <div className="text-xs text-text-tertiary">
                  {t('monitor.no_preview_hint')}
                </div>
              </div>
            </div>
          ) : null}
        </div>
      </div>
    </div>
  )
}
