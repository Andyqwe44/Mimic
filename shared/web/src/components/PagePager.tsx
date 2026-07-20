// Native overflow-x + scroll-snap (finger) · nav tap = scrollTo({ behavior:'smooth' }).
// Effective action wins: nav tap always; finger only after slop + H-lock (tap ignored).
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

/** 1-based axis: 1=Monitor 2=Peers 3=Log 4=Settings (matches bottom-nav left→right). */
function axisX(frac0: number): number {
  return frac0 + 1
}

/** Human label for fractional 0-based page index, e.g. "3.42(Log→Settings)". */
function fmtX(frac0: number): string {
  const x = axisX(frac0)
  const n = PRIMARY_PAGES.length
  if (n <= 0) return `x=${x.toFixed(2)}`
  const lo = Math.max(0, Math.min(n - 1, Math.floor(frac0 + 1e-6)))
  const hi = Math.max(0, Math.min(n - 1, Math.ceil(frac0 - 1e-6)))
  if (lo === hi || Math.abs(frac0 - lo) < 0.02) {
    return `x=${x.toFixed(2)}(${PRIMARY_PAGES[lo]})`
  }
  return `x=${x.toFixed(2)}(${PRIMARY_PAGES[lo]}→${PRIMARY_PAGES[hi]})`
}

function fmtTarget(idx0: number): string {
  const name = PRIMARY_PAGES[idx0] ?? `?${idx0}`
  return `${axisX(idx0).toFixed(0)}(${name})`
}

function axisLegend(): string {
  return PRIMARY_PAGES.map((p, i) => `${i + 1}=${p}`).join(' ')
}

/** Turn off snap so a newer smooth / finger gesture is not fought by compositor snap. */
function disarmSnap(vp: HTMLElement) {
  vp.style.scrollSnapType = 'none'
}

function enableFingerSnap(vp: HTMLElement) {
  vp.style.scrollSnapType = 'x mandatory'
}

