// UU-style virtual mouse — one widget: body drag moves remote cursor; L / wheel / R on the same piece.
import { useRef, useState } from 'react'
import { useTranslation } from 'react-i18next'
import { Tooltip } from './Toolkit'
import { TEXT, VMOUSE, RADIUS } from '../lib/design'

/** How many remote-screen widths one full stage-width finger drag covers. */
const SENSITIVITY = 1.15

/** Map viewport finger delta into element-local axes when parent uses rotate(90deg) CW. */
function mapDelta(dSx: number, dSy: number, W: number, H: number, rotated: boolean) {
  if (rotated) {
    // local ← (dSy, -dSx)
    return { dx: (dSy / W) * SENSITIVITY, dy: (-dSx / H) * SENSITIVITY }
  }
  return { dx: (dSx / W) * SENSITIVITY, dy: (dSy / H) * SENSITIVITY }
}

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
  const [pos, setPos] = useState({ x: 0.5, y: 0.5 })

  const clampNorm = (x: number, y: number) => ({
    x_norm: Math.min(1, Math.max(0, x)),
    y_norm: Math.min(1, Math.max(0, y)),
  })

  const applyDelta = (clientX: number, clientY: number, held: boolean) => {
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
    onAction({ type: 'move', held, button: 'left', x_norm: next.x_norm, y_norm: next.y_norm })
  }

  const beginDrag = (clientX: number, clientY: number, el: HTMLElement, pointerId: number) => {
    el.setPointerCapture(pointerId)
    draggingRef.current = true
    lastPtrRef.current = { x: clientX, y: clientY }
  }

  const clickButton = (button: 'left' | 'right') => {
    const { x, y } = posRef.current
    onAction({ type: 'mousedown', button, x_norm: x, y_norm: y })
    onAction({ type: 'mouseup', button, x_norm: x, y_norm: y })
  }

  const wheel = (delta: number) => {
    const { x, y } = posRef.current
    onAction({ type: 'wheel', delta, x_norm: x, y_norm: y })
  }

  if (!enabled || !showPanel) return null

  const aspect = videoAspect && videoAspect > 0 ? videoAspect : 16 / 9

  return (
    <div className="absolute inset-0 z-10 flex items-center justify-center p-1 pointer-events-none" data-no-page-swipe>
      <div
        ref={stageRef}
        className="relative max-w-full max-h-full w-full pointer-events-auto touch-none"
        style={{ aspectRatio: `${aspect}`, height: 'auto' }}
        onPointerDown={(e) => {
          // Empty stage drag also moves cursor (same axis mapping).
          if (e.button !== 0) return
          if ((e.target as HTMLElement).closest('[data-vmouse-ui]')) return
          beginDrag(e.clientX, e.clientY, e.currentTarget, e.pointerId)
        }}
        onPointerMove={(e) => {
          if (!draggingRef.current) return
          applyDelta(e.clientX, e.clientY, true)
        }}
        onPointerUp={() => { draggingRef.current = false }}
        onPointerCancel={() => { draggingRef.current = false }}
      >
        {/* Single mouse widget — hotspot = remote cursor; body drag = move */}
        <div
          data-vmouse-ui
          className={`absolute ${VMOUSE.panel} ${RADIUS.xl} bg-bg-secondary/95 ring-1 ring-inset ring-border shadow-lg select-none pointer-events-auto touch-none`}
          style={{
            left: `${pos.x * 100}%`,
            top: `${pos.y * 100}%`,
            // Tip (hotspot) sits near top-left of the mouse body
            transform: 'translate(-12%, -8%)',
          }}
        >
          {/* Hotspot pip — visual lock between widget and remote cursor */}
          <div
            className="absolute -top-1 -left-1 w-2.5 h-2.5 rounded-full bg-accent ring-2 ring-bg-primary pointer-events-none"
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
              applyDelta(e.clientX, e.clientY, true)
            }}
            onPointerUp={() => { draggingRef.current = false }}
            onPointerCancel={() => { draggingRef.current = false }}
          >
            {t('peer.vmouse_panel')}
          </div>
          <div className="flex items-stretch p-1.5 gap-1">
            <Tooltip text={t('peer.vmouse_left')}>
              <button
                type="button"
                className={`flex-1 ${VMOUSE.btnH} ${RADIUS.lg} ${TEXT.xs} font-semibold bg-bg-tertiary text-text-primary active:bg-accent-soft-mid`}
                onClick={(e) => { e.stopPropagation(); clickButton('left') }}
              >
                {t('peer.vmouse_left_short')}
              </button>
            </Tooltip>
            <div className={`${VMOUSE.wheelW} flex flex-col gap-0.5`}>
              <Tooltip text={t('peer.vmouse_wheel_up')}>
                <button
                  type="button"
                  className={`flex-1 ${RADIUS.md} ${TEXT.tiny} font-medium bg-bg-tertiary text-text-primary active:bg-accent-soft-mid`}
                  onClick={(e) => { e.stopPropagation(); wheel(120) }}
                >
                  ↑
                </button>
              </Tooltip>
              <Tooltip text={t('peer.vmouse_wheel_down')}>
                <button
                  type="button"
                  className={`flex-1 ${RADIUS.md} ${TEXT.tiny} font-medium bg-bg-tertiary text-text-primary active:bg-accent-soft-mid`}
                  onClick={(e) => { e.stopPropagation(); wheel(-120) }}
                >
                  ↓
                </button>
              </Tooltip>
            </div>
            <Tooltip text={t('peer.vmouse_right')}>
              <button
                type="button"
                className={`flex-1 ${VMOUSE.btnH} ${RADIUS.lg} ${TEXT.xs} font-semibold bg-bg-tertiary text-text-primary active:bg-accent-soft-mid`}
                onClick={(e) => { e.stopPropagation(); clickButton('right') }}
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
