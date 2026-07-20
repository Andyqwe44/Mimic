// Paged horizontal track — native overflow-x + CSS scroll-snap (finger drag).
// Nav tap: also native — scrollTo({ behavior: 'smooth' }). Same compositor path.
// Last action wins; settle commits only after a finger gesture (never after nav tap).
import {
  Children,
  useEffect,
  useLayoutEffect,
  useRef,
  type ReactNode,
  type RefObject,
} from 'react'
import { NAV } from '../lib/design'
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

/** Kill fling without overflow:hidden flash. Snap should already be off. */
function freezeScroll(vp: HTMLElement) {
  const x = vp.scrollLeft
  vp.scrollTo({ left: x, behavior: 'auto' })
  vp.scrollLeft = x
}

export function PagePager({
  page,
  navSeq = 0,
  onPageChange,
  progressHostRef,
  children,
}: {
  page: AppPage
  /** Bumped on every bottom/side nav tap (even same page) so retap re-scrolls. */
  navSeq?: number
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
  /** Ignore next page-prop scroll (finger already committed). */
  const skipPropScroll = useRef(false)
  const fingerDown = useRef(false)
  /**
   * Only finger gestures may settle→commit. Nav tap clears this so a leftover
   * scrollend/debounce cannot override the tap (log: intent-done then commit→Peers).
   */
  const settleAllowed = useRef(false)
  /** Target index while native smooth scroll from nav is in flight. */
  const navIntent = useRef<number | null>(null)
  const programmatic = useRef(false)
  const settleTimer = useRef(0)
  const watchdog = useRef(0)

  const progressHost = () => progressHostRef?.current ?? null

  const clearSettleTimer = () => {
    if (settleTimer.current) {
      window.clearTimeout(settleTimer.current)
      settleTimer.current = 0
    }
  }

  const clearWatchdog = () => {
    if (watchdog.current) {
      window.clearTimeout(watchdog.current)
      watchdog.current = 0
    }
  }

  const syncPill = (scrollLeft: number, dragging: boolean) => {
    const w = widthRef.current
    if (w <= 0) return
    const p = scrollLeft / w
    progressRef.current = p
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

  /** Snap exactly to nav target; do NOT re-enable settleAllowed. */
  const finishNavScroll = (reason: string) => {
    const vp = viewportRef.current
    const w = widthRef.current
    const target = navIntent.current
    clearWatchdog()
    clearSettleTimer()
    programmatic.current = false
    if (target === null || !vp || w <= 0) {
      navIntent.current = null
      if (vp) vp.style.scrollSnapType = 'x mandatory'
      return
    }
    const exact = target * w
    vp.style.scrollSnapType = 'none'
    if (Math.abs(vp.scrollLeft - exact) > 0.5) {
      vp.scrollLeft = exact
    }
    syncPill(exact, false)
    navIntent.current = null
    // Keep settleAllowed=false — only next finger pointerdown may commit.
    vp.style.scrollSnapType = 'x mandatory'
    addLog(`[pager] intent-done idx=${target} (${reason})`)
  }

  /**
   * Nav tap: native smooth scroll (browser curve/duration — same family as snap).
   * React page already set; we only move scrollLeft.
   */
  const nativeScrollTo = (targetIdx: number) => {
    const vp = viewportRef.current
    const w = widthRef.current
    if (!vp || w <= 0) return

    clearSettleTimer()
    clearWatchdog()
    settleAllowed.current = false
    fingerDown.current = false

    vp.style.scrollSnapType = 'none'
    freezeScroll(vp)

    const targetLeft = targetIdx * w
    const from = vp.scrollLeft
    navIntent.current = targetIdx

    if (reduceMotion.current || Math.abs(from - targetLeft) < 1) {
      vp.scrollLeft = targetLeft
      syncPill(targetLeft, false)
      finishNavScroll('instant')
      return
    }

    programmatic.current = true
    addLog(
      `[pager] native-smooth→${PRIMARY_PAGES[targetIdx]} `
      + `from=${(from / w).toFixed(2)} Δ=${(Math.abs(targetLeft - from) / w).toFixed(2)}`,
    )
    vp.scrollTo({ left: targetLeft, behavior: 'smooth' })

    // Android may omit scrollend; cap wait then snap (browser smooth ~200–500ms).
    watchdog.current = window.setTimeout(() => {
      watchdog.current = 0
      if (navIntent.current === targetIdx) finishNavScroll('watchdog')
    }, NAV.tapSmoothWatchdogMs)
  }

  const nativeScrollToRef = useRef(nativeScrollTo)
  nativeScrollToRef.current = nativeScrollTo

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

  // Nav / external page → native smooth from current scrollLeft.
  useLayoutEffect(() => {
    if (skipPropScroll.current) {
      skipPropScroll.current = false
      return
    }
    const vp = viewportRef.current
    const w = widthRef.current
    if (!vp || w <= 0) return
    const targetLeft = index * w
    if (
      Math.abs(vp.scrollLeft - targetLeft) < 1
      && navIntent.current === null
      && !programmatic.current
    ) {
      syncPill(targetLeft, false)
      return
    }
    addLog(`[pager] prop→scroll idx=${index} from=${(vp.scrollLeft / w).toFixed(2)} seq=${navSeq}`)
    nativeScrollToRef.current(index)
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [index, navSeq])

  useEffect(() => {
    const vp = viewportRef.current
    if (!vp) return

    const settleFromScroll = () => {
      if (!settleAllowed.current) return
      if (navIntent.current !== null || programmatic.current || fingerDown.current) return

      const w = widthRef.current
      if (w <= 0) return
      const nearest = clampIndex(Math.round(vp.scrollLeft / w), pageCount)
      const exact = nearest * w
      if (Math.abs(vp.scrollLeft - exact) > 1) {
        vp.scrollLeft = exact
      }
      syncPill(vp.scrollLeft, false)
      commitIndex(nearest)
    }

    const onScroll = () => {
      const dragging = fingerDown.current
        || settleAllowed.current
        || programmatic.current
        || navIntent.current !== null
      syncPill(vp.scrollLeft, dragging)

      if (fingerDown.current) return

      if (navIntent.current !== null || programmatic.current) {
        const w = widthRef.current
        const target = navIntent.current
        if (w > 0 && target !== null && Math.abs(vp.scrollLeft - target * w) <= 1) {
          finishNavScroll('near')
        }
        return
      }

      if (!settleAllowed.current) return
      clearSettleTimer()
      settleTimer.current = window.setTimeout(settleFromScroll, 100)
    }

    const onScrollEnd = () => {
      if (fingerDown.current) return
      if (navIntent.current !== null || programmatic.current) {
        finishNavScroll('scrollend')
        return
      }
      if (!settleAllowed.current) return
      clearSettleTimer()
      settleFromScroll()
    }

    const onPointerDown = () => {
      // Finger takes over — cancel nav smooth, allow settle commits again.
      if (navIntent.current !== null || programmatic.current) {
        clearWatchdog()
        programmatic.current = false
        navIntent.current = null
        freezeScroll(vp)
        vp.style.scrollSnapType = 'x mandatory'
        addLog('[pager] intent-cancel (finger)')
      }
      fingerDown.current = true
      settleAllowed.current = true
      clearSettleTimer()
    }

    const onPointerUp = () => {
      fingerDown.current = false
      // Snap inertia may continue — settle via scrollend / debounce.
      if (!settleAllowed.current) return
      clearSettleTimer()
      settleTimer.current = window.setTimeout(settleFromScroll, 120)
    }

    vp.addEventListener('scroll', onScroll, { passive: true })
    vp.addEventListener('scrollend', onScrollEnd as EventListener)
    vp.addEventListener('pointerdown', onPointerDown, { passive: true })
    vp.addEventListener('pointerup', onPointerUp, { passive: true })
    vp.addEventListener('pointercancel', onPointerUp, { passive: true })
    addLog(`[pager] native-scroll ready pages=${pageCount} idx=${indexRef.current}`)
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
      clearWatchdog()
      programmatic.current = false
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
