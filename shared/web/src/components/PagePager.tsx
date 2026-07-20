// Unified page track — ONE animateTo() for finger settle + bottom-nav tap.
// Position = transform translate3d (no native scroll-snap) so interrupts never flash.
import {
  Children,
  useEffect,
  useLayoutEffect,
  useRef,
  type ReactNode,
  type RefObject,
} from 'react'
import {
  NAV,
  navTapDurationMs,
  resolvePagerAxis,
  rubberBandPage,
} from '../lib/design'
import { PRIMARY_PAGES, pageIndex, type AppPage } from '../lib/pages'
import { addLog } from '../lib/bridge'

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
  pill.style.left = `${padL}px`
  pill.style.width = `${slotW}px`
  return { padL, slotW, pitch, n }
}

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
  pill.style.transform = `translate3d(${fractional * pillLayout.pitch}px,0,0)`
}

function prefersReducedMotion(): boolean {
  return typeof window !== 'undefined'
    && window.matchMedia('(prefers-reduced-motion: reduce)').matches
}

function clampIndex(i: number, pageCount: number): number {
  if (pageCount <= 0) return 0
  return Math.max(0, Math.min(pageCount - 1, i))
}

/** Solve cubic-bezier Y for time t in [0,1] (CSS easing). */
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

type VelSample = { t: number; p: number }

function pickSnapTarget(
  progress: number,
  velocity: number,
  pageCount: number,
): number {
  const fling = NAV.pagerFlingPagesPerMs
  let target: number
  // velocity > 0: content moving to higher index (finger left)
  if (velocity >= fling) {
    target = Math.ceil(progress - 1e-6)
  } else if (velocity <= -fling) {
    target = Math.floor(progress + 1e-6)
  } else {
    const base = Math.floor(progress)
    const frac = progress - base
    if (frac >= 1 - NAV.pagerSnapThreshold) target = base + 1
    else if (frac <= NAV.pagerSnapThreshold) target = base
    else target = Math.round(progress)
  }
  return clampIndex(target, pageCount)
}