/** Cancel in-flight smooth at current offset (finger grab). No overflow lock. Snap stays off. */
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
  /** True only while pointer phase === dragging (past slop + H). */
  const fingerDragging = useRef(false)

  /**
   * Monotonic user-action id. Nav tap and drag-takeover bump this.
   * Settle may commit only when settleArmSeq === actionSeq (armed by that drag).
   */
  const actionSeq = useRef(0)
  const settleArmSeq = useRef(-1)
  const navActionSeq = useRef(-1)
  const navIntent = useRef<number | null>(null)
  const programmatic = useRef(false)
  const holdIdx = useRef<number | null>(null)
  /** After one hold-correct for current holdIdx — block spam. */
  const holdCorrected = useRef(false)

  const settleTimer = useRef(0)
  const watchdog = useRef(0)
  const navRaf = useRef(0)
  const lastScrollTs = useRef(0)
  const lastProgLogTs = useRef(0)
  const lastProgLogX = useRef(Number.NaN)

  /** Pointer gesture: none → pending → dragging (only H past slop). */
  const ptrPhase = useRef<PtrPhase>('none')
  const ptrStartX = useRef(0)
  const ptrStartY = useRef(0)
  const ptrId = useRef<number | null>(null)

  const progressHost = () => progressHostRef?.current ?? null

  const readFrac = () => {
    const vp = viewportRef.current
    const w = widthRef.current
    if (!vp || w <= 0) return progressRef.current
    return vp.scrollLeft / w
  }

  const settleArmed = () => settleArmSeq.current === actionSeq.current && settleArmSeq.current >= 0

  const logProgress = (why: string, force = false) => {
    const frac = readFrac()
    const x = axisX(frac)
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
        : settleArmed()
          ? 'snap'
          : holdIdx.current !== null
            ? 'hold'
            : ptrPhase.current === 'pending'
              ? 'pending'
              : 'idle'
    addLog(`[pager] ${fmtX(frac)} ${why} mode=${mode}${intentStr}`)
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
    const p = scrollLeft / w
    progressRef.current = p
    writeNavProgress(progressHost(), p, dragging)
  }

  const commitIndex = (next: number) => {
    const i = clampIndex(next, pageCount)
    if (i !== indexRef.current) {
      skipPropScroll.current = true
      const from = indexRef.current
      addLog(
        `[pager] commit ${fmtTarget(from)}→${fmtTarget(i)} at ${fmtX(readFrac())}`,
      )
      onPageChangeRef.current(PRIMARY_PAGES[i])
    }
  }

  /**
   * End nav smooth. Always pin to exact slot. Snap stays OFF.
   */
  const finishNavScroll = (reason: string, forSeq: number) => {
    if (forSeq !== actionSeq.current) {
      addLog(
        `[pager] finish-skip stale seq=${forSeq} now=${actionSeq.current} at ${fmtX(readFrac())}`,
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
    const dist = Math.abs(vp.scrollLeft - exact)
    disarmSnap(vp)
    try {
      vp.scrollTo({ left: exact, behavior: 'instant' as ScrollBehavior })
    } catch {
      vp.scrollLeft = exact
    }
    syncPill(exact, false)
    navIntent.current = null
    holdIdx.current = target
    holdCorrected.current = false
    settleArmSeq.current = -1
    addLog(
      `[pager] intent-done →${fmtTarget(target)} (${reason}) `
      + `from ${fmtX(before)} distPx=${dist.toFixed(1)}`,
    )
    logProgress('landed', true)
  }

  const finishNavScrollRef = useRef(finishNavScroll)
  finishNavScrollRef.current = finishNavScroll

  /**
   * Nav tap / prop change. Same-target in-flight adopted.
   * disarmSnap → rAF → scrollTo(smooth) to avoid start-reverse hitch.
   */
  const nativeScrollTo = (targetIdx: number) => {
    const vp = viewportRef.current
    const w = widthRef.current
    if (!vp || w <= 0) return

    const targetLeft = targetIdx * w
    const from = vp.scrollLeft
    const frac = from / w

    if (Math.abs(from - targetLeft) < 1) {
      clearSettleTimer()
      clearWatchdog()
      clearNavRaf()
      settleArmSeq.current = -1
      navIntent.current = null
      programmatic.current = false
      holdIdx.current = targetIdx
      holdCorrected.current = false
      disarmSnap(vp)
      syncPill(targetLeft, false)
      addLog(`[pager] noop-on-slot ${fmtX(frac)} already ${fmtTarget(targetIdx)}`)
      return
    }

    if (navIntent.current === targetIdx && programmatic.current) {
      addLog(`[pager] adopt-nav ${fmtX(frac)} keep→${fmtTarget(targetIdx)}`)
      return
    }

    const recentlyScrolling = performance.now() - lastScrollTs.current < 120
    if (
      Math.abs(frac - targetIdx) <= 0.5
      && (settleArmed() || recentlyScrolling)
      && ptrPhase.current !== 'dragging'
    ) {
      clearWatchdog()
      navIntent.current = null
      programmatic.current = false
      holdIdx.current = null
      addLog(
        `[pager] adopt-snap ${fmtX(frac)} keep→${fmtTarget(targetIdx)}`
        + ` armed=${settleArmed() ? 1 : 0} recent=${recentlyScrolling ? 1 : 0}`,
      )
      return
    }

    clearSettleTimer()
    clearWatchdog()
    clearNavRaf()

    actionSeq.current += 1
    const mySeq = actionSeq.current
    navActionSeq.current = mySeq
    settleArmSeq.current = -1
    holdIdx.current = null
    holdCorrected.current = false
    fingerDragging.current = false
    // Don't clear pending pointer — tap during nav stays pending (P2)

    disarmSnap(vp)
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
      `[pager] nav-smooth ${fmtX(frac)}→${fmtTarget(targetIdx)} `
      + `Δ=${Math.abs(axisX(targetIdx) - axisX(frac)).toFixed(2)} seq=${mySeq}`
      + ` armed=0 recent=${recentlyScrolling ? 1 : 0}`,
    )
    logProgress('nav-start', true)

    // One frame after disarm so residual snap momentum dies before smooth starts.
    navRaf.current = requestAnimationFrame(() => {
      navRaf.current = 0
      if (navIntent.current !== targetIdx || navActionSeq.current !== mySeq) return
      const vp2 = viewportRef.current
      if (!vp2) return
      disarmSnap(vp2)
      vp2.scrollTo({ left: targetLeft, behavior: 'smooth' })
    })

    watchdog.current = window.setTimeout(() => {
      watchdog.current = 0
      if (navIntent.current !== targetIdx || navActionSeq.current !== mySeq) return
      const dist = Math.abs(vp.scrollLeft - targetLeft)
      if (dist <= 3) {
        finishNavScrollRef.current('watchdog-near', mySeq)
      } else {
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
      holdCorrected.current = false
      return
    }
    addLog(`[pager] prop→scroll →${fmtTarget(index)} at ${fmtX(vp.scrollLeft / w)} seq=${navSeq}`)
    nativeScrollToRef.current(index)
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [index, navSeq])

  useEffect(() => {
    const vp = viewportRef.current
    if (!vp) return

    const settleFromScroll = () => {
      if (!settleArmed()) return
      if (navIntent.current !== null || programmatic.current || fingerDragging.current) return

      const w = widthRef.current
      if (w <= 0) return
      const frac = vp.scrollLeft / w
      const nearest = clampIndex(Math.round(frac), pageCount)
      const exact = nearest * w
      addLog(`[pager] settle ${fmtX(frac)} →${fmtTarget(nearest)}`)
      disarmSnap(vp)
      if (Math.abs(vp.scrollLeft - exact) > 1) {
        try {
          vp.scrollTo({ left: exact, behavior: 'instant' as ScrollBehavior })
        } catch {
          vp.scrollLeft = exact
        }
      }
      syncPill(exact, false)
      holdIdx.current = nearest
      holdCorrected.current = false
      settleArmSeq.current = -1
      commitIndex(nearest)
    }

    const beginDragTakeover = (clientX: number, clientY: number) => {
      if (ptrPhase.current === 'dragging') return
      ptrPhase.current = 'dragging'
      fingerDragging.current = true
      holdIdx.current = null
      holdCorrected.current = false
      clearSettleTimer()

      actionSeq.current += 1
      settleArmSeq.current = actionSeq.current

      const frac = widthRef.current > 0 ? vp.scrollLeft / widthRef.current : progressRef.current
      const wasNav = navIntent.current !== null || programmatic.current
      if (wasNav) {
        clearWatchdog()
        clearNavRaf()
        const was = navIntent.current
        programmatic.current = false
        navIntent.current = null
        // Freeze at current x — snap stays OFF (no nearest jump).
        cancelScrollAtCurrent(vp)
        addLog(
          `[pager] drag-takeover ${fmtX(frac)}`
          + (was !== null ? ` was→${fmtTarget(was)}` : '')
          + ` seq=${actionSeq.current}`,
        )
      } else {
        disarmSnap(vp)
        addLog(`[pager] slop-pass ${fmtX(frac)} seq=${actionSeq.current}`)
      }
      void clientX
      void clientY
      logProgress('drag-start', true)
    }

    const onScroll = () => {
      lastScrollTs.current = performance.now()
      const w = widthRef.current
      const moving = fingerDragging.current
        || settleArmed()
        || programmatic.current
        || navIntent.current !== null
      syncPill(vp.scrollLeft, moving)

      if (moving) {
        logProgress('tick')
      }

      // One-shot hold pin — never re-enable snap (avoids hold-correct spam).
      if (
        !fingerDragging.current
        && ptrPhase.current === 'none'
        && holdIdx.current !== null
        && !holdCorrected.current
        && navIntent.current === null
        && !programmatic.current
        && w > 0
      ) {
        const exact = holdIdx.current * w
        if (Math.abs(vp.scrollLeft - exact) > 2) {
          const held = holdIdx.current
          holdCorrected.current = true
          disarmSnap(vp)
          try {
            vp.scrollTo({ left: exact, behavior: 'instant' as ScrollBehavior })
          } catch {
            vp.scrollLeft = exact
          }
          syncPill(exact, false)
          addLog(`[pager] hold-correct →${fmtTarget(held)} at ${fmtX(exact / w)}`)
          return
        }
      }

      if (fingerDragging.current || ptrPhase.current === 'pending') return

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
      logProgress('scrollend', true)
      if (fingerDragging.current || ptrPhase.current === 'pending') return
      if (navIntent.current !== null || programmatic.current) {
        finishNavScrollRef.current('scrollend', navActionSeq.current)
        return
      }
      if (!settleArmed()) return
      clearSettleTimer()
      settleFromScroll()
    }

    const onPointerDown = (e: PointerEvent) => {
      if (ptrPhase.current !== 'none') return
      ptrPhase.current = 'pending'
      ptrStartX.current = e.clientX
      ptrStartY.current = e.clientY
      ptrId.current = e.pointerId
      fingerDragging.current = false
      // P2: do NOT cancel nav, do NOT enable snap, do NOT arm settle.
      const frac = widthRef.current > 0 ? vp.scrollLeft / widthRef.current : progressRef.current
      addLog(
        `[pager] pointer↓ pending ${fmtX(frac)}`
        + (navIntent.current !== null ? ` nav→${fmtTarget(navIntent.current)}` : ''),
      )
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
        // Vertical wins — abandon pager gesture; leave nav alone.
        ptrPhase.current = 'none'
        ptrId.current = null
        addLog(`[pager] tap-ignore axis=v ${fmtX(readFrac())}`)
        return
      }
      // H past slop → real drag
      beginDragTakeover(e.clientX, e.clientY)
    }

    const endPointer = (e: PointerEvent, reason: 'up' | 'cancel') => {
      if (ptrId.current !== null && e.pointerId !== ptrId.current) return
      const phase = ptrPhase.current
      ptrPhase.current = 'none'
      ptrId.current = null
      const frac = widthRef.current > 0 ? vp.scrollLeft / widthRef.current : progressRef.current

      if (phase === 'pending') {
        // Tap / short touch — ignore. Resume nav smooth if browser stalled it.
        addLog(`[pager] tap-ignore ${fmtX(frac)} reason=${reason}`)
        fingerDragging.current = false
        if (navIntent.current !== null) {
          const target = navIntent.current
          const w = widthRef.current
          if (w > 0) {
            programmatic.current = true
            disarmSnap(vp)
            vp.scrollTo({ left: target * w, behavior: 'smooth' })
            addLog(`[pager] nav-resume →${fmtTarget(target)} at ${fmtX(frac)}`)
          }
        }
        logProgress('tap-ignore', true)
        return
      }

      if (phase !== 'dragging') return

      fingerDragging.current = false
      addLog(`[pager] finger↑ ${fmtX(frac)} armed=${settleArmed() ? 1 : 0}`)
      logProgress('finger-up', true)

      if (!settleArmed()) return
      // Enable snap only now so release can settle — then settleFromScroll disarms.
      enableFingerSnap(vp)
      clearSettleTimer()
      settleTimer.current = window.setTimeout(settleFromScroll, 120)
    }

    const onPointerUp = (e: PointerEvent) => endPointer(e, 'up')
    const onPointerCancel = (e: PointerEvent) => endPointer(e, 'cancel')

    vp.addEventListener('scroll', onScroll, { passive: true })
    vp.addEventListener('scrollend', onScrollEnd as EventListener)
    vp.addEventListener('pointerdown', onPointerDown, { passive: true })
    vp.addEventListener('pointermove', onPointerMove, { passive: true })
    vp.addEventListener('pointerup', onPointerUp, { passive: true })
    vp.addEventListener('pointercancel', onPointerCancel, { passive: true })
    addLog(`[pager] ready axis ${axisLegend()}`)
    addLog(`[pager] ready at ${fmtX(indexRef.current)} pages=${pageCount}`)
    if (widthRef.current > 0) {
      vp.scrollLeft = indexRef.current * widthRef.current
      syncPill(vp.scrollLeft, false)
      holdIdx.current = indexRef.current
      holdCorrected.current = false
      disarmSnap(vp)
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
  }, [pageCount, progressHostRef])

  return (
    <div
      ref={viewportRef}
      className="flex-1 min-h-0 overflow-x-auto overflow-y-hidden"
      data-page-pager
      style={{
        // Snap only enabled briefly on finger↑ for settle; default off (nav/hold).
        scrollSnapType: 'none',
        WebkitOverflowScrolling: 'touch',
        overscrollBehaviorX: 'contain',
        scrollbarWidth: 'none',
        msOverflowStyle: 'none',
        touchAction: 'pan-x pan-y',
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
