// Paged horizontal track — Clash Royale–style snap slots.
// Finger can drag mid-slot; on release always settles to an integer page.
// First/last edges rubber-band then spring back. Page count = PRIMARY_PAGES.length.
import {
  Children,
  useEffect,
  useLayoutEffect,
  useRef,
  type ReactNode,
  type RefObject,
} from 'react'
import { NAV, navTapDurationMs, rubberBandPage } from '../lib/design'
import { PRIMARY_PAGES, pageIndex, type AppPage } from '../lib/pages'

type PillLayout = { padL: number; slotW: number; pitch: number; n: number }

let pillEl: HTMLElement | null = null
let pillHost: HTMLElement | null = null
let pillLayout: PillLayout | null = null
let pillDragging = false

/** Call after nav resize / PRIMARY_PAGES length change. */
export function invalidateNavPillLayout() {
  pillLayout = null
}

function measurePillLayout(pill: HTMLElement): PillLayout {
  const track = pill.parentElement
  const n = PRIMARY_PAGES.length
  if (!track || n <= 0) return { padL: 0, slotW: 0, pitch: 0, n }
  const rem = parseFloat(getComputedStyle(document.documentElement).fontSize) || 16
  const gapPx = NAV.bottomGapRem * rem
  const cs = getComputedStyle(track)
  const padL = parseFloat(cs.paddingLeft) || 0
  const padR = parseFloat(cs.paddingRight) || 0
  const innerW = Math.max(0, track.clientWidth - padL - padR)
  const slotW = (innerW - (n - 1) * gapPx) / n
  const pitch = slotW + gapPx
  // Geometry only when dirty — not every scroll frame.
  pill.style.left = `${padL}px`
  pill.style.width = `${slotW}px`
  return { padL, slotW, pitch, n }
}

/**
 * Drive bottom-nav pill via compositor-friendly translate3d only.
 * Layout (left/width) cached; no getComputedStyle / querySelector on hot path
 * after the first measure.
 */
export function writeNavProgress(
  host: HTMLElement | null,
  fractional: number,
  dragging: boolean,
) {
  if (!host) return
  if (pillHost !== host) {
    pillHost = host
    pillEl = host.querySelector('[data-nav-pill]') as HTMLElement | null
    pillLayout = null
    pillDragging = false
  }
  const pill = pillEl
  if (!pill) return

  if (pillDragging !== dragging) {
    pillDragging = dragging
    host.classList.toggle('nav-dragging', dragging)
  }

  if (!pillLayout || pillLayout.n !== PRIMARY_PAGES.length) {
    pillLayout = measurePillLayout(pill)
  }
  const { pitch } = pillLayout
  pill.style.transform = `translate3d(${fractional * pitch}px,0,0)`
}

/** CSS cubic-bezier unit ease (x1,y1,x2,y2) → y at t∈[0,1]. */
function bezierEase(t: number, x1: number, y1: number, x2: number, y2: number): number {
  if (t <= 0) return 0
  if (t >= 1) return 1
  let x = t
  for (let i = 0; i < 6; i++) {
    const cx = 3 * x1
    const bx = 3 * (x2 - x1) - cx
    const ax = 1 - cx - bx
    const dx = ((ax * x + bx) * x + cx) * x - t
    const d = (3 * ax * x + 2 * bx) * x + cx
    if (Math.abs(d) < 1e-6) break
    x -= dx / d
  }
  const cy = 3 * y1
  const by = 3 * (y2 - y1) - cy
  const ay = 1 - cy - by
  return ((ay * x + by) * x + cy) * x
}

function prefersReducedMotion(): boolean {
  return typeof window !== 'undefined'
    && window.matchMedia('(prefers-reduced-motion: reduce)').matches
}

function clampIndex(i: number, pageCount: number): number {
  if (pageCount <= 0) return 0
  return Math.max(0, Math.min(pageCount - 1, i))
}

/** Nearest snap slot — decision ONLY on release (Clash Royale / ViewPager).
 * Anchor = page where the gesture started, so dragging past halfway then back = regret OK.
 */
function snapTarget(
  progress: number,
  velocity: number,
  pageCount: number,
  startPage: number,
  moveAgeMs: number,
): number {
  const max = pageCount - 1
  if (pageCount <= 0) return 0
  // Edge rubber-band release → first/last
  if (progress < 0) return 0
  if (progress > max) return max

  const start = clampIndex(startPage, pageCount)
  const delta = progress - start
  const flingFresh = moveAgeMs <= NAV.pagerFlingStaleMs
  const fling = NAV.pagerFlingPagesPerMs
  const minD = NAV.pagerFlingMinDelta
  const thr = NAV.pagerSnapThreshold

  // Fling: only if finger was still moving recently, and left the start slot a bit.
  if (flingFresh && velocity >= fling && delta > minD) {
    return clampIndex(start + 1, pageCount)
  }
  if (flingFresh && velocity <= -fling && delta < -minD) {
    return clampIndex(start - 1, pageCount)
  }

  // Position: past halfway from start → neighbor; else spring back (regret).
  if (delta >= thr) return clampIndex(start + 1, pageCount)
  if (delta <= -thr) return clampIndex(start - 1, pageCount)
  return start
}

