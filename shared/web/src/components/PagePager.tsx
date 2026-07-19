// Horizontal page track — four primary pages side-by-side with drag/swipe animation.
import {
  Children,
  useCallback,
  useEffect,
  useLayoutEffect,
  useRef,
  useState,
  type ReactNode,
  type RefObject,
} from 'react'
import { NAV } from '../lib/design'
import { PRIMARY_PAGES, fractionalPageIndex, pageIndex, type AppPage } from '../lib/pages'

const MIN_DX = 48
const MAX_SLOPE = 0.7
/** Screen-edge strips always allow horizontal page swipe (even over scroll/canvas). */
const EDGE_PX = 28

function blockedTarget(t: EventTarget | null): boolean {
  if (!(t instanceof Element)) return false
  return !!t.closest(
    'input, textarea, select, [contenteditable="true"], canvas, [data-no-page-swipe]',
  )
}

function nearHorizontalEdge(clientX: number, width: number): boolean {
  return clientX <= EDGE_PX || clientX >= width - EDGE_PX
}

function prefersReducedMotion(): boolean {
  return typeof window !== 'undefined'
    && window.matchMedia('(prefers-reduced-motion: reduce)').matches
}

/** Write CSS vars on a host so bottom-nav pill can follow without React re-renders. */
export function writeNavProgress(
  host: HTMLElement | null,
  fractional: number,
  dragging: boolean,
) {
  if (!host) return
  host.style.setProperty('--nav-fraction', String(fractional))
  host.classList.toggle('nav-dragging', dragging)
}

