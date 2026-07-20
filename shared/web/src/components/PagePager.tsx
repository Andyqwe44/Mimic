// Horizontal page track — native CSS scroll-snap while dragging;
// programmatic jumps use rAF (distance-scaled duration) + direct pill translate3d.
import {
  Children,
  useEffect,
  useLayoutEffect,
  useRef,
  type ReactNode,
  type RefObject,
} from 'react'
import { NAV, navTapDurationMs } from '../lib/design'
import { PRIMARY_PAGES, pageIndex, type AppPage } from '../lib/pages'

/**
 * Drive bottom-nav pill via compositor-friendly translate3d (not CSS vars).
 * CSS custom-property updates force main-thread style recalc on Android WebView.
 */
export function writeNavProgress(
  host: HTMLElement | null,
  fractional: number,
  dragging: boolean,
) {
  if (!host) return
  host.style.setProperty('--nav-fraction', String(fractional))
  host.classList.toggle('nav-dragging', dragging)
  const pill = host.querySelector('[data-nav-pill]') as HTMLElement | null
  if (!pill) return
  const track = pill.parentElement
  if (!track) return
  const n = PRIMARY_PAGES.length
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
  const panels = Children.toArray(children)
  const index = pageIndex(page)
  const scrollerRef = useRef<HTMLDivElement>(null)
  const indexRef = useRef(index)
  indexRef.current = index
  const onPageChangeRef = useRef(onPageChange)
  onPageChangeRef.current = onPageChange
  /** True while programmatic rAF jump from a nav tap is in flight. */
  const syncingFromProp = useRef(false)
  /** Skip next layout scroll — page was committed from our own snap settle. */
  const skipNextScrollSync = useRef(false)
  /** Finger/pointer still down on the pager. */
  const gestureActive = useRef(false)
  const settleTimer = useRef(0)
  const animRaf = useRef(0)
  const reduceMotion = useRef(prefersReducedMotion())

  const progressHost = () => progressHostRef?.current ?? null

  const cancelAnim = () => {
    if (animRaf.current) {
      cancelAnimationFrame(animRaf.current)
      animRaf.current = 0
    }
  }

  useEffect(() => {
    const mq = window.matchMedia('(prefers-reduced-motion: reduce)')
    const sync = () => { reduceMotion.current = mq.matches }
    sync()
    mq.addEventListener('change', sync)
    return () => mq.removeEventListener('change', sync)
  }, [])

  // Nav tap / external page change → distance-scaled rAF scroll (pill locked to scrollLeft).
  useLayoutEffect(() => {
    const el = scrollerRef.current
    if (!el) return
    const w = el.clientWidth
    if (w <= 0) return

    if (skipNextScrollSync.current) {
      skipNextScrollSync.current = false
      writeNavProgress(progressHost(), index, false)
      return
    }

    if (gestureActive.current) return

    const target = index * w
    if (Math.abs(el.scrollLeft - target) < 2) {
      writeNavProgress(progressHost(), index, false)
      return
    }

    cancelAnim()
    syncingFromProp.current = true
    window.clearTimeout(settleTimer.current)

    if (reduceMotion.current) {
      el.scrollLeft = target
      writeNavProgress(progressHost(), index, false)
      syncingFromProp.current = false
      return
    }

    const from = el.scrollLeft
    const pageDelta = Math.abs(target - from) / w
    const duration = navTapDurationMs(pageDelta)
    const [x1, y1, x2, y2] = NAV.tapEase
    const t0 = performance.now()
    const prevSnap = el.style.scrollSnapType
    el.style.scrollSnapType = 'none'

    const tick = (now: number) => {
      const t = Math.min(1, (now - t0) / duration)
      const e = bezierEase(t, x1, y1, x2, y2)
      const left = from + (target - from) * e
      el.scrollLeft = left
      writeNavProgress(progressHost(), left / w, true)
      if (t < 1) {
        animRaf.current = requestAnimationFrame(tick)
        return
      }
      animRaf.current = 0
      el.scrollLeft = target
      el.style.scrollSnapType = prevSnap || 'x mandatory'
      writeNavProgress(progressHost(), index, false)
      syncingFromProp.current = false
    }
    animRaf.current = requestAnimationFrame(tick)
  }, [index, progressHostRef])

  useEffect(() => {
    const el = scrollerRef.current
    if (!el) return

    const commitPage = () => {
      if (gestureActive.current || syncingFromProp.current) return
      const w = el.clientWidth
      if (w <= 0) return
      const next = Math.max(0, Math.min(PRIMARY_PAGES.length - 1, Math.round(el.scrollLeft / w)))
      writeNavProgress(progressHost(), next, false)
      if (next !== indexRef.current) {
        skipNextScrollSync.current = true
        onPageChangeRef.current(PRIMARY_PAGES[next])
      }
    }

    const scheduleCommit = () => {
      window.clearTimeout(settleTimer.current)
      settleTimer.current = window.setTimeout(() => {
        if (gestureActive.current || syncingFromProp.current) return
        commitPage()
      }, 120) as unknown as number
    }

    const onScroll = () => {
      if (syncingFromProp.current) return // rAF tick already drives pill
      const w = el.clientWidth
      if (w <= 0) return
      writeNavProgress(progressHost(), el.scrollLeft / w, true)
      if (gestureActive.current) return
      scheduleCommit()
    }

    const onScrollEnd = () => {
      if (gestureActive.current || syncingFromProp.current) return
      window.clearTimeout(settleTimer.current)
      commitPage()
    }

    const onPointerDown = (e: PointerEvent) => {
      if (e.pointerType === 'mouse' && e.button !== 0) return
      // Interrupt tap animation if user grabs the track.
      if (syncingFromProp.current) {
        cancelAnim()
        el.style.scrollSnapType = 'x mandatory'
        syncingFromProp.current = false
      }
      gestureActive.current = true
      window.clearTimeout(settleTimer.current)
      writeNavProgress(progressHost(), el.scrollLeft / Math.max(1, el.clientWidth), true)
    }

    const endGesture = () => {
      if (!gestureActive.current) return
      gestureActive.current = false
      scheduleCommit()
    }

    const onTouchStart = () => {
      if (syncingFromProp.current) {
        cancelAnim()
        el.style.scrollSnapType = 'x mandatory'
        syncingFromProp.current = false
      }
      gestureActive.current = true
      window.clearTimeout(settleTimer.current)
    }

    el.addEventListener('scroll', onScroll, { passive: true })
    el.addEventListener('scrollend', onScrollEnd)
    el.addEventListener('pointerdown', onPointerDown, { passive: true })
    el.addEventListener('touchstart', onTouchStart, { passive: true })
    window.addEventListener('pointerup', endGesture, { passive: true })
    window.addEventListener('pointercancel', endGesture, { passive: true })
    window.addEventListener('touchend', endGesture, { passive: true })
    window.addEventListener('touchcancel', endGesture, { passive: true })

    writeNavProgress(progressHost(), indexRef.current, false)

    const ro = new ResizeObserver(() => {
      const w = el.clientWidth
      if (w <= 0) return
      cancelAnim()
      syncingFromProp.current = true
      el.scrollLeft = indexRef.current * w
      writeNavProgress(progressHost(), indexRef.current, false)
      window.clearTimeout(settleTimer.current)
      settleTimer.current = window.setTimeout(() => {
        syncingFromProp.current = false
      }, 50) as unknown as number
    })
    ro.observe(el)

    return () => {
      el.removeEventListener('scroll', onScroll)
      el.removeEventListener('scrollend', onScrollEnd)
      el.removeEventListener('pointerdown', onPointerDown)
      el.removeEventListener('touchstart', onTouchStart)
      window.removeEventListener('pointerup', endGesture)
      window.removeEventListener('pointercancel', endGesture)
      window.removeEventListener('touchend', endGesture)
      window.removeEventListener('touchcancel', endGesture)
      ro.disconnect()
      window.clearTimeout(settleTimer.current)
      cancelAnim()
    }
  }, [progressHostRef])

  return (
    <div
      ref={scrollerRef}
      className="flex-1 min-h-0 flex overflow-x-auto overflow-y-hidden overscroll-x-contain"
      style={{
        scrollSnapType: 'x mandatory',
        WebkitOverflowScrolling: 'touch',
        scrollbarWidth: 'none',
        msOverflowStyle: 'none',
      }}
      data-page-pager
    >
      {panels.map((panel, i) => (
        <div
          key={PRIMARY_PAGES[i] ?? i}
          className="h-full shrink-0 flex flex-col min-h-0 overflow-hidden"
          style={{
            width: '100%',
            minWidth: '100%',
            scrollSnapAlign: 'start',
            scrollSnapStop: 'always',
          }}
          aria-hidden={i !== index}
        >
          {panel}
        </div>
      ))}
    </div>
  )
}