export function PagePager({
  page,
  onPageChange,
  progressHostRef,
  children,
}: {
  page: AppPage
  onPageChange: (p: AppPage) => void
  /** Element that receives nav-dragging + hosts [data-nav-pill] (AppShell root). */
  progressHostRef?: RefObject<HTMLElement | null>
  children: ReactNode
}) {
  /** SSOT for slot count — add a page by extending PRIMARY_PAGES + a child. */
  const pageCount = PRIMARY_PAGES.length
  const panels = Children.toArray(children).slice(0, pageCount)
  const index = clampIndex(pageIndex(page), pageCount)

  const viewportRef = useRef<HTMLDivElement>(null)
  const trackRef = useRef<HTMLDivElement>(null)
  const widthRef = useRef(0)
  const progressRef = useRef(index)
  const indexRef = useRef(index)
  indexRef.current = index
  const onPageChangeRef = useRef(onPageChange)
  onPageChangeRef.current = onPageChange

  const animRaf = useRef(0)
  const reduceMotion = useRef(prefersReducedMotion())
  /** Skip prop→anim when we just committed this index ourselves. */
  const skipPropAnim = useRef(false)

  // Gesture state (refs — no re-render on move).
  const dragging = useRef(false)
  const axisLocked = useRef<'none' | 'h' | 'v'>('none')
  const startX = useRef(0)
  const startY = useRef(0)
  const startProgress = useRef(0)
  /** Integer page where this gesture began — snap/regret anchor. */
  const startPage = useRef(0)
  const lastX = useRef(0)
  const lastT = useRef(0)
  const velocity = useRef(0) // pages/ms; >0 finger left → progress↑

  const progressHost = () => progressHostRef?.current ?? null

  const cancelAnim = () => {
    if (animRaf.current) {
      cancelAnimationFrame(animRaf.current)
      animRaf.current = 0
    }
  }

  const paint = (p: number, isDragging: boolean) => {
    progressRef.current = p
    const track = trackRef.current
    const w = widthRef.current
    if (track && w > 0) {
      track.style.transform = `translate3d(${-p * w}px,0,0)`
    }
    writeNavProgress(progressHost(), p, isDragging)
  }

  const animateTo = (
    target: number,
    durationMs: number,
    ease: readonly [number, number, number, number],
    onDone?: () => void,
  ) => {
    cancelAnim()
    const from = progressRef.current
    if (reduceMotion.current || Math.abs(from - target) < 0.001) {
      paint(target, false)
      onDone?.()
      return
    }
    const [x1, y1, x2, y2] = ease
    const t0 = performance.now()
    const tick = (now: number) => {
      // Finger took over mid-settle — abandon programmatic anim.
      if (dragging.current) {
        animRaf.current = 0
        return
      }
      const t = Math.min(1, (now - t0) / durationMs)
      const e = bezierEase(t, x1, y1, x2, y2)
      paint(from + (target - from) * e, true)
      if (t < 1) {
        animRaf.current = requestAnimationFrame(tick)
        return
      }
      animRaf.current = 0
      paint(target, false)
      onDone?.()
    }
    animRaf.current = requestAnimationFrame(tick)
  }

  /** Update React page only — never jump the track (animateTo owns transform). */
  const commitIndex = (next: number) => {
    const i = clampIndex(next, pageCount)
    if (i !== indexRef.current) {
      skipPropAnim.current = true
      onPageChangeRef.current(PRIMARY_PAGES[i])
    }
  }

  useEffect(() => {
    const mq = window.matchMedia('(prefers-reduced-motion: reduce)')
    const sync = () => { reduceMotion.current = mq.matches }
    sync()
    mq.addEventListener('change', sync)
    return () => mq.removeEventListener('change', sync)
  }, [])

  // Measure viewport width; keep integer page aligned on resize.
  useLayoutEffect(() => {
    const vp = viewportRef.current
    if (!vp) return
    const measure = () => {
      const w = vp.clientWidth
      if (w <= 0) return
      widthRef.current = w
      invalidateNavPillLayout()
      // During drag: only refresh width — do NOT snap to committed index.
      if (dragging.current) {
        paint(progressRef.current, true)
        return
      }
      cancelAnim()
      paint(indexRef.current, false)
    }
    measure()
    const ro = new ResizeObserver(measure)
    ro.observe(vp)
    return () => ro.disconnect()
  }, [progressHostRef])

  // Nav tap / external page change → distance-scaled settle onto that slot.
  useLayoutEffect(() => {
    if (skipPropAnim.current) {
      // Gesture already animating to this index — do not paint(index) (would jump).
      skipPropAnim.current = false
      return
    }
    if (dragging.current) return
    if (Math.abs(progressRef.current - index) < 0.001) {
      paint(index, false)
      return
    }
    const delta = Math.abs(index - progressRef.current)
    animateTo(index, navTapDurationMs(delta), NAV.tapEase)
    // eslint-disable-next-line react-hooks/exhaustive-deps -- paint/animate use refs
  }, [index])

  // Pointer-driven paging (no native overflow scroll — Android snap is unreliable).
  useEffect(() => {
    const vp = viewportRef.current
    if (!vp) return

    const onPointerDown = (e: PointerEvent) => {
      if (e.pointerType === 'mouse' && e.button !== 0) return
      cancelAnim()
      dragging.current = true
      axisLocked.current = 'none'
      startX.current = e.clientX
      startY.current = e.clientY
      startProgress.current = progressRef.current
      startPage.current = clampIndex(Math.round(progressRef.current), pageCount)
      lastX.current = e.clientX
      lastT.current = e.timeStamp
      velocity.current = 0
    }

    const onPointerMove = (e: PointerEvent) => {
      if (!dragging.current) return
      const w = widthRef.current
      if (w <= 0) return

      const dx = e.clientX - startX.current
      const dy = e.clientY - startY.current

      if (axisLocked.current === 'none') {
        const adx = Math.abs(dx)
        const ady = Math.abs(dy)
        if (adx < NAV.pagerAxisLockPx && ady < NAV.pagerAxisLockPx) return
        axisLocked.current = adx > ady ? 'h' : 'v'
        if (axisLocked.current === 'v') {
          // Vertical wins — abandon horizontal pager for this gesture.
          dragging.current = false
          writeNavProgress(progressHost(), progressRef.current, false)
          return
        }
        try { vp.setPointerCapture(e.pointerId) } catch { /* ignore */ }
        writeNavProgress(progressHost(), progressRef.current, true)
      }
      if (axisLocked.current !== 'h') return

      e.preventDefault()
      const dt = e.timeStamp - lastT.current
      if (dt > 0) {
        // Finger right (+) → progress decreases.
        const dPages = -(e.clientX - lastX.current) / w
        velocity.current = dPages / dt
      }
      lastX.current = e.clientX
      lastT.current = e.timeStamp

      // 1:1 free drag — never commit / never snap while finger is down.
      const raw = startProgress.current - dx / w
      paint(rubberBandPage(raw, pageCount), true)
    }

    const endGesture = (e: PointerEvent) => {
      if (!dragging.current && axisLocked.current !== 'h') return
      const wasH = axisLocked.current === 'h'
      dragging.current = false
      axisLocked.current = 'none'
      try { vp.releasePointerCapture(e.pointerId) } catch { /* ignore */ }

      if (!wasH) {
        writeNavProgress(progressHost(), progressRef.current, false)
        return
      }

      const moveAge = e.timeStamp - lastT.current
      const target = snapTarget(
        progressRef.current,
        velocity.current,
        pageCount,
        startPage.current,
        moveAge,
      )
      const delta = Math.abs(target - progressRef.current)
      const dur = delta < 0.15
        ? NAV.pagerSnapMs
        : navTapDurationMs(Math.max(delta, 0.5))
      // Snap animate first; commit React page without jumping the track.
      commitIndex(target)
      animateTo(target, dur, NAV.pagerSnapEase)
    }

    vp.addEventListener('pointerdown', onPointerDown, { passive: true })
    vp.addEventListener('pointermove', onPointerMove, { passive: false })
    vp.addEventListener('pointerup', endGesture)
    vp.addEventListener('pointercancel', endGesture)
    vp.addEventListener('lostpointercapture', endGesture)

    paint(indexRef.current, false)

    return () => {
      vp.removeEventListener('pointerdown', onPointerDown)
      vp.removeEventListener('pointermove', onPointerMove)
      vp.removeEventListener('pointerup', endGesture)
      vp.removeEventListener('pointercancel', endGesture)
      vp.removeEventListener('lostpointercapture', endGesture)
      cancelAnim()
    }
    // pageCount is PRIMARY_PAGES.length — rebuild if nav slots change
  }, [pageCount, progressHostRef])

  return (
    <div
      ref={viewportRef}
      className="flex-1 min-h-0 overflow-hidden touch-pan-y"
      data-page-pager
      style={{ touchAction: 'pan-y' }}
    >
      <div
        ref={trackRef}
        className="flex h-full w-full will-change-transform"
      >
        {panels.map((panel, i) => (
          <div
            key={PRIMARY_PAGES[i] ?? i}
            className="h-full shrink-0 flex flex-col min-h-0 overflow-hidden"
            style={{ flex: '0 0 100%', width: '100%' }}
            aria-hidden={i !== index}
          >
            {panel}
          </div>
        ))}
      </div>
    </div>
  )
}