export function PagePager({
  page,
  navSeq = 0,
  onPageChange,
  progressHostRef,
  children,
}: {
  page: AppPage
  /** Bumped on every bottom/side nav tap (even same page) so retap re-animates. */
  navSeq?: number
  onPageChange: (p: AppPage) => void
  progressHostRef?: RefObject<HTMLElement | null>
  children: ReactNode
}) {
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
  const animToken = useRef(0)
  const reduceMotion = useRef(prefersReducedMotion())
  /** Ignore next page-prop scroll (finger already animated / committed). */
  const skipPropScroll = useRef(false)
  /** True while a shared animateTo is running (finger settle or nav tap). */
  const animating = useRef(false)
  /** Finger gesture state */
  const finger = useRef<{
    id: number
    startX: number
    startY: number
    startP: number
    axis: 'none' | 'h' | 'v'
    samples: VelSample[]
  } | null>(null)

  const progressHost = () => progressHostRef?.current ?? null

  const cancelAnim = () => {
    if (animRaf.current) {
      cancelAnimationFrame(animRaf.current)
      animRaf.current = 0
    }
    animToken.current += 1
    animating.current = false
  }

  /** Apply fractional page progress to track + pill (single paint path). */
  const applyProgress = (p: number, dragging: boolean) => {
    progressRef.current = p
    const track = trackRef.current
    const w = widthRef.current
    if (track && w > 0) {
      track.style.transform = `translate3d(${-p * w}px,0,0)`
    }
    writeNavProgress(progressHost(), p, dragging)
  }

  const commitIndex = (next: number) => {
    const i = clampIndex(next, pageCount)
    if (i !== indexRef.current) {
      skipPropScroll.current = true
      addLog(`[pager] commit ${PRIMARY_PAGES[indexRef.current]}→${PRIMARY_PAGES[i]}`)
      onPageChangeRef.current(PRIMARY_PAGES[i])
    }
  }

  /**
   * Shared transition — finger settle AND nav tap both call this.
   * Interrupting = cancelAnim + new animateTo from current progress (no flash).
   */
  const animateTo = (
    targetIdx: number,
    opts?: { commit?: boolean; reason?: string },
  ) => {
    const w = widthRef.current
    if (w <= 0) return
    const target = clampIndex(targetIdx, pageCount)
    const from = progressRef.current
    const durationMs = navTapDurationMs(Math.abs(target - from))
    const commit = opts?.commit !== false
    const reason = opts?.reason ?? 'anim'

    cancelAnim()
    finger.current = null

    if (reduceMotion.current || Math.abs(from - target) < 0.001) {
      applyProgress(target, false)
      if (commit) commitIndex(target)
      addLog(`[pager] snap→${PRIMARY_PAGES[target]} (${reason})`)
      return
    }

    animating.current = true
    const token = animToken.current
    const [x1, y1, x2, y2] = NAV.pageAnimEase
    const t0 = performance.now()
    addLog(
      `[pager] ease→${PRIMARY_PAGES[target]} from=${from.toFixed(2)} `
      + `Δ=${Math.abs(target - from).toFixed(2)} ${durationMs}ms (${reason})`,
    )

    const tick = (now: number) => {
      if (token !== animToken.current) {
        animRaf.current = 0
        return
      }
      const t = Math.min(1, (now - t0) / durationMs)
      const e = bezierEase(t, x1, y1, x2, y2)
      applyProgress(from + (target - from) * e, true)
      if (t < 1) {
        animRaf.current = requestAnimationFrame(tick)
        return
      }
      animRaf.current = 0
      animating.current = false
      applyProgress(target, false)
      if (commit) commitIndex(target)
      addLog(`[pager] intent-done idx=${target} (${reason})`)
    }
    animRaf.current = requestAnimationFrame(tick)
  }

  // Keep animateTo stable for event handlers via ref.
  const animateToRef = useRef(animateTo)
  animateToRef.current = animateTo

  useEffect(() => {
    const mq = window.matchMedia('(prefers-reduced-motion: reduce)')
    const sync = () => { reduceMotion.current = mq.matches }
    sync()
    mq.addEventListener('change', sync)
    return () => mq.removeEventListener('change', sync)
  }, [])

  useLayoutEffect(() => {
    const vp = viewportRef.current
    if (!vp) return
    const measure = () => {
      const w = vp.clientWidth
      if (w <= 0) return
      const prevW = widthRef.current
      widthRef.current = w
      invalidateNavPillLayout()
      if (prevW > 0 && Math.abs(prevW - w) > 0.5) {
        applyProgress(progressRef.current, false)
      } else {
        applyProgress(progressRef.current, false)
      }
    }
    measure()
    const ro = new ResizeObserver(measure)
    ro.observe(vp)
    return () => ro.disconnect()
  }, [progressHostRef])

  // Nav tap / external page change → same animateTo as finger settle.
  useLayoutEffect(() => {
    if (skipPropScroll.current) {
      skipPropScroll.current = false
      return
    }
    if (widthRef.current <= 0) {
      progressRef.current = index
      return
    }
    // At rest on target — no-op (same-page retap at rest).
    if (
      !animating.current
      && !finger.current
      && Math.abs(progressRef.current - index) < 0.001
    ) {
      return
    }
    // Nav owns React page already — animate track only (don't re-commit).
    animateToRef.current(index, { commit: false, reason: `nav seq=${navSeq}` })
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [index, navSeq])

  // Finger drag — same progress + animateTo pipeline (no native scroll).
  useEffect(() => {
    const vp = viewportRef.current
    if (!vp) return

    const onPointerDown = (e: PointerEvent) => {
      if (e.pointerType === 'mouse' && e.button !== 0) return
      // Last action wins: finger interrupts in-flight ease.
      cancelAnim()
      finger.current = {
        id: e.pointerId,
        startX: e.clientX,
        startY: e.clientY,
        startP: progressRef.current,
        axis: 'none',
        samples: [{ t: performance.now(), p: progressRef.current }],
      }
    }

    const onPointerMove = (e: PointerEvent) => {
      const g = finger.current
      if (!g || g.id !== e.pointerId) return
      const w = widthRef.current
      if (w <= 0) return
      const dx = e.clientX - g.startX
      const dy = e.clientY - g.startY
      if (g.axis === 'none') {
        g.axis = resolvePagerAxis(Math.abs(dx), Math.abs(dy))
        if (g.axis === 'none') return
        if (g.axis === 'v') {
          // Vertical — release to child scrollers; abandon pager gesture.
          finger.current = null
          return
        }
        // Horizontal lock — capture so moves keep arriving.
        try { vp.setPointerCapture(e.pointerId) } catch { /* */ }
      }
      if (g.axis !== 'h') return
      e.preventDefault()
      // Finger left → higher page index
      const raw = g.startP - dx / w
      const p = rubberBandPage(raw, pageCount)
      const now = performance.now()
      g.samples.push({ t: now, p })
      if (g.samples.length > 8) g.samples.shift()
      applyProgress(p, true)
    }

    const endGesture = (e: PointerEvent) => {
      const g = finger.current
      if (!g || g.id !== e.pointerId) return
      finger.current = null
      try { vp.releasePointerCapture(e.pointerId) } catch { /* */ }
      if (g.axis !== 'h') return

      const now = performance.now()
      const stale = NAV.pagerFlingStaleMs
      const samples = g.samples.filter((s) => now - s.t <= stale)
      let velocity = 0
      if (samples.length >= 2) {
        const a = samples[0]
        const b = samples[samples.length - 1]
        const dt = b.t - a.t
        if (dt >= NAV.pagerFlingMinMs) {
          velocity = (b.p - a.p) / dt
        }
      }
      const moved = Math.abs(progressRef.current - g.startP)
      if (moved < NAV.pagerFlingMinDelta && Math.abs(velocity) < NAV.pagerFlingPagesPerMs) {
        // Tiny nudge — snap back to start page of gesture (or nearest).
        const target = pickSnapTarget(progressRef.current, 0, pageCount)
        animateToRef.current(target, { commit: true, reason: 'finger' })
        return
      }
      const target = pickSnapTarget(progressRef.current, velocity, pageCount)
      animateToRef.current(target, { commit: true, reason: 'finger' })
    }

    vp.addEventListener('pointerdown', onPointerDown, { passive: true })
    vp.addEventListener('pointermove', onPointerMove, { passive: false })
    vp.addEventListener('pointerup', endGesture, { passive: true })
    vp.addEventListener('pointercancel', endGesture, { passive: true })
    addLog(`[pager] unified ready pages=${pageCount} idx=${indexRef.current} `
      + `${NAV.pageAnimMs}ms ease`)
    applyProgress(indexRef.current, false)

    return () => {
      vp.removeEventListener('pointerdown', onPointerDown)
      vp.removeEventListener('pointermove', onPointerMove)
      vp.removeEventListener('pointerup', endGesture)
      vp.removeEventListener('pointercancel', endGesture)
      cancelAnim()
      finger.current = null
    }
  }, [pageCount, progressHostRef])

  return (
    <div
      ref={viewportRef}
      className="flex-1 min-h-0 overflow-hidden"
      data-page-pager
      style={{
        touchAction: 'pan-y',
        overscrollBehaviorX: 'none',
      }}
    >
      <div
        ref={trackRef}
        className="flex h-full will-change-transform"
        style={{
          width: `${pageCount * 100}%`,
          transform: 'translate3d(0,0,0)',
        }}
      >
        {panels.map((panel, i) => (
          <div
            key={PRIMARY_PAGES[i] ?? i}
            className="h-full shrink-0 flex flex-col min-h-0 overflow-hidden"
            style={{
              width: `${100 / pageCount}%`,
              flex: `0 0 ${100 / pageCount}%`,
            }}
            aria-hidden={i !== index}
          >
            {panel}
          </div>
        ))}
      </div>
    </div>
  )
}
