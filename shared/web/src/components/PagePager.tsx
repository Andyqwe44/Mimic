// Native overflow-x + scroll-snap (finger) · nav tap = scrollTo({ behavior:'smooth' }).
// Last user action wins via actionSeq. After nav land, snap stays off (no hold-correct flash).
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

/** Turn off snap so a newer smooth / finger gesture is not fought by compositor snap. */
function disarmSnap(vp: HTMLElement) {
  vp.style.scrollSnapType = 'none'
}

function enableFingerSnap(vp: HTMLElement) {
  vp.style.scrollSnapType = 'x mandatory'
}

/** Cancel in-flight smooth at current offset (finger grab). No overflow lock. */
function cancelScrollAtCurrent(vp: HTMLElement) {
  const x = vp.scrollLeft
  disarmSnap(vp)
  try {
    vp.scrollTo({ left: x, behavior: 'instant' as ScrollBehavior })
  } catch {
    vp.scrollLeft = x
  }
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
  const skipPropScroll = useRef(false)
  const fingerDown = useRef(false)

  /**
   * Monotonic user-action id. Every finger-down and every nav tap bumps this.
   * Settle may commit only when settleArmSeq === actionSeq (armed by that finger).
   * Nav tap sets settleArmSeq = -1 so any older snap settle is ignored.
   */
  const actionSeq = useRef(0)
  const settleArmSeq = useRef(-1)
  /** actionSeq of the in-flight nav smooth (ignore finish if superseded). */
  const navActionSeq = useRef(-1)
  const navIntent = useRef<number | null>(null)
  const programmatic = useRef(false)
  /** After nav lands: hold slot against leftover compositor snap (no commit). */
  const holdIdx = useRef<number | null>(null)

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

  const settleArmed = () => settleArmSeq.current === actionSeq.current && settleArmSeq.current >= 0

  /**
   * End nav smooth. Keep snap OFF after land — re-enabling snap here caused
   * compositor pull + hold-correct flash. Snap returns on next finger-down.
   */
  const finishNavScroll = (reason: string, forSeq: number) => {
    if (forSeq !== actionSeq.current) {
      addLog(`[pager] finish-skip stale seq=${forSeq} now=${actionSeq.current}`)
      return
    }
    const vp = viewportRef.current
    const w = widthRef.current
    const target = navIntent.current
    clearWatchdog()
    clearSettleTimer()
    programmatic.current = false
    if (target === null || !vp || w <= 0) {
      navIntent.current = null
      return
    }
    const exact = target * w
    const dist = Math.abs(vp.scrollLeft - exact)
    vp.style.scrollSnapType = 'none'
    // Only assign if meaningfully off — assigning when already there = hitch.
    if (dist > 2) {
      try {
        vp.scrollTo({ left: exact, behavior: 'instant' as ScrollBehavior })
      } catch {
        vp.scrollLeft = exact
      }
    }
    syncPill(exact, false)
    navIntent.current = null
    holdIdx.current = target
    settleArmSeq.current = -1
    // snap stays none — finger pointerdown re-enables
    addLog(`[pager] intent-done idx=${target} (${reason}) dist=${dist.toFixed(1)}`)
  }

  const finishNavScrollRef = useRef(finishNavScroll)
  finishNavScrollRef.current = finishNavScroll

  /** Nav tap / prop change: last action wins — kill older snap, native smooth to target. */
  const nativeScrollTo = (targetIdx: number) => {
    const vp = viewportRef.current
    const w = widthRef.current
    if (!vp || w <= 0) return

    clearSettleTimer()
    clearWatchdog()

    actionSeq.current += 1
    const mySeq = actionSeq.current
    navActionSeq.current = mySeq
    settleArmSeq.current = -1
    holdIdx.current = null
    fingerDown.current = false

    // Disarm snap only — do NOT freeze at current (that hitch = 顿挫).
    // scrollTo(smooth) cancels the previous scroll and retargets from here.
    disarmSnap(vp)

    const targetLeft = targetIdx * w
    const from = vp.scrollLeft
    navIntent.current = targetIdx

    if (reduceMotion.current || Math.abs(from - targetLeft) < 1) {
      try {
        vp.scrollTo({ left: targetLeft, behavior: 'instant' as ScrollBehavior })
      } catch {
        vp.scrollLeft = targetLeft
      }
      syncPill(targetLeft, false)
      finishNavScrollRef.current('instant', mySeq)
      return
    }

    programmatic.current = true
    addLog(
      `[pager] native-smooth→${PRIMARY_PAGES[targetIdx]} `
      + `from=${(from / w).toFixed(2)} Δ=${(Math.abs(targetLeft - from) / w).toFixed(2)} `
      + `seq=${mySeq}`,
    )
    vp.scrollTo({ left: targetLeft, behavior: 'smooth' })

    // Prefer scrollend / near. Watchdog only after browser had time; no early chop.
    watchdog.current = window.setTimeout(() => {
      watchdog.current = 0
      if (navIntent.current !== targetIdx || navActionSeq.current !== mySeq) return
      const dist = Math.abs(vp.scrollLeft - targetLeft)
      if (dist <= 3) {
        finishNavScrollRef.current('watchdog-near', mySeq)
      } else {
        // Still far — one instant settle (smooth may be stuck fighting old snap).
        finishNavScrollRef.current('watchdog-force', mySeq)
      }
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
        const i = holdIdx.current ?? navIntent.current ?? indexRef.current
        vp.scrollLeft = i * w
      }
      syncPill(vp.scrollLeft, false)
    }
    measure()
    const ro = new ResizeObserver(measure)
    ro.observe(vp)
    return () => ro.disconnect()
  }, [progressHostRef])

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
      && holdIdx.current === index
    ) {
      syncPill(targetLeft, false)
      return
    }
    if (
      Math.abs(vp.scrollLeft - targetLeft) < 1
      && navIntent.current === null
      && !programmatic.current
    ) {
      syncPill(targetLeft, false)
      holdIdx.current = index
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
      // Last-action gate: only the finger gesture that armed settle may commit.
      if (!settleArmed()) {
        return
      }
      if (navIntent.current !== null || programmatic.current || fingerDown.current) return

      const w = widthRef.current
      if (w <= 0) return
      const nearest = clampIndex(Math.round(vp.scrollLeft / w), pageCount)
      const exact = nearest * w
      if (Math.abs(vp.scrollLeft - exact) > 1) {
        try {
          vp.scrollTo({ left: exact, behavior: 'instant' as ScrollBehavior })
        } catch {
          vp.scrollLeft = exact
        }
      }
      enableFingerSnap(vp)
      syncPill(vp.scrollLeft, false)
      holdIdx.current = nearest
      commitIndex(nearest)
    }

    const onScroll = () => {
      const w = widthRef.current
      const dragging = fingerDown.current
        || settleArmed()
        || programmatic.current
        || navIntent.current !== null
      syncPill(vp.scrollLeft, dragging)

      // Safety net while snap is off after nav — correct drift without re-enabling snap
      // (re-enable was a feedback loop: snap pull → hold-correct → flash).
      if (
        !fingerDown.current
        && holdIdx.current !== null
        && navIntent.current === null
        && !programmatic.current
        && w > 0
      ) {
        const exact = holdIdx.current * w
        if (Math.abs(vp.scrollLeft - exact) > 2) {
          vp.style.scrollSnapType = 'none'
          try {
            vp.scrollTo({ left: exact, behavior: 'instant' as ScrollBehavior })
          } catch {
            vp.scrollLeft = exact
          }
          syncPill(exact, false)
          addLog(`[pager] hold-correct idx=${holdIdx.current}`)
          return
        }
      }

      if (fingerDown.current) return

      if (navIntent.current !== null || programmatic.current) {
        const target = navIntent.current
        const mySeq = navActionSeq.current
        if (w > 0 && target !== null && Math.abs(vp.scrollLeft - target * w) <= 2) {
          finishNavScrollRef.current('near', mySeq)
        }
        return
      }

      if (!settleArmed()) return
      clearSettleTimer()
      settleTimer.current = window.setTimeout(settleFromScroll, 100)
    }

    const onScrollEnd = () => {
      if (fingerDown.current) return
      if (navIntent.current !== null || programmatic.current) {
        finishNavScrollRef.current('scrollend', navActionSeq.current)
        return
      }
      if (!settleArmed()) return
      clearSettleTimer()
      settleFromScroll()
    }

    const onPointerDown = () => {
      // Finger is a new last action — supersede nav / old settle.
      actionSeq.current += 1
      settleArmSeq.current = actionSeq.current
      holdIdx.current = null
      if (navIntent.current !== null || programmatic.current) {
        clearWatchdog()
        programmatic.current = false
        navIntent.current = null
        cancelScrollAtCurrent(vp)
        addLog(`[pager] intent-cancel (finger) seq=${actionSeq.current}`)
      }
      // Re-enable snap only for finger gestures (kept off after nav land).
      enableFingerSnap(vp)
      fingerDown.current = true
      clearSettleTimer()
    }

    const onPointerUp = () => {
      fingerDown.current = false
      if (!settleArmed()) return
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
      holdIdx.current = indexRef.current
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
