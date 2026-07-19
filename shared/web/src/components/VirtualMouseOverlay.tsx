// UU-style virtual mouse — driver-level atoms: mousedown / mouseup / move / wheel.
// NW triangle tip is pinned to (x_norm, y_norm); panel body sits down-right of the tip.
import { useEffect, useRef, useState, type PointerEvent as ReactPointerEvent } from 'react'
import { useTranslation } from 'react-i18next'
import { Tooltip } from './Toolkit'
import { TEXT, VMOUSE, RADIUS } from '../lib/design'

/** How many remote-screen widths one full stage-width finger drag covers. */
const SENSITIVITY = 1.15

/** Panel body offset from tip (px) — keeps buttons clear of the hotspot. */
const PANEL_OFFSET_X = 10
const PANEL_OFFSET_Y = 12

/** Map viewport finger delta into element-local axes when parent uses rotate(90deg) CW. */
function mapDelta(dSx: number, dSy: number, W: number, H: number, rotated: boolean) {
  if (rotated) {
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
  fitWidth,
  fitHeight,
  onAction,
}: {
  enabled: boolean
  videoAspect?: number
  /** Parent CSS rotate(90deg) CW fake-landscape. */
  rotated?: boolean
  /** Full mouse widget (expanded). Compact preview: no overlay. */
  showPanel?: boolean
  /** Match PeerRemoteView fitted canvas size when provided. */
  fitWidth?: number
  fitHeight?: number
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
  const useFit = (fitWidth ?? 0) > 0 && (fitHeight ?? 0) > 0

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
    onClick: (e: { stopPropagation: () => void; preventDefault: () => void }) => {
      e.stopPropagation()
      e.preventDefault()
    },
  })

  return (
    <div className="absolute inset-0 z-10 flex items-center justify-center p-0 pointer-events-none" data-no-page-swipe>
      <div
        ref={stageRef}
        className="relative pointer-events-auto touch-none"
        style={
          useFit
            ? { width: fitWidth, height: fitHeight }
            : { aspectRatio: `${aspect}`, width: '100%', maxWidth: '100%', maxHeight: '100%', height: 'auto' }
        }
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
        {/* Anchor at exact remote cursor; tip tip = (x_norm, y_norm). */}
        <div
          data-vmouse-ui
          className="absolute pointer-events-auto touch-none"
          style={{
            left: `${pos.x * 100}%`,
            top: `${pos.y * 100}%`,
          }}
        >
          {/* NW-pointing triangle — tip at (0,0) = exact control point. */}
          <svg
            className="absolute pointer-events-none"
            width={14}
            height={18}
            viewBox="0 0 14 18"
            style={{ left: 0, top: 0 }}
            aria-hidden
          >
            <path
              d="M0 0 L14 10 L7.5 11.5 L10.5 18 L7 18 L4 11.5 Z"
              fill="var(--color-accent, #3b82f6)"
              stroke="#000"
              strokeWidth="1"
              strokeLinejoin="round"
            />
          </svg>
          {/* Panel body offset down-right of tip. */}
          <div
            className={`absolute ${VMOUSE.panel} ${RADIUS.xl} bg-bg-secondary/95 ring-1 ring-inset ring-border shadow-lg ${VMOUSE.stroke} select-none`}
            style={{
              left: PANEL_OFFSET_X,
              top: PANEL_OFFSET_Y,
            }}
          >
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
            <div className={`flex items-stretch ${VMOUSE.pad} gap-1`}>
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
    </div>
  )
}
