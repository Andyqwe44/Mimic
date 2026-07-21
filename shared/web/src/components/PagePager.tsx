// Native H overflow for finger pan (compositor). No fling after lift:
// touchend → hard killMomentum (overflow-x toggle) → pin 2 rAF → B1 settle.
// Δ≤0.25 → instant pin (smooth loses to residual fling on Android WebView).
// Idle: if holdAxis drifts → re-pin. PAGER_DEBUG dumps x every animation frame while active.
// Android pointercancel early when browser takes pan — do NOT settle on cancel.
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

const PAGER_DEBUG = true
function pagerLog(msg: string) {
  if (PAGER_DEBUG) addLog(msg)
}

const BLANK_SLOTS = 2
const CONTENT_COUNT = PRIMARY_PAGES.length
const SLOT_COUNT = CONTENT_COUNT + BLANK_SLOTS
const X_MIN = 1
const X_MAX = CONTENT_COUNT
const SNAP_EPS = NAV.pagerSnapThreshold
const ON_SLOT_EPS = 0.05

type PillLayout = { padL: number; slotW: number; pitch: number; n: number }
type PtrPhase = 'none' | 'pending' | 'dragging'

let pillEl: HTMLElement | null = null
let pillHost: HTMLElement | null = null
let pillLayout: PillLayout | null = null
let pillDragging = false

export function invalidateNavPillLayout() {
  pillLayout = null
}

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
  pill.style.left = `${padL - pitch}px`
  pill.style.width = `${slotW}px`
  return { padL, slotW, pitch, n: SLOT_COUNT }
}

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

function pageToAxis(page: AppPage): number {
  return pageIndex(page) + X_MIN
}

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

function isOnContentSlot(x: number): boolean {
  if (x < X_MIN - ON_SLOT_EPS || x > X_MAX + ON_SLOT_EPS) return false
  const r = Math.round(x)
  if (r < X_MIN || r > X_MAX) return false
  return Math.abs(x - r) <= ON_SLOT_EPS
}

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
  if (Math.abs(originX - origin) > ON_SLOT_EPS) {
    return clamp(Math.round(x), X_MIN, X_MAX)
  }

  const delta = x - origin
  if (delta > SNAP_EPS) {
    return clamp(Math.max(origin + 1, Math.round(x)), X_MIN, X_MAX)
  }
  if (delta < -SNAP_EPS) {
    return clamp(Math.min(origin - 1, Math.round(x)), X_MIN, X_MAX)
  }
  return origin
}

function resolveDragOrigin(
  x: number,
  hold: number | null,
  interruptedEase: boolean,
): number | null {
  if (interruptedEase) return null
  if (hold !== null && Math.abs(x - hold) <= ON_SLOT_EPS) {
    return clamp(hold, X_MIN, X_MAX)
  }
  if (isOnContentSlot(x)) return clamp(Math.round(x), X_MIN, X_MAX)
  return null
}

/**
 * Hard-stop native fling. Plain scrollTo(instant) is NOT enough on Android WebView —
 * momentum keeps fighting scrollTo(smooth). Toggle overflow-x to drop the scroller's velocity.
 */
function killMomentum(vp: HTMLElement) {
  const left = vp.scrollLeft
  const prev = vp.style.overflowX
  vp.style.overflowX = 'hidden'
  vp.scrollLeft = left
  // Force layout so the hidden overflow takes effect before restoring.
  void vp.offsetWidth
  vp.style.overflowX = prev || 'auto'
  try {
    vp.scrollTo({ left, behavior: 'instant' as ScrollBehavior })
  } catch {
    vp.scrollLeft = left
  }
}

function cancelScrollAtCurrent(vp: HTMLElement) {
  killMomentum(vp)
}

