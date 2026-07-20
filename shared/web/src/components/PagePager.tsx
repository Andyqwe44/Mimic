// Horizontal page track — native CSS scroll-snap (compositor-driven).
// No per-frame React state / no per-child touch hijacking.
import {
  Children,
  useEffect,
  useLayoutEffect,
  useRef,
  type ReactNode,
  type RefObject,
} from 'react'
import { PRIMARY_PAGES, pageIndex, type AppPage } from '../lib/pages'

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
  /** Element that receives --nav-fraction / --nav-dragging (AppShell root). */
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
  /** True while programmatic scrollTo from a nav tap is in flight. */
  const syncingFromProp = useRef(false)
  /** Skip next layout scroll — page was committed from our own snap settle. */
  const skipNextScrollSync = useRef(false)
  /** Finger/pointer still down on the pager. */
  const gestureActive = useRef(false)
  const settleTimer = useRef(0)
  const reduceMotion = useRef(prefersReducedMotion())

  useEffect(() => {
    const mq = window.matchMedia('(prefers-reduced-motion: reduce)')
    const sync = () => { reduceMotion.current = mq.matches }
    sync()
    mq.addEventListener('change', sync)
    return () => mq.removeEventListener('change', sync)
  }, [])

  // Nav tap / external page change → native scroll (no transform math).
  // Swipe-driven commits set skipNextScrollSync so we never fight the finger.
  useLayoutEffect(() => {
    const el = scrollerRef.current
    if (!el) return
    const w = el.clientWidth
    if (w <= 0) return

    if (skipNextScrollSync.current) {
      skipNextScrollSync.current = false
      writeNavProgress(progressHostRef?.current ?? null, index, false)
      return
    }

    // User is mid-gesture — don't inject scrollTo (causes page flicker).
    if (gestureActive.current) {
      return
    }

    const target = index * w
    if (Math.abs(el.scrollLeft - target) < 2) {
      writeNavProgress(progressHostRef?.current ?? null, index, false)
      return
    }
    syncingFromProp.current = true
    el.scrollTo({
      left: target,
      behavior: reduceMotion.current ? 'auto' : 'smooth',
    })
    writeNavProgress(progressHostRef?.current ?? null, index, false)
    window.clearTimeout(settleTimer.current)
    settleTimer.current = window.setTimeout(() => {
      syncingFromProp.current = false
    }, reduceMotion.current ? 50 : 360) as unknown as number
  }, [index, progressHostRef])

  useEffect(() => {
    const el = scrollerRef.current
    if (!el) return

    const progressHost = () => progressHostRef?.current ?? null

    const commitPage = () => {
      if (gestureActive.current) return
      const w = el.clientWidth
      if (w <= 0) return
      const next = Math.max(0, Math.min(PRIMARY_PAGES.length - 1, Math.round(el.scrollLeft / w)))
      writeNavProgress(progressHost(), next, false)
      if (syncingFromProp.current) return
      if (next !== indexRef.current) {
        // Page already snapped here — don't scrollTo back in useLayoutEffect.
        skipNextScrollSync.current = true
        onPageChangeRef.current(PRIMARY_PAGES[next])
      }
    }

    const scheduleCommit = () => {
      window.clearTimeout(settleTimer.current)
      // Wait for momentum / snap to finish. Longer than a quick reverse pause.
      settleTimer.current = window.setTimeout(() => {
        if (gestureActive.current) return
        syncingFromProp.current = false
        commitPage()
      }, 120) as unknown as number
    }

    const onScroll = () => {
      const w = el.clientWidth
      if (w <= 0) return
      // Keep pill 1:1 with scroll; leave nav-dragging on until true settle.
      writeNavProgress(progressHost(), el.scrollLeft / w, true)
      // While finger is down, never commit (avoids flicker on quick reverse).
      if (gestureActive.current) return
      // After finger-up (or nav-tap smooth scroll): debounce settle.
      // Android WebView may lack scrollend.
      scheduleCommit()
    }

    const onScrollEnd = () => {
      if (gestureActive.current) return
      window.clearTimeout(settleTimer.current)
      syncingFromProp.current = false
      commitPage()
    }

    const onPointerDown = (e: PointerEvent) => {
      // Only track primary touch/pen on the scroller itself (not nested controls).
      if (e.pointerType === 'mouse' && e.button !== 0) return
      gestureActive.current = true
      window.clearTimeout(settleTimer.current)
      writeNavProgress(progressHost(), el.scrollLeft / Math.max(1, el.clientWidth), true)
    }

    const endGesture = () => {
      if (!gestureActive.current) return
      gestureActive.current = false
      // Momentum may still be running — commit after scroll quiesces.
      scheduleCommit()
    }

    el.addEventListener('scroll', onScroll, { passive: true })
    el.addEventListener('scrollend', onScrollEnd)
    el.addEventListener('pointerdown', onPointerDown, { passive: true })
    // Some Android WebViews are flaky on pointer*; touch* is a belt-and-suspenders.
    const onTouchStart = () => {
      gestureActive.current = true
      window.clearTimeout(settleTimer.current)
    }
    el.addEventListener('touchstart', onTouchStart, { passive: true })
    // pointerup/cancel on window: finger may leave the scroller mid-swipe.
    window.addEventListener('pointerup', endGesture, { passive: true })
    window.addEventListener('pointercancel', endGesture, { passive: true })
    window.addEventListener('touchend', endGesture, { passive: true })
    window.addEventListener('touchcancel', endGesture, { passive: true })

    // Initial pill position
    writeNavProgress(progressHost(), indexRef.current, false)

    const ro = new ResizeObserver(() => {
      const w = el.clientWidth
      if (w <= 0) return
      // Keep current page aligned after rotation / keyboard resize.
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
