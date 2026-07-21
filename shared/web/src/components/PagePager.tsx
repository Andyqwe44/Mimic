// Native overflow-x (6 slots: blank|4 content|blank) · nav/settle = scrollTo(smooth).
// Pill minimap: same axis x∈[0,5], pillTranslateX = x * pitch. No velocity continuity (N0).
import {
  Children,
  useEffect,
  useLayoutEffect,
  useRef,
  type ReactNode,
  type RefObject,
} from 'react'
import { NAV, resolvePagerAxis } from '../lib/design'
import { PRIMARY_PAGES, pageIndex, type AppPage } from '../lib/pages'
import { addLog } from '../lib/bridge'

/** Set true only when debugging PagePager; keep false so peer video logs stay readable. */
const PAGER_DEBUG = false
function pagerLog(msg: string) {
  if (PAGER_DEBUG) addLog(msg)
}

/** Content pages + left/right bounce blanks. */
const BLANK_SLOTS = 2
const CONTENT_COUNT = PRIMARY_PAGES.length
const SLOT_COUNT = CONTENT_COUNT + BLANK_SLOTS
/** First content slot on axis x (Monitor). */
const X_MIN = 1
/** Last content slot on axis x (Settings). */
const X_MAX = CONTENT_COUNT
const SNAP_EPS = NAV.pagerSnapThreshold

type PillLayout = { padL: number; slotW: number; pitch: number; n: number }
type PtrPhase = 'none' | 'pending' | 'dragging'

let pillEl: HTMLElement | null = null
let pillHost: HTMLElement | null = null
let pillLayout: PillLayout | null = null
let pillDragging = false

/** Call after nav resize / PRIMARY_PAGES length change. */
export function invalidateNavPillLayout() {
  pillLayout = null
}

/**
 * Measure visible 4-tab track; pill origin is slot 0 (one pitch left of first tab).
 * Axis x∈[0,5] → translate = x * pitch (unified with main pager).
 */
function measurePillLayout(pill: HTMLElement): PillLayout {
  const track = pill.parentElement
  const nContent = CONTENT_COUNT
  if (!track || nContent <= 0) return { padL: 0, slotW: 0, pitch: 0, n: SLOT_COUNT }
  const rem = parseFloat(getComputedStyle(document.documentElement).fontSize) || 16
  const gapPx = NAV.bottomGapRem * rem
  const cs = getComputedStyle(track)
  const padL = parseFloat(cs.paddingLeft) || 0
  const padR = parseFloat(cs.paddingRight) || 0
  const innerW = Math.max(0, track.clientWidth - padL - padR)
  const slotW = (innerW - (nContent - 1) * gapPx) / nContent
  const pitch = slotW + gapPx
  // Slot 0 left edge = first tab left − pitch
  pill.style.left = `${padL - pitch}px`
  pill.style.width = `${slotW}px`
  return { padL, slotW, pitch, n: SLOT_COUNT }
}

/**
 * @param axisX fractional slot on 0…5 (same as pager scrollLeft/width)
 */
export function writeNavProgress(
  host: HTMLElement | null,
  axisX: number,
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

  if (!pillLayout || pillLayout.n !== SLOT_COUNT) {
    pillLayout = measurePillLayout(pill)
  }
  pill.style.transform = `translate3d(${axisX * pillLayout.pitch}px,0,0)`
}

function clamp(n: number, lo: number, hi: number): number {
  return Math.max(lo, Math.min(hi, n))
}

/** AppPage → axis x (1…4). */
function pageToAxis(page: AppPage): number {
  return pageIndex(page) + X_MIN
}

/** Axis slot → AppPage (content only). */
function axisToPage(slot: number): AppPage {
  const i = clamp(Math.round(slot) - X_MIN, 0, CONTENT_COUNT - 1)
  return PRIMARY_PAGES[i]
}

function fmtX(x: number): string {
  const lo = clamp(Math.floor(x + 1e-6), 0, SLOT_COUNT - 1)
  const hi = clamp(Math.ceil(x - 1e-6), 0, SLOT_COUNT - 1)
  const name = (s: number) => {
    if (s < X_MIN || s > X_MAX) return 'blank'
    return PRIMARY_PAGES[s - X_MIN] ?? '?'
  }
  if (lo === hi || Math.abs(x - lo) < 0.02) {
    return `x=${x.toFixed(2)}(${name(lo)})`
  }
  return `x=${x.toFixed(2)}(${name(lo)}→${name(hi)})`
}

function fmtTarget(axis: number): string {
  return `${axis}(${axisToPage(axis)})`
}

