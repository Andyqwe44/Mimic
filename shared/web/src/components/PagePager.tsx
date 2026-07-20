// Paged horizontal track — native overflow-x + CSS scroll-snap (same compositor path as vertical).
// Finger drag is browser scrolling (60/120fps). Nav tap uses native smooth scroll (not rAF scrollLeft).
// JS only syncs the nav pill + commits page index from finger settles.
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

/**
 * Hard-stop Android WebView fling / snap inertia.
 * Assigning scrollLeft alone often loses to an in-flight fling; briefly locking
 * overflow forces the scroller to drop momentum before programmatic navigation.
 */
function stopScrollMomentum(vp: HTMLElement) {
  const x = vp.scrollLeft
  const prev = vp.style.overflowX
  vp.style.overflowX = 'hidden'
  vp.scrollLeft = x
  // Force reflow so the overflow lock takes effect before we restore.
  void vp.offsetWidth
  vp.style.overflowX = prev || ''
  vp.scrollLeft = x
}

export function PagePager({
  page,
  onPageChange,
  progressHostRef,
  children,
}: {
  page: AppPage
  onPageChange: (p: AppPage) => void
  progressHostRef?: RefObject<HTMLElement | null>
  children: ReactNode
}) {
  const pageCount = PRIMARY_PAGES.length
  const panels = Children.toArray(children).slice(0, pageCount)
  const index = clampIndex(pageIndex(page), pageCount)

  const viewportRef = useRef<HTMLDivElement>(null)
  const widthRef = useRef(0)
  const progressRef = useRef(index)
  const indexRef = useRef(index)
  indexRef.current = index
  const onPageChangeRef = useRef(onPageChange)
  onPageChangeRef.current = onPageChange

  const reduceMotion = useRef(prefersReducedMotion())
  /** Ignore next page-prop scroll (we already scrolled / committed from finger). */
  const skipPropScroll = useRef(false)
  /** True while finger is down on the pager. */
  const fingerDown = useRef(false)
  /** True while snap inertia may still be running after lift. */
  const userScrolling = useRef(false)
  /**
   * Nav-tap / external page target. While set, settle must NOT commit a different
   * page from mid-fling scrollLeft (fixes Settings→Peers race in logs).
   */
  const navIntent = useRef<number | null>(null)
  /** True while a nav-driven scroll is in flight. */
  const programmaticAnim = useRef(false)
  const settleTimer = useRef(0)
  const intentWatchdog = useRef(0)

  const progressHost = () => progressHostRef?.current ?? null

  const clearSettleTimer = () => {
    if (settleTimer.current) {
      window.clearTimeout(settleTimer.current)
      settleTimer.current = 0
    }
  }

  const clearIntentWatchdog = () => {
    if (intentWatchdog.current) {
      window.clearTimeout(intentWatchdog.current)
      intentWatchdog.current = 0
    }
  }

  const syncPill = (scrollLeft: number, dragging: boolean) => {
    const w = widthRef.current
    if (w <= 0) return
    const p = scrollLeft / w
    progressRef.current = p
    writeNavProgress(progressHost(), p, dragging)
  }

  /** Finger-only: scroll position is SSOT → React page. */
  const commitIndex = (next: number) => {
    const i = clampIndex(next, pageCount)
    if (i !== indexRef.current) {
      skipPropScroll.current = true
      addLog(`[pager] commit ${PRIMARY_PAGES[indexRef.current]}→${PRIMARY_PAGES[i]}`)
      onPageChangeRef.current(PRIMARY_PAGES[i])
    }
  }

  /**
   * Finish nav intent: snap exactly to target, re-enable snap, never commit away.
   * React page was already set by Nav / setPage — scroll follows, does not lead.
   */
  const finishNavIntent = (reason: string) => {
    const vp = viewportRef.current
    const w = widthRef.current
    const target = navIntent.current
    clearIntentWatchdog()
    clearSettleTimer()
    programmaticAnim.current = false
    if (target === null || !vp || w <= 0) {
      navIntent.current = null
      if (vp) vp.style.scrollSnapType = 'x mandatory'
      return
    }
    const exact = target * w
    // Exact slot BEFORE re-enabling snap — otherwise snap may jump to a neighbor.
    vp.style.scrollSnapType = 'none'
    vp.scrollLeft = exact
    syncPill(exact, false)
    navIntent.current = null
    userScrolling.current = false
    vp.style.scrollSnapType = 'x mandatory'
    addLog(`[pager] intent-done idx=${target} (${reason})`)
  }

  /**
   * Bottom-nav tap: stop fling → native smooth scroll (compositor) → intent lock.
   * Pill stays in lockstep via scroll events (same path as finger drag).
   */
  const animateScrollTo = (targetIdx: number, durationMs: number) => {
    const vp = viewportRef.current
    const w = widthRef.current
    if (!vp || w <= 0) return

    clearSettleTimer()
    clearIntentWatchdog()
    stopScrollMomentum(vp)

    const targetLeft = targetIdx * w
    const from = vp.scrollLeft
    navIntent.current = targetIdx
    userScrolling.current = false

    if (reduceMotion.current || Math.abs(from - targetLeft) < 1) {
      vp.style.scrollSnapType = 'none'
      vp.scrollLeft = targetLeft
      syncPill(targetLeft, false)
      finishNavIntent('instant')
      return
    }

    programmaticAnim.current = true
    vp.style.scrollSnapType = 'none'
    // Native smooth scroll — compositor path (matches finger FPS). Avoids rAF
    // scrollLeft which forces main-thread layout every frame on Android WebView.
    vp.scrollTo({ left: targetLeft, behavior: 'smooth' })

    // Watchdog: Android may omit scrollend or leave us short of the target.
    intentWatchdog.current = window.setTimeout(() => {
      intentWatchdog.current = 0
      if (navIntent.current === targetIdx) {
        finishNavIntent('watchdog')
      }
    }, Math.max(durationMs + 80, 280))
  }

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
      // Keep current page after resize (don't animate).
      if (prevW > 0 && Math.abs(prevW - w) > 0.5) {
        const i = navIntent.current ?? indexRef.current
        vp.scrollLeft = i * w
      }
      syncPill(vp.scrollLeft, false)
    }
    measure()
    const ro = new ResizeObserver(measure)
    ro.observe(vp)
    return () => ro.disconnect()
  }, [progressHostRef])

  // Bottom-nav / external page change → scroll to slot.
  useLayoutEffect(() => {
    if (skipPropScroll.current) {
      skipPropScroll.current = false
      return
    }
    const vp = viewportRef.current
    const w = widthRef.current
    if (!vp || w <= 0) return
    const targetLeft = index * w
    if (Math.abs(vp.scrollLeft - targetLeft) < 1 && navIntent.current === null) {
      syncPill(targetLeft, false)
      return
    }
    // Already animating to this slot — keep intent, don't restart.
    if (navIntent.current === index && programmaticAnim.current) {
      return
    }
    const delta = Math.abs(index - progressRef.current)
    addLog(`[pager] prop→scroll idx=${index} from=${progressRef.current.toFixed(2)}`)
    animateScrollTo(index, navTapDurationMs(delta))
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [index])

  useEffect(() => {
    const vp = viewportRef.current
    if (!vp) return

    const settleFromScroll = () => {
      // Nav tap owns the destination until finishNavIntent — never commit nearest
      // from a mid-fling scrollLeft (log: Settings→Peers / Monitor→Settings races).
      if (navIntent.current !== null || programmaticAnim.current) {
        const w = widthRef.current
        const target = navIntent.current ?? indexRef.current
        if (w > 0 && Math.abs(vp.scrollLeft - target * w) <= 2) {
          finishNavIntent('scrollend')
        }
        // else: still moving toward intent — wait for scrollend / watchdog
        return
      }

      const w = widthRef.current
      if (w <= 0) return
      const nearest = clampIndex(Math.round(vp.scrollLeft / w), pageCount)
      const exact = nearest * w
      if (Math.abs(vp.scrollLeft - exact) > 1) {
        // Instant snap — smooth here would race with a subsequent nav tap.
        vp.scrollLeft = exact
      }
      syncPill(vp.scrollLeft, false)
      commitIndex(nearest)
      userScrolling.current = false
    }

    const onScroll = () => {
      const dragging = fingerDown.current
        || userScrolling.current
        || programmaticAnim.current
        || navIntent.current !== null
      syncPill(vp.scrollLeft, dragging)
      if (fingerDown.current) return
      if (navIntent.current !== null || programmaticAnim.current) {
        // Near target → finish early (some WebViews never fire scrollend).
        const w = widthRef.current
        const target = navIntent.current
        if (w > 0 && target !== null && Math.abs(vp.scrollLeft - target * w) <= 1) {
          finishNavIntent('near')
        }
        return
      }
      clearSettleTimer()
      // Fallback when scrollend is missing / delayed (older WebViews).
      settleTimer.current = window.setTimeout(settleFromScroll, 100)
    }

    const onScrollEnd = () => {
      if (fingerDown.current) return
      clearSettleTimer()
      settleFromScroll()
    }

    const onPointerDown = () => {
      // User takes over — cancel nav intent so finger settle can commit.
      if (navIntent.current !== null || programmaticAnim.current) {
        clearIntentWatchdog()
        programmaticAnim.current = false
        navIntent.current = null
        addLog('[pager] intent-cancel (finger)')
      }
      fingerDown.current = true
      userScrolling.current = true
      clearSettleTimer()
      vp.style.scrollSnapType = 'x mandatory'
    }
    const onPointerUp = () => {
      fingerDown.current = false
      userScrolling.current = true
      // Snap may run after lift — settle via scrollend / debounce.
      clearSettleTimer()
      settleTimer.current = window.setTimeout(settleFromScroll, 120)
    }

    vp.addEventListener('scroll', onScroll, { passive: true })
    vp.addEventListener('scrollend', onScrollEnd as EventListener)
    vp.addEventListener('pointerdown', onPointerDown, { passive: true })
    vp.addEventListener('pointerup', onPointerUp, { passive: true })
    vp.addEventListener('pointercancel', onPointerUp, { passive: true })
    addLog(`[pager] native-scroll ready pages=${pageCount} idx=${indexRef.current}`)
    // Seed position without smooth (first paint).
    if (widthRef.current > 0) {
      vp.scrollLeft = indexRef.current * widthRef.current
      syncPill(vp.scrollLeft, false)
    }

    return () => {
      vp.removeEventListener('scroll', onScroll)
      vp.removeEventListener('scrollend', onScrollEnd as EventListener)
      vp.removeEventListener('pointerdown', onPointerDown)
      vp.removeEventListener('pointerup', onPointerUp)
      vp.removeEventListener('pointercancel', onPointerUp)
      clearSettleTimer()
      clearIntentWatchdog()
      programmaticAnim.current = false
      navIntent.current = null
    }
  }, [pageCount, progressHostRef])

  return (
    <div
      ref={viewportRef}
      className="flex-1 min-h-0 overflow-x-auto overflow-y-hidden"
      data-page-pager
      style={{
        scrollSnapType: 'x mandatory',
        WebkitOverflowScrolling: 'touch',
        overscrollBehaviorX: 'contain',
        scrollbarWidth: 'none',
        msOverflowStyle: 'none',
      }}
    >
      <div className="flex h-full w-full">
        {panels.map((panel, i) => (
          <div
            key={PRIMARY_PAGES[i] ?? i}
            className="h-full shrink-0 flex flex-col min-h-0 overflow-hidden"
            style={{
              flex: '0 0 100%',
              width: '100%',
              scrollSnapAlign: 'start',
              scrollSnapStop: 'always',
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