export function PagePager({
  page,
  navSeq = 0,
  onPageChange,
  progressHostRef,
  children,
}: {
  page: AppPage
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
  const navIntent = useRef<number | null>(null)
  const programmatic = useRef(false)
  const holdAxis = useRef<number | null>(targetAxis)

  const dragOriginX = useRef<number | null>(null)
  const ptrOriginX = useRef<number | null>(null)
  const interruptedEase = useRef(false)

  const watchdog = useRef(0)
  const navRaf = useRef(0)
  const lastProgLogTs = useRef(0)
  const lastProgLogX = useRef(Number.NaN)

  const ptrPhase = useRef<PtrPhase>('none')
  const ptrStartX = useRef(0)
  const ptrStartY = useRef(0)
  const ptrId = useRef<number | null>(null)
  const fingerDragging = useRef(false)
  /**
   * True from pointerdown until touchend settles.
   * Survives pointercancel (browser took pan) so we still snap on real finger-up.
   */
  const touchArmed = useRef(false)
  /** True only after H-axis lock (real swipe, not short tap). */
  const didHDrag = useRef(false)
  /** rAF id for per-frame x dump while armed / settling. */
  const frameRaf = useRef(0)
  const frameSeq = useRef(0)
  /** After finger-up: pin scrollLeft for 2 frames before starting smooth (let kill stick). */
  const settleDelayRaf = useRef(0)

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
    // Per-frame dump uses force=true; event logs keep light throttle.
    const minDt = PAGER_DEBUG ? 48 : 32
    const minDx = PAGER_DEBUG ? 0.02 : 0.03
    if (!force && dt < minDt && dx < minDx) return
    lastProgLogTs.current = now
    lastProgLogX.current = x
    const intent = navIntent.current
    const intentStr = intent !== null ? ` intent→${fmtTarget(intent)}` : ''
    const mode = fingerDragging.current || (touchArmed.current && ptrPhase.current === 'dragging')
      ? 'finger'
      : touchArmed.current
        ? 'native-pan'
        : programmatic.current || intent !== null
          ? 'nav-smooth'
          : ptrPhase.current === 'pending'
            ? 'pending'
            : 'idle'
    pagerLog(`[pager] x=${x.toFixed(3)} ${fmtX(x)} | ${why} | mode=${mode}${intentStr}`)
  }

  const stopFramePump = () => {
    if (frameRaf.current) {
      cancelAnimationFrame(frameRaf.current)
      frameRaf.current = 0
    }
  }

  /** Every animation frame while touch/settle active — dense x trace for Android debug. */
  const startFramePump = (tag: string) => {
    stopFramePump()
    frameSeq.current = 0
    const tick = () => {
      frameRaf.current = 0
      const active = touchArmed.current
        || programmatic.current
        || navIntent.current !== null
        || fingerDragging.current
      if (!active) return
      frameSeq.current += 1
      logProgress(`frame#${frameSeq.current}(${tag})`, true)
      frameRaf.current = requestAnimationFrame(tick)
    }
    frameRaf.current = requestAnimationFrame(tick)
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

  const clearSettleDelay = () => {
    if (settleDelayRaf.current) {
      cancelAnimationFrame(settleDelayRaf.current)
      settleDelayRaf.current = 0
    }
  }

  /** Pin scrollLeft for `frames` rAFs after killMomentum so Android fling dies before smooth. */
  const pinThen = (vp: HTMLElement, left: number, frames: number, then: () => void) => {
    clearSettleDelay()
    let n = 0
    const step = () => {
      settleDelayRaf.current = 0
      killMomentum(vp)
      vp.scrollLeft = left
      syncPill(left, false)
      n += 1
      if (n < frames) {
        settleDelayRaf.current = requestAnimationFrame(step)
        return
      }
      then()
    }
    settleDelayRaf.current = requestAnimationFrame(step)
  }

  const syncPill = (scrollLeft: number, dragging: boolean) => {
    const w = widthRef.current
    if (w <= 0) return
    const x = scrollLeft / w
    progressRef.current = x
    writeNavProgress(progressHost(), x, dragging)
  }

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
    clearNavRaf()
    clearSettleDelay()
    programmatic.current = false
    stopFramePump()
    if (target === null || !vp || w <= 0) {
      navIntent.current = null
      return
    }
    const exact = target * w
    const before = vp.scrollLeft / w
    killMomentum(vp)
    vp.scrollLeft = exact
    syncPill(exact, false)
    navIntent.current = null
    holdAxis.current = target
    interruptedEase.current = false
    dragOriginX.current = null
    ptrOriginX.current = null
    pagerLog(
      `[pager] intent-done →${fmtTarget(target)} (${reason}) from ${fmtX(before)}`,
    )
    logProgress('landed', true)
  }

  const finishNavScrollRef = useRef(finishNavScroll)
  finishNavScrollRef.current = finishNavScroll

  const nativeScrollTo = (targetAxisSlot: number) => {
    const vp = viewportRef.current
    const w = widthRef.current
    if (!vp || w <= 0) return

    const target = clamp(Math.round(targetAxisSlot), X_MIN, X_MAX)
    const targetLeft = target * w
    const from = vp.scrollLeft
    const x = from / w

    if (Math.abs(from - targetLeft) < 1) {
      clearWatchdog()
      clearNavRaf()
      clearSettleDelay()
      navIntent.current = null
      programmatic.current = false
      stopFramePump()
      holdAxis.current = target
      interruptedEase.current = false
      syncPill(targetLeft, false)
      commitAxis(target)
      pagerLog(`[pager] noop-on-slot ${fmtX(x)} already ${fmtTarget(target)}`)
      return
    }

    if (navIntent.current === target && programmatic.current) {
      pagerLog(`[pager] adopt-nav ${fmtX(x)} keep→${fmtTarget(target)}`)
      commitAxis(target)
      return
    }

    clearWatchdog()
    clearNavRaf()
    clearSettleDelay()

    actionSeq.current += 1
    const mySeq = actionSeq.current
    navActionSeq.current = mySeq
    holdAxis.current = null
    fingerDragging.current = false
    interruptedEase.current = false
    dragOriginX.current = null
    ptrOriginX.current = null

    navIntent.current = target
    commitAxis(target)

    // Small residual (typical after ±0.15 stay): instant pin — smooth loses to fling.
    const delta = Math.abs(target - x)
    if (delta <= 0.25) {
      killMomentum(vp)
      vp.scrollLeft = targetLeft
      syncPill(targetLeft, false)
      programmatic.current = false
      navIntent.current = null
      holdAxis.current = target
      stopFramePump()
      pagerLog(
        `[pager] nav-instant 从 x=${x.toFixed(3)} →${fmtTarget(target)} `
        + `Δ=${delta.toFixed(2)} seq=${mySeq}`,
      )
      logProgress('landed-instant', true)
      return
    }

    programmatic.current = true
    startFramePump('nav')
    pagerLog(
      `[pager] nav-smooth 从 x=${x.toFixed(3)} →${fmtTarget(target)} `
      + `Δ=${delta.toFixed(2)} seq=${mySeq}`,
    )
    logProgress('nav-start', true)

    // Kill + pin 2 frames, then smooth — otherwise Android fling fights scrollTo(smooth).
    pinThen(vp, vp.scrollLeft, 2, () => {
      if (navIntent.current !== target || navActionSeq.current !== mySeq) return
      const vp2 = viewportRef.current
      if (!vp2) return
      killMomentum(vp2)
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

  const freezeEase = (why: string) => {
    const vp = viewportRef.current
    if (!vp) return
    const was = navIntent.current
    if (was === null && !programmatic.current) return
    clearWatchdog()
    clearNavRaf()
    clearSettleDelay()
    killMomentum(vp)
    cancelScrollAtCurrent(vp)
    syncPill(vp.scrollLeft, false)
    programmatic.current = false
    navIntent.current = null
    interruptedEase.current = true
    dragOriginX.current = null
    holdAxis.current = null
    pagerLog(
      `[pager] freeze(${why}) x=${readX().toFixed(3)}`
      + (was !== null ? ` was→${fmtTarget(was)}` : ''),
    )
  }

  const settleToPicked = (x: number, why: string) => {
    const origin = dragOriginX.current
    const interrupted = interruptedEase.current
    const target = pickSettleTarget(x, origin, interrupted)
    const rule = interrupted || origin === null
      ? 'round'
      : Math.abs(x - origin) <= SNAP_EPS
        ? `stay(±${SNAP_EPS})`
        : `step+round(±${SNAP_EPS})`
    interruptedEase.current = false
    dragOriginX.current = null
    ptrOriginX.current = null
    pagerLog(
      `[pager] 立刻吸附(${why}) x=${x.toFixed(3)} origin=${origin ?? 'null'} `
      + `interrupted=${interrupted ? 1 : 0} rule=${rule} →${fmtTarget(target)}`,
    )
    nativeScrollToRef.current(target)
  }

  const settleToPickedRef = useRef(settleToPicked)
  settleToPickedRef.current = settleToPicked

  /**
   * Real finger-up: hard-kill fling, pin 2 frames, then B1.
   * Called from touchend (authoritative on Android).
   */
  const onFingerReleased = (why: string) => {
    if (!touchArmed.current) return
    touchArmed.current = false
    const wasDrag = didHDrag.current
    fingerDragging.current = false
    didHDrag.current = false
    ptrPhase.current = 'none'
    ptrId.current = null

    const vp = viewportRef.current
    if (!vp) return

    killMomentum(vp)
    const freezeLeft = vp.scrollLeft
    const w = widthRef.current
    const x0 = w > 0 ? freezeLeft / w : progressRef.current
    syncPill(freezeLeft, false)
    startFramePump('finger-up')

    if (dragOriginX.current === null && ptrOriginX.current !== null) {
      dragOriginX.current = resolveDragOrigin(x0, ptrOriginX.current, interruptedEase.current)
    }

    pagerLog(
      `[pager] 手指松开(${why}) x=${x0.toFixed(3)} wasDrag=${wasDrag ? 1 : 0} `
      + `硬刹停→钉2帧→吸附`,
    )
    logProgress('finger-up', true)

    // Short tap on a slot: ignore. Still pin 2 frames so residual fling cannot drift.
    if (!wasDrag && isOnContentSlot(x0) && !interruptedEase.current) {
      const slot = Math.round(x0)
      holdAxis.current = slot
      pinThen(vp, slot * (w || 1), 2, () => {
        stopFramePump()
        pagerLog(`[pager] 短触在槽上忽略 x=${readX().toFixed(3)}`)
      })
      return
    }

    if (!wasDrag && !isOnContentSlot(x0)) {
      interruptedEase.current = true
      dragOriginX.current = null
    }

    pinThen(vp, freezeLeft, 2, () => {
      const vp2 = viewportRef.current
      const w2 = widthRef.current
      if (!vp2 || w2 <= 0) {
        stopFramePump()
        return
      }
      killMomentum(vp2)
      const x = vp2.scrollLeft / w2
      settleToPickedRef.current(x, why)
    })
  }

  const onFingerReleasedRef = useRef(onFingerReleased)
  onFingerReleasedRef.current = onFingerReleased

  useLayoutEffect(() => {
    const vp = viewportRef.current
    if (!vp) return
    const measure = () => {
      const w = vp.clientWidth
      if (w <= 0) return
      const prevW = widthRef.current
      widthRef.current = w
      invalidateNavPillLayout()
      const pin = () => {
        const i = holdAxis.current ?? navIntent.current ?? pageToAxis(pageRef.current)
        vp.scrollLeft = i * w
        holdAxis.current = i
        syncPill(vp.scrollLeft, false)
      }
      if (prevW <= 0 || Math.abs(prevW - w) > 0.5) pin()
      else syncPill(vp.scrollLeft, false)
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
    pagerLog(`[pager] prop→scroll →${fmtTarget(targetAxis)} 当前x=${(vp.scrollLeft / w).toFixed(3)} navSeq=${navSeq}`)
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
      didHDrag.current = true
      actionSeq.current += 1

      const x = widthRef.current > 0 ? vp.scrollLeft / widthRef.current : progressRef.current
      if (navIntent.current !== null || programmatic.current) {
        freezeEase('drag-takeover')
      }

      const origin = resolveDragOrigin(
        x,
        ptrOriginX.current ?? holdAxis.current,
        interruptedEase.current,
      )
      dragOriginX.current = origin
      holdAxis.current = null
      pagerLog(
        `[pager] 横滑开始(原生跟手) x=${x.toFixed(3)} seq=${actionSeq.current}`
        + ` origin=${origin ?? 'round'}`,
      )
      logProgress('drag-start', true)
    }

    const onScroll = () => {
      const moving = touchArmed.current
        || fingerDragging.current
        || programmatic.current
        || navIntent.current !== null
        || settleDelayRaf.current !== 0
      syncPill(vp.scrollLeft, moving)
      if (moving) logProgress(touchArmed.current && !programmatic.current ? 'pan' : 'tick')

      if (touchArmed.current || fingerDragging.current || ptrPhase.current === 'pending') return
      if (settleDelayRaf.current !== 0) return

      if (navIntent.current !== null || programmatic.current) {
        const target = navIntent.current
        const mySeq = navActionSeq.current
        const w = widthRef.current
        if (w > 0 && target !== null && Math.abs(vp.scrollLeft - target * w) <= 2) {
          finishNavScrollRef.current('near', mySeq)
        }
        return
      }

      // Idle drift guard: residual fling after "landed" (log: 2.000 → 2.138 with no pan).
      const hold = holdAxis.current
      const w = widthRef.current
      if (hold !== null && w > 0) {
        const x = vp.scrollLeft / w
        if (Math.abs(x - hold) > ON_SLOT_EPS) {
          const pinLeft = hold * w
          pagerLog(
            `[pager] idle-drift 纠偏 x=${x.toFixed(3)} →${fmtTarget(hold)}`,
          )
          killMomentum(vp)
          vp.scrollLeft = pinLeft
          syncPill(pinLeft, false)
          logProgress('idle-repin', true)
        }
      }
    }

    const onScrollEnd = () => {
      logProgress('scrollend', true)
      if (touchArmed.current || fingerDragging.current || ptrPhase.current === 'pending') return
      if (navIntent.current !== null || programmatic.current) {
        finishNavScrollRef.current('scrollend', navActionSeq.current)
      }
    }

    const onPointerDown = (e: PointerEvent) => {
      if (ptrPhase.current !== 'none' && touchArmed.current) return
      ptrPhase.current = 'pending'
      ptrStartX.current = e.clientX
      ptrStartY.current = e.clientY
      ptrId.current = e.pointerId
      fingerDragging.current = false
      touchArmed.current = true
      didHDrag.current = false

      const easing = navIntent.current !== null || programmatic.current
      const x0 = readX()
      // Catch silent idle fling that never delivered scroll events (seen in Android logs).
      const hold0 = holdAxis.current
      if (
        hold0 !== null
        && !easing
        && Math.abs(x0 - hold0) > ON_SLOT_EPS
      ) {
        const w = widthRef.current
        if (w > 0) {
          pagerLog(
            `[pager] idle-drift(on-press) x=${x0.toFixed(3)} →${fmtTarget(hold0)}`,
          )
          killMomentum(vp)
          vp.scrollLeft = hold0 * w
          syncPill(vp.scrollLeft, false)
        }
      }
      const x1 = readX()
      ptrOriginX.current = resolveDragOrigin(x1, holdAxis.current, false)
      dragOriginX.current = ptrOriginX.current

      pagerLog(
        `[pager] 按下 content cx=${e.clientX.toFixed(0)} cy=${e.clientY.toFixed(0)} `
        + `x=${x1.toFixed(3)} easing=${easing ? 1 : 0} origin=${ptrOriginX.current ?? 'round'}`,
      )

      if (easing) {
        freezeEase('pointerdown')
        ptrOriginX.current = null
        dragOriginX.current = null
      }
      startFramePump('touch')
      logProgress('pointer-down', true)
    }

    const onPointerMove = (e: PointerEvent) => {
      if (!touchArmed.current) return
      if (ptrId.current !== null && e.pointerId !== ptrId.current) return
      if (ptrPhase.current !== 'pending') return

      const dx = e.clientX - ptrStartX.current
      const dy = e.clientY - ptrStartY.current
      const axis = resolvePagerAxis(Math.abs(dx), Math.abs(dy))
      if (axis === 'none') return
      if (axis === 'v') {
        // Vertical: abandon pager arm — let page scroll; clear arm so touchend won't snap.
        ptrPhase.current = 'none'
        touchArmed.current = false
        ptrId.current = null
        pagerLog(`[pager] 轴锁定=竖滑 放弃横滑 x=${readX().toFixed(3)}`)
        stopFramePump()
        const x = readX()
        if (interruptedEase.current || !isOnContentSlot(x)) {
          killMomentum(vp)
          dragOriginX.current = null
          settleToPickedRef.current(x, 'axis-v')
        }
        return
      }
      pagerLog(`[pager] 轴锁定=横滑(原生) Δx=${dx.toFixed(0)} Δy=${dy.toFixed(0)} x=${readX().toFixed(3)}`)
      beginDrag()
    }

    /**
     * pointercancel = browser stole the gesture for native pan.
     * Keep touchArmed; wait for touchend to settle at real release position.
     */
    const onPointerCancel = (e: PointerEvent) => {
      if (ptrId.current !== null && e.pointerId !== ptrId.current) return
      if (!touchArmed.current) return
      const x = readX()
      pagerLog(
        `[pager] pointercancel(浏览器接管原生横滑) x=${x.toFixed(3)} `
        + `phase=${ptrPhase.current} →等 touchend 再吸附`,
      )
      // Stay armed; phase can stay dragging or become none — scroll still updates pill.
      ptrPhase.current = 'dragging'
      fingerDragging.current = true
      ptrId.current = null
    }

    const onPointerUp = (e: PointerEvent) => {
      if (ptrId.current !== null && e.pointerId !== ptrId.current) return
      if (!touchArmed.current) return
      // Prefer touchend on mobile (fires after cancel). pointerup OK on desktop.
      // If we already got cancel, skip pointerup — wait for touchend.
      if (ptrPhase.current === 'dragging' && ptrId.current === null) return
      onFingerReleasedRef.current(
        ptrPhase.current === 'pending' ? 'pointerup-tap' : 'pointerup',
      )
    }

    const onTouchEnd = (e: TouchEvent) => {
      // Only when no remaining touches — real finger lift.
      if (e.touches.length > 0) return
      if (!touchArmed.current) return
      onFingerReleasedRef.current('touchend')
    }

    const onTouchCancel = () => {
      if (!touchArmed.current) return
      onFingerReleasedRef.current('touchcancel')
    }

    vp.addEventListener('scroll', onScroll, { passive: true })
    vp.addEventListener('scrollend', onScrollEnd as EventListener)
    vp.addEventListener('pointerdown', onPointerDown, { passive: true })
    vp.addEventListener('pointermove', onPointerMove, { passive: true })
    vp.addEventListener('pointerup', onPointerUp, { passive: true })
    vp.addEventListener('pointercancel', onPointerCancel, { passive: true })
    // touchend survives pointercancel — this is the real "finger left the screen".
    vp.addEventListener('touchend', onTouchEnd, { passive: true })
    vp.addEventListener('touchcancel', onTouchCancel, { passive: true })

    pagerLog(
      `[pager] ready 轴0..${SLOT_COUNT - 1} 内容${X_MIN}..${X_MAX} `
      + `当前x=${pageToAxis(pageRef.current)} (${pageRef.current}) `
      + `mode=native-H+hardKill+frameX+idleRepin`,
    )

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
      vp.removeEventListener('touchend', onTouchEnd)
      vp.removeEventListener('touchcancel', onTouchCancel)
      clearWatchdog()
      clearNavRaf()
      clearSettleDelay()
      if (frameRaf.current) {
        cancelAnimationFrame(frameRaf.current)
        frameRaf.current = 0
      }
      programmatic.current = false
      navIntent.current = null
      touchArmed.current = false
    }
  }, [progressHostRef])

  return (
    <div
      ref={viewportRef}
      className="flex-1 min-h-0 overflow-x-auto overflow-y-hidden"
      data-page-pager
      style={{
        scrollSnapType: 'none',
        WebkitOverflowScrolling: 'auto', // prefer less momentum than 'touch' where honored
        overscrollBehaviorX: 'contain',
        scrollbarWidth: 'none',
        msOverflowStyle: 'none',
        touchAction: 'pan-x pan-y',
      }}
    >
      <div className="flex h-full w-full">
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
            style={{ flex: '0 0 100%', width: '100%' }}
            aria-hidden={pageToAxis(page) !== i + X_MIN}
          >
            {panel}
          </div>
        ))}
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