/**
 * B1 hybrid settle target.
 * - Bounce blanks → nearest content edge.
 * - After ease interrupt / fractional: round(x).
 * - From integer content origin: dead-zone ±SNAP_EPS else round with at-least-one-step.
 */
function pickSettleTarget(
  x: number,
  originX: number | null,
  interruptedEase: boolean,
): number {
  if (x < X_MIN) return X_MIN
  if (x > X_MAX) return X_MAX

  if (interruptedEase || originX === null) {
    return clamp(Math.round(x), X_MIN, X_MAX)
  }

  const origin = clamp(Math.round(originX), X_MIN, X_MAX)
  const nearOrigin = Math.abs(originX - origin) < 0.02
  if (nearOrigin && !interruptedEase) {
    const delta = x - origin
    if (delta > SNAP_EPS) {
      return clamp(Math.max(origin + 1, Math.round(x)), X_MIN, X_MAX)
    }
    if (delta < -SNAP_EPS) {
      return clamp(Math.min(origin - 1, Math.round(x)), X_MIN, X_MAX)
    }
    return origin
  }

  return clamp(Math.round(x), X_MIN, X_MAX)
}

function cancelScrollAtCurrent(vp: HTMLElement) {
  const left = vp.scrollLeft
  try {
    vp.scrollTo({ left, behavior: 'instant' as ScrollBehavior })
  } catch {
    vp.scrollLeft = left
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
  const panels = Children.toArray(children).slice(0, CONTENT_COUNT)
  const targetAxis = clamp(pageToAxis(page), X_MIN, X_MAX)

  const viewportRef = useRef<HTMLDivElement>(null)
  const widthRef = useRef(0)
  const progressRef = useRef(targetAxis)
  const pageRef = useRef(page)
  pageRef.current = page
  const onPageChangeRef = useRef(onPageChange)
  onPageChangeRef.current = onPageChange

  const skipPropScroll = useRef(false)

  const actionSeq = useRef(0)
  const navActionSeq = useRef(-1)
  /** Target axis slot for in-flight smooth (1…4). */
  const navIntent = useRef<number | null>(null)
  const programmatic = useRef(false)
  /** Settled content axis; used for resize pin. */
  const holdAxis = useRef<number | null>(targetAxis)

  /**
   * Finger drag started from this axis (integer content) → B1 ±0.15 path.
   * Null after ease-interrupt freeze (B2-A → round on short tap).
   */
  const dragOriginX = useRef<number | null>(null)
  const interruptedEase = useRef(false)

  const settleTimer = useRef(0)
  const watchdog = useRef(0)
  const navRaf = useRef(0)
  const lastProgLogTs = useRef(0)
  const lastProgLogX = useRef(Number.NaN)

  const ptrPhase = useRef<PtrPhase>('none')
  const ptrStartX = useRef(0)
  const ptrStartY = useRef(0)
  const ptrId = useRef<number | null>(null)
  const fingerDragging = useRef(false)

  const progressHost = () => progressHostRef?.current ?? null

  const readX = () => {
    const vp = viewportRef.current
    const w = widthRef.current
    if (!vp || w <= 0) return progressRef.current
    return vp.scrollLeft / w
  }

  const logProgress = (why: string, force = false) => {
    const x = readX()
    const now = performance.now()
    const dt = now - lastProgLogTs.current
    const dx = Number.isFinite(lastProgLogX.current) ? Math.abs(x - lastProgLogX.current) : 99
    if (!force && dt < 32 && dx < 0.03) return
    lastProgLogTs.current = now
    lastProgLogX.current = x
    const intent = navIntent.current
    const intentStr = intent !== null ? ` →${fmtTarget(intent)}` : ''
    const mode = fingerDragging.current
      ? 'finger'
      : programmatic.current || intent !== null
        ? 'nav'
        : ptrPhase.current === 'pending'
          ? 'pending'
          : 'idle'
    pagerLog(`[pager] ${fmtX(x)} ${why} mode=${mode}${intentStr}`)
  }

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

  const clearNavRaf = () => {
    if (navRaf.current) {
      cancelAnimationFrame(navRaf.current)
      navRaf.current = 0
    }
  }

  const syncPill = (scrollLeft: number, dragging: boolean) => {
    const w = widthRef.current
    if (w <= 0) return
    const x = scrollLeft / w
    progressRef.current = x
    writeNavProgress(progressHost(), x, dragging)
  }

  /** T1: commit page as soon as target is chosen. */
  const commitAxis = (axis: number) => {
    const a = clamp(Math.round(axis), X_MIN, X_MAX)
    const next = axisToPage(a)
    if (next !== pageRef.current) {
      skipPropScroll.current = true
      pagerLog(`[pager] commit →${fmtTarget(a)} at ${fmtX(readX())}`)
      onPageChangeRef.current(next)
    }
  }

  const finishNavScroll = (reason: string, forSeq: number) => {
    if (forSeq !== actionSeq.current) {
      pagerLog(
        `[pager] finish-skip stale seq=${forSeq} now=${actionSeq.current} at ${fmtX(readX())}`,
      )
      return
    }
    const vp = viewportRef.current
    const w = widthRef.current
    const target = navIntent.current
    clearWatchdog()
    clearSettleTimer()
    clearNavRaf()
    programmatic.current = false
    if (target === null || !vp || w <= 0) {
      navIntent.current = null
      return
    }
    const exact = target * w
    const before = vp.scrollLeft / w
    try {
      vp.scrollTo({ left: exact, behavior: 'instant' as ScrollBehavior })
    } catch {
      vp.scrollLeft = exact
    }
    syncPill(exact, false)
    navIntent.current = null
    holdAxis.current = target
    interruptedEase.current = false
    dragOriginX.current = null
    pagerLog(
      `[pager] intent-done →${fmtTarget(target)} (${reason}) from ${fmtX(before)}`,
    )
    logProgress('landed', true)
  }

  const finishNavScrollRef = useRef(finishNavScroll)
  finishNavScrollRef.current = finishNavScroll

  /**
   * Smooth to content axis (1…4). T1 commits immediately.
   * Same-target in-flight: adopt (no restart).
   */
  const nativeScrollTo = (targetAxisSlot: number) => {
    const vp = viewportRef.current
    const w = widthRef.current
    if (!vp || w <= 0) return

    const target = clamp(Math.round(targetAxisSlot), X_MIN, X_MAX)
    const targetLeft = target * w
    const from = vp.scrollLeft
    const x = from / w

    if (Math.abs(from - targetLeft) < 1) {
      clearSettleTimer()
      clearWatchdog()
      clearNavRaf()
      navIntent.current = null
      programmatic.current = false
      holdAxis.current = target
      interruptedEase.current = false
      syncPill(targetLeft, false)
      commitAxis(target)
      pagerLog(`[pager] noop-on-slot ${fmtX(x)} already ${fmtTarget(target)}`)
      return
    }

    // P1: ease→C, tap C again → do not restart
    if (navIntent.current === target && programmatic.current) {
      pagerLog(`[pager] adopt-nav ${fmtX(x)} keep→${fmtTarget(target)}`)
      commitAxis(target)
      return
    }

    clearSettleTimer()
    clearWatchdog()
    clearNavRaf()

    actionSeq.current += 1
    const mySeq = actionSeq.current
    navActionSeq.current = mySeq
    holdAxis.current = null
    fingerDragging.current = false
    interruptedEase.current = false
    dragOriginX.current = null

    navIntent.current = target
    commitAxis(target)

    // Always ease (B10).
    programmatic.current = true
    pagerLog(
      `[pager] nav-smooth ${fmtX(x)}→${fmtTarget(target)} `
      + `Δ=${Math.abs(target - x).toFixed(2)} seq=${mySeq}`,
    )
    logProgress('nav-start', true)

    navRaf.current = requestAnimationFrame(() => {
      navRaf.current = 0
      if (navIntent.current !== target || navActionSeq.current !== mySeq) return
      const vp2 = viewportRef.current
      if (!vp2) return
      vp2.scrollTo({ left: targetLeft, behavior: 'smooth' })
    })

    watchdog.current = window.setTimeout(() => {
      watchdog.current = 0
      if (navIntent.current !== target || navActionSeq.current !== mySeq) return
      finishNavScrollRef.current(
        Math.abs(vp.scrollLeft - targetLeft) <= 3 ? 'watchdog-near' : 'watchdog-force',
        mySeq,
      )
    }, NAV.tapSmoothWatchdogMs)
  }

  const nativeScrollToRef = useRef(nativeScrollTo)
  nativeScrollToRef.current = nativeScrollTo

  /** Freeze in-flight smooth (B2); mark interrupted for B1 round on short-tap. */
  const freezeEase = (why: string) => {
    const vp = viewportRef.current
    if (!vp) return
    const was = navIntent.current
    if (was === null && !programmatic.current) return
    clearWatchdog()
    clearNavRaf()
    clearSettleTimer()
    cancelScrollAtCurrent(vp)
    syncPill(vp.scrollLeft, false)
    programmatic.current = false
    navIntent.current = null
    interruptedEase.current = true
    dragOriginX.current = null
    holdAxis.current = null
    pagerLog(`[pager] freeze ${why} at ${fmtX(readX())}`
      + (was !== null ? ` was→${fmtTarget(was)}` : ''))
  }

  /** After finger-up / short-tap: pick target, T1 commit, smooth. */
  const settleToPicked = (x: number) => {
    const target = pickSettleTarget(
      x,
      dragOriginX.current,
      interruptedEase.current,
    )
    interruptedEase.current = false
    dragOriginX.current = null
    pagerLog(`[pager] settle-pick ${fmtX(x)} →${fmtTarget(target)}`)
    nativeScrollToRef.current(target)
  }

  useLayoutEffect(() => {
    const vp = viewportRef.current
    if (!vp) return
    const measure = () => {
      const w = vp.clientWidth
      if (w <= 0) return
      const prevW = widthRef.current
      widthRef.current = w
      invalidateNavPillLayout()
      if (prevW <= 0) {
        // First layout — land on content slot (not blank 0).
        const i = holdAxis.current ?? navIntent.current ?? pageToAxis(pageRef.current)
        vp.scrollLeft = i * w
        holdAxis.current = i
      } else if (Math.abs(prevW - w) > 0.5) {
        const i = holdAxis.current ?? navIntent.current ?? pageToAxis(pageRef.current)
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
    const targetLeft = targetAxis * w
    if (
      Math.abs(vp.scrollLeft - targetLeft) < 1
      && navIntent.current === null
      && !programmatic.current
      && holdAxis.current === targetAxis
    ) {
      syncPill(targetLeft, false)
      return
    }
    pagerLog(`[pager] prop→scroll →${fmtTarget(targetAxis)} at ${fmtX(vp.scrollLeft / w)} seq=${navSeq}`)
    nativeScrollToRef.current(targetAxis)
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [targetAxis, navSeq])

  useEffect(() => {
    const vp = viewportRef.current
    if (!vp) return

    const beginDrag = () => {
      if (ptrPhase.current === 'dragging') return
      ptrPhase.current = 'dragging'
      fingerDragging.current = true
      clearSettleTimer()

      actionSeq.current += 1

      const x = widthRef.current > 0 ? vp.scrollLeft / widthRef.current : progressRef.current
      const wasNav = navIntent.current !== null || programmatic.current
      if (wasNav) {
        freezeEase('drag-takeover')
      }
      // Origin for B1 ±0.15 only if we started from settled content (not interrupt).
      if (!interruptedEase.current) {
        const hold = holdAxis.current
        dragOriginX.current = hold !== null ? hold : Math.round(x)
      } else {
        dragOriginX.current = null
      }
      holdAxis.current = null
      pagerLog(`[pager] drag-start ${fmtX(x)} seq=${actionSeq.current}`
        + ` origin=${dragOriginX.current ?? 'round'} interrupted=${interruptedEase.current ? 1 : 0}`)
      logProgress('drag-start', true)
    }

    const onScroll = () => {
      const w = widthRef.current
      const moving = fingerDragging.current
        || programmatic.current
        || navIntent.current !== null
      syncPill(vp.scrollLeft, moving)
      if (moving) logProgress('tick')

      if (fingerDragging.current || ptrPhase.current === 'pending') return

      if (navIntent.current !== null || programmatic.current) {
        const target = navIntent.current
        const mySeq = navActionSeq.current
        if (w > 0 && target !== null && Math.abs(vp.scrollLeft - target * w) <= 2) {
          finishNavScrollRef.current('near', mySeq)
        }
      }
    }

    const onScrollEnd = () => {
      logProgress('scrollend', true)
      if (fingerDragging.current || ptrPhase.current === 'pending') return
      if (navIntent.current !== null || programmatic.current) {
        finishNavScrollRef.current('scrollend', navActionSeq.current)
      }
    }

    const onPointerDown = (e: PointerEvent) => {
      if (ptrPhase.current !== 'none') return
      ptrPhase.current = 'pending'
      ptrStartX.current = e.clientX
      ptrStartY.current = e.clientY
      ptrId.current = e.pointerId
      fingerDragging.current = false

      // B2: ease in flight → freeze immediately
      if (navIntent.current !== null || programmatic.current) {
        freezeEase('pointerdown')
      }

      pagerLog(`[pager] pointer↓ pending ${fmtX(readX())}`)
      logProgress('pointer-down', true)
    }

    const onPointerMove = (e: PointerEvent) => {
      if (ptrPhase.current !== 'pending') return
      if (ptrId.current !== null && e.pointerId !== ptrId.current) return
      const adx = Math.abs(e.clientX - ptrStartX.current)
      const ady = Math.abs(e.clientY - ptrStartY.current)
      const axis = resolvePagerAxis(adx, ady)
      if (axis === 'none') return
      if (axis === 'v') {
        // Vertical wins — abandon pager gesture; if we froze ease, settle via B1.
        ptrPhase.current = 'none'
        ptrId.current = null
        pagerLog(`[pager] axis=v ${fmtX(readX())}`)
        if (interruptedEase.current) {
          settleToPicked(readX())
        }
        return
      }
      beginDrag()
    }

    const endPointer = (e: PointerEvent, reason: 'up' | 'cancel') => {
      if (ptrId.current !== null && e.pointerId !== ptrId.current) return
      const phase = ptrPhase.current
      ptrPhase.current = 'none'
      ptrId.current = null
      const x = widthRef.current > 0 ? vp.scrollLeft / widthRef.current : progressRef.current

      if (phase === 'pending') {
        // Short tap: B2-A if we interrupted ease → settle by B1; else ignore.
        fingerDragging.current = false
        pagerLog(`[pager] tap ${fmtX(x)} reason=${reason} interrupted=${interruptedEase.current ? 1 : 0}`)
        if (interruptedEase.current) {
          settleToPicked(x)
        }
        logProgress('tap-end', true)
        return
      }

      if (phase !== 'dragging') return

      fingerDragging.current = false
      pagerLog(`[pager] finger↑ ${fmtX(x)}`)
      logProgress('finger-up', true)
      settleToPicked(x)
    }

    const onPointerUp = (e: PointerEvent) => endPointer(e, 'up')
    const onPointerCancel = (e: PointerEvent) => endPointer(e, 'cancel')

    vp.addEventListener('scroll', onScroll, { passive: true })
    vp.addEventListener('scrollend', onScrollEnd as EventListener)
    vp.addEventListener('pointerdown', onPointerDown, { passive: true })
    vp.addEventListener('pointermove', onPointerMove, { passive: true })
    vp.addEventListener('pointerup', onPointerUp, { passive: true })
    vp.addEventListener('pointercancel', onPointerCancel, { passive: true })
    pagerLog(`[pager] ready slots=0..${SLOT_COUNT - 1} content=${X_MIN}..${X_MAX}`)

    if (widthRef.current > 0) {
      const ax = pageToAxis(pageRef.current)
      vp.scrollLeft = ax * widthRef.current
      syncPill(vp.scrollLeft, false)
      holdAxis.current = ax
    }

    return () => {
      vp.removeEventListener('scroll', onScroll)
      vp.removeEventListener('scrollend', onScrollEnd as EventListener)
      vp.removeEventListener('pointerdown', onPointerDown)
      vp.removeEventListener('pointermove', onPointerMove)
      vp.removeEventListener('pointerup', onPointerUp)
      vp.removeEventListener('pointercancel', onPointerCancel)
      clearSettleTimer()
      clearWatchdog()
      clearNavRaf()
      programmatic.current = false
      navIntent.current = null
    }
  }, [progressHostRef])

  return (
    <div
      ref={viewportRef}
      className="flex-1 min-h-0 overflow-x-auto overflow-y-hidden"
      data-page-pager
      style={{
        scrollSnapType: 'none',
        WebkitOverflowScrolling: 'touch',
        overscrollBehaviorX: 'contain',
        scrollbarWidth: 'none',
        msOverflowStyle: 'none',
        touchAction: 'pan-x pan-y',
      }}
    >
      <div className="flex h-full w-full">
        {/* Slot 0 — left bounce blank */}
        <div
          key="blank-l"
          className="h-full shrink-0"
          style={{ flex: '0 0 100%', width: '100%' }}
          aria-hidden
        />
        {panels.map((panel, i) => (
          <div
            key={PRIMARY_PAGES[i] ?? i}
            className="h-full shrink-0 flex flex-col min-h-0 overflow-hidden"
            style={{
              flex: '0 0 100%',
              width: '100%',
            }}
            aria-hidden={pageToAxis(page) !== i + X_MIN}
          >
            {panel}
          </div>
        ))}
        {/* Slot 5 — right bounce blank */}
        <div
          key="blank-r"
          className="h-full shrink-0"
          style={{ flex: '0 0 100%', width: '100%' }}
          aria-hidden
        />
      </div>
    </div>
  )
}