export function PagePager({
  page,
  onPageChange,
  progressHostRef,
  children,
}: {
  page: AppPage
  onPageChange: (p: AppPage) => void
  /** Element that receives --nav-fraction / --nav-dragging (AppShell root). */
  progressHostRef?: RefObject<HTMLElement | null>
  children: ReactNode
}) {
  const panels = Children.toArray(children)
  const index = pageIndex(page)
  const viewportRef = useRef<HTMLDivElement>(null)
  const [width, setWidth] = useState(0)
  const [dragPx, setDragPx] = useState(0)
  const [dragging, setDragging] = useState(false)
  const start = useRef<{
    x: number
    y: number
    id: number
    blocked: boolean
    axis: 'undecided' | 'h' | 'v'
  } | null>(null)
  const indexRef = useRef(index)
  indexRef.current = index
  const widthRef = useRef(width)
  widthRef.current = width
  const onPageChangeRef = useRef(onPageChange)
  onPageChangeRef.current = onPageChange
  const reduceMotion = useRef(prefersReducedMotion())

  useLayoutEffect(() => {
    const el = viewportRef.current
    if (!el) return
    const measure = () => setWidth(el.clientWidth)
    measure()
    const ro = new ResizeObserver(measure)
    ro.observe(el)
    return () => ro.disconnect()
  }, [])

  useEffect(() => {
    const mq = window.matchMedia('(prefers-reduced-motion: reduce)')
    const sync = () => { reduceMotion.current = mq.matches }
    sync()
    mq.addEventListener('change', sync)
    return () => mq.removeEventListener('change', sync)
  }, [])

  // Snap drag offset when page changes from tabs (not mid-drag)
  useEffect(() => {
    if (!dragging) setDragPx(0)
  }, [index, dragging])

  // Keep nav pill in sync (CSS vars — no App re-render during drag)
  useLayoutEffect(() => {
    const frac = fractionalPageIndex(index, dragPx, width)
    writeNavProgress(progressHostRef?.current ?? null, frac, dragging)
  }, [index, dragPx, width, dragging, progressHostRef])

  const settleTo = useCallback((nextIdx: number) => {
    const cur = indexRef.current
    const clamped = Math.max(0, Math.min(PRIMARY_PAGES.length - 1, nextIdx))
    setDragging(false)
    setDragPx(0)
    if (clamped !== cur) {
      onPageChangeRef.current(PRIMARY_PAGES[clamped])
    }
  }, [])

  useEffect(() => {
    const el = viewportRef.current
    if (!el || width <= 0) return

    const onDown = (e: PointerEvent) => {
      if (e.pointerType === 'mouse' && e.button !== 0) return
      const w = widthRef.current
      const edge = nearHorizontalEdge(e.clientX, w)
      start.current = {
        x: e.clientX,
        y: e.clientY,
        id: e.pointerId,
        // Edge swipe always wins so Monitor canvas / scroll areas don't trap navigation.
        blocked: edge ? false : blockedTarget(e.target),
        axis: 'undecided',
      }
    }

    const onMove = (e: PointerEvent) => {
      const s = start.current
      if (!s || s.blocked || s.id !== e.pointerId) return
      const dx = e.clientX - s.x
      const dy = e.clientY - s.y

      if (s.axis === 'undecided') {
        if (Math.abs(dx) < 8 && Math.abs(dy) < 8) return
        s.axis = Math.abs(dx) > Math.abs(dy) * (1 / MAX_SLOPE) ? 'h' : 'v'
        if (s.axis === 'v') return
        setDragging(true)
        try {
          el.setPointerCapture(e.pointerId)
        } catch { /* */ }
      }
      if (s.axis !== 'h') return

      e.preventDefault()
      const i = indexRef.current
      // Rubber-band at ends
      let next = dx
      if ((i === 0 && dx > 0) || (i === PRIMARY_PAGES.length - 1 && dx < 0)) {
        next = dx * 0.35
      }
      setDragPx(next)
    }

    const onUp = (e: PointerEvent) => {
      const s = start.current
      start.current = null
      if (!s || s.blocked || s.id !== e.pointerId) {
        setDragging(false)
        setDragPx(0)
        return
      }
      if (s.axis !== 'h') {
        setDragging(false)
        setDragPx(0)
        return
      }

      const dx = e.clientX - s.x
      const i = indexRef.current
      const w = widthRef.current
      const threshold = Math.max(MIN_DX, w * 0.22)
      if (dx <= -threshold && i < PRIMARY_PAGES.length - 1) {
        settleTo(i + 1)
      } else if (dx >= threshold && i > 0) {
        settleTo(i - 1)
      } else {
        setDragging(false)
        setDragPx(0)
      }
      try {
        el.releasePointerCapture(e.pointerId)
      } catch { /* */ }
    }

    const onCancel = () => {
      start.current = null
      setDragging(false)
      setDragPx(0)
    }

    el.addEventListener('pointerdown', onDown)
    el.addEventListener('pointermove', onMove, { passive: false })
    el.addEventListener('pointerup', onUp)
    el.addEventListener('pointercancel', onCancel)
    return () => {
      el.removeEventListener('pointerdown', onDown)
      el.removeEventListener('pointermove', onMove)
      el.removeEventListener('pointerup', onUp)
      el.removeEventListener('pointercancel', onCancel)
    }
  }, [width, settleTo])

  const tx = width > 0 ? -index * width + dragPx : 0
  const settleMs = reduceMotion.current ? 0 : NAV.settleMs

  return (
    <div ref={viewportRef} className="flex-1 min-h-0 overflow-hidden relative touch-pan-y">
      <div
        className="pager-track flex h-full will-change-transform"
        style={{
          transform: `translate3d(${tx}px, 0, 0)`,
          transition: dragging ? 'none' : `transform ${settleMs}ms ${NAV.settleEase}`,
        }}
      >
        {panels.map((panel, i) => (
          <div
            key={PRIMARY_PAGES[i] ?? i}
            className="h-full shrink-0 flex flex-col min-h-0 overflow-hidden"
            style={{ width: width > 0 ? width : '100%' }}
            aria-hidden={i !== index}
          >
            {panel}
          </div>
        ))}
      </div>
    </div>
  )
}
