// UU-style virtual mouse — driver-level atoms: mousedown / mouseup / move / wheel.
import { useEffect, useRef, useState, type PointerEvent as ReactPointerEvent } from 'react'
import { useTranslation } from 'react-i18next'
import { Tooltip } from './Toolkit'
import { TEXT, VMOUSE, RADIUS } from '../lib/design'

/** How many remote-screen widths one full stage-width finger drag covers. */
const SENSITIVITY = 1.15

/**
 * Panel offset so the hotspot (triangle tip) sits further down-right of the panel
 * origin — fingertip / visual click point lands on remote coords.
 * Larger positive % → panel body shifts further down-right of the tip.
 */
const HOTSPOT_PANEL_TX = '-6%'
const HOTSPOT_PANEL_TY = '-4%'

/** Map viewport finger delta into element-local axes when parent uses rotate(90deg) CW. */
function mapDelta(dSx: number, dSy: number, W: number, H: number, rotated: boolean) {
  if (rotated) {
    // local ← (dSy, -dSx)
    return { dx: (dSy / W) * SENSITIVITY, dy: (-dSx / H) * SENSITIVITY }
  }
  return { dx: (dSx / W) * SENSITIVITY, dy: (dSy / H) * SENSITIVITY }
}

type MouseButton = 'left' | 'right'

export function VirtualMouseOverlay({
  enabled,
  videoAspect,
  rotated = false,
  showPanel = true,
  onAction,
}: {
  enabled: boolean
  videoAspect?: number
  /** Parent CSS rotate(90deg) CW fake-landscape. */
  rotated?: boolean
  /** Full mouse widget (expanded). Compact preview: no overlay. */
  showPanel?: boolean
  onAction: (action: Record<string, unknown>) => void
}) {
  const { t } = useTranslation()
  const stageRef = useRef<HTMLDivElement>(null)
  const draggingRef = useRef(false)
  const lastPtrRef = useRef({ x: 0, y: 0 })
  const posRef = useRef({ x: 0.5, y: 0.5 })
  const leftDownRef = useRef(false)
  const rightDownRef = useRef(false)
  const [pos, setPos] = useState({ x: 0.5, y: 0.5 })

  const clampNorm = (x: number, y: number) => ({
    x_norm: Math.min(1, Math.max(0, x)),
    y_norm: Math.min(1, Math.max(0, y)),
  })

  const anyHeld = () => leftDownRef.current || rightDownRef.current
  const heldButton = (): MouseButton =>
    (leftDownRef.current ? 'left' : rightDownRef.current ? 'right' : 'left')

  const releaseButton = (button: MouseButton) => {
    const down = button === 'left' ? leftDownRef : rightDownRef
    if (!down.current) return
    down.current = false
    const { x, y } = posRef.current
    onAction({ type: 'mouseup', button, x_norm: x, y_norm: y })
  }

  const onActionRef = useRef(onAction)
  onActionRef.current = onAction

  // Emergency release if overlay hides / unmounts while a button is held.
  useEffect(() => {
    if (!enabled || !showPanel) {
      if (leftDownRef.current) {
        leftDownRef.current = false
        const { x, y } = posRef.current
        onActionRef.current({ type: 'mouseup', button: 'left', x_norm: x, y_norm: y })
      }
      if (rightDownRef.current) {
        rightDownRef.current = false
        const { x, y } = posRef.current
        onActionRef.current({ type: 'mouseup', button: 'right', x_norm: x, y_norm: y })
      }
    }
    return () => {
      if (leftDownRef.current) {
        leftDownRef.current = false
        const { x, y } = posRef.current
        onActionRef.current({ type: 'mouseup', button: 'left', x_norm: x, y_norm: y })
      }
      if (rightDownRef.current) {
        rightDownRef.current = false
        const { x, y } = posRef.current
        onActionRef.current({ type: 'mouseup', button: 'right', x_norm: x, y_norm: y })
      }
    }
  }, [enabled, showPanel])

  const applyDelta = (clientX: number, clientY: number) => {
    const el = stageRef.current
    if (!el) return
    const W = el.offsetWidth
    const H = el.offsetHeight
    if (W <= 0 || H <= 0) return
    const dSx = clientX - lastPtrRef.current.x
    const dSy = clientY - lastPtrRef.current.y
    lastPtrRef.current = { x: clientX, y: clientY }
    const { dx, dy } = mapDelta(dSx, dSy, W, H, rotated)
    const next = clampNorm(posRef.current.x + dx, posRef.current.y + dy)
    posRef.current = { x: next.x_norm, y: next.y_norm }
    setPos({ x: next.x_norm, y: next.y_norm })
    // held mirrors physical button state only (driver-level: no fake press).
    onAction({
      type: 'move',
      held: anyHeld(),
      button: heldButton(),
      x_norm: next.x_norm,
      y_norm: next.y_norm,
    })
  }

  const beginDrag = (clientX: number, clientY: number, el: HTMLElement, pointerId: number) => {
    el.setPointerCapture(pointerId)
    draggingRef.current = true
    lastPtrRef.current = { x: clientX, y: clientY }
  }

  const endDrag = () => {
    draggingRef.current = false
  }

  /** Physical press — one wire event; also starts drag so hold+move works with same finger. */
  const pressButton = (button: MouseButton, el: HTMLElement, e: ReactPointerEvent) => {
    const down = button === 'left' ? leftDownRef : rightDownRef
    if (down.current) return
    down.current = true
    beginDrag(e.clientX, e.clientY, el, e.pointerId)
    const { x, y } = posRef.current
    onAction({ type: 'mousedown', button, x_norm: x, y_norm: y })
  }

  const wheel = (delta: number) => {
    const { x, y } = posRef.current
    onAction({ type: 'wheel', delta, x_norm: x, y_norm: y })
  }

  if (!enabled || !showPanel) return null

  const aspect = videoAspect && videoAspect > 0 ? videoAspect : 16 / 9

  const bindButton = (button: MouseButton) => ({
    onPointerDown: (e: ReactPointerEvent<HTMLButtonElement>) => {
      e.stopPropagation()
      e.preventDefault()
      if (e.button !== 0) return
      pressButton(button, e.currentTarget, e)
    },
    onPointerMove: (e: ReactPointerEvent<HTMLButtonElement>) => {
      e.stopPropagation()
      if (!draggingRef.current) return
      applyDelta(e.clientX, e.clientY)
    },
    onPointerUp: (e: ReactPointerEvent<HTMLButtonElement>) => {
      e.stopPropagation()
      e.preventDefault()
      endDrag()
      releaseButton(button)
    },
    onPointerCancel: (e: ReactPointerEvent<HTMLButtonElement>) => {
      e.stopPropagation()
      endDrag()
      releaseButton(button)
    },
    // Swallow click — we already emitted down/up atoms.
    onClick: (e: { stopPropagation: () => void; preventDefault: () => void }) => {
      e.stopPropagation()
      e.preventDefault()
    },
  })

  return (
    <div className="absolute inset-0 z-10 flex items-center justify-center p-1 pointer-events-none" data-no-page-swipe>
      <div
        ref={stageRef}
        className="relative max-w-full max-h-full w-full pointer-events-auto touch-none"
        style={{ aspectRatio: `${aspect}`, height: 'auto' }}
        onPointerDown={(e) => {
          if (e.button !== 0) return
          if ((e.target as HTMLElement).closest('[data-vmouse-ui]')) return
          beginDrag(e.clientX, e.clientY, e.currentTarget, e.pointerId)
        }}
        onPointerMove={(e) => {
          if (!draggingRef.current) return
          applyDelta(e.clientX, e.clientY)
        }}
        onPointerUp={endDrag}
        onPointerCancel={endDrag}
      >
        {/* Single mouse widget — triangle tip = remote cursor; body drag = move */}
        <div
          data-vmouse-ui
          className={`absolute ${VMOUSE.panel} ${RADIUS.xl} bg-bg-secondary/95 ring-1 ring-inset ring-border shadow-lg select-none pointer-events-auto touch-none`}
          style={{
            left: `${pos.x * 100}%`,
            top: `${pos.y * 100}%`,
            transform: `translate(${HOTSPOT_PANEL_TX}, ${HOTSPOT_PANEL_TY})`,
          }}
        >
          {/* Precise hotspot: small accent triangle, tip at remote (x_norm, y_norm). */}
          <div
            className="absolute pointer-events-none"
            style={{
              left: 0,
              top: 0,
              width: 0,
              height: 0,
              borderLeft: '4px solid transparent',
              borderRight: '4px solid transparent',
              borderTop: '7px solid var(--color-accent, #3b82f6)',
              transform: 'translate(-50%, -2px)',
            }}
            aria-hidden
          />
          <div
            className={`${VMOUSE.handleH} flex items-center justify-center ${TEXT.tiny} text-text-muted border-b border-border cursor-grab active:cursor-grabbing`}
            onPointerDown={(e) => {
              e.stopPropagation()
              if (e.button !== 0) return
              beginDrag(e.clientX, e.clientY, e.currentTarget, e.pointerId)
            }}
            onPointerMove={(e) => {
              if (!draggingRef.current) return
              applyDelta(e.clientX, e.clientY)
            }}
            onPointerUp={endDrag}
            onPointerCancel={endDrag}
          >
            {t('peer.vmouse_panel')}
          </div>
          <div className="flex items-stretch p-1.5 gap-1">
            <Tooltip text={t('peer.vmouse_left')}>
              <button
                type="button"
                className={`flex-1 ${VMOUSE.btnH} ${RADIUS.lg} ${TEXT.xs} font-semibold bg-bg-tertiary text-text-primary active:bg-accent-soft-mid`}
                {...bindButton('left')}
              >
                {t('peer.vmouse_left_short')}
              </button>
            </Tooltip>
            <div className={`${VMOUSE.wheelW} flex flex-col gap-0.5`}>
              <Tooltip text={t('peer.vmouse_wheel_up')}>
                <button
                  type="button"
                  className={`flex-1 ${RADIUS.md} ${TEXT.tiny} font-medium bg-bg-tertiary text-text-primary active:bg-accent-soft-mid`}
                  onPointerDown={(e) => {
                    e.stopPropagation()
                    e.preventDefault()
                    wheel(120)
                  }}
                  onClick={(e) => { e.stopPropagation(); e.preventDefault() }}
                >
                  ↑
                </button>
              </Tooltip>
              <Tooltip text={t('peer.vmouse_wheel_down')}>
                <button
                  type="button"
                  className={`flex-1 ${RADIUS.md} ${TEXT.tiny} font-medium bg-bg-tertiary text-text-primary active:bg-accent-soft-mid`}
                  onPointerDown={(e) => {
                    e.stopPropagation()
                    e.preventDefault()
                    wheel(-120)
                  }}
                  onClick={(e) => { e.stopPropagation(); e.preventDefault() }}
                >
                  ↓
                </button>
              </Tooltip>
            </div>
            <Tooltip text={t('peer.vmouse_right')}>
              <button
                type="button"
                className={`flex-1 ${VMOUSE.btnH} ${RADIUS.lg} ${TEXT.xs} font-semibold bg-bg-tertiary text-text-primary active:bg-accent-soft-mid`}
                {...bindButton('right')}
              >
                {t('peer.vmouse_right_short')}
              </button>
            </Tooltip>
          </div>
        </div>
      </div>
    </div>
  )
}
