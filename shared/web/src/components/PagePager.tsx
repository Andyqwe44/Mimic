// MAA-Meow / ViewPager single-owner: one fractional offset.
// Drag = translate3d finger-follow; settleTo(T) = rAF + MAA cubic-bezier (last wins).
import {
  Children,
  useEffect,
  useLayoutEffect,
  useRef,
  type ReactNode,
  type RefObject,
} from 'react'
import { NAV, maaSettleMs, resolvePagerAxis, rubberBandPage } from '../lib/design'
import { PRIMARY_PAGES, pageIndex, type AppPage } from '../lib/pages'
import { addLog } from '../lib/bridge'

/** Set true only when debugging PagePager; keep false so peer video logs stay readable. */
const PAGER_DEBUG = false
function pagerLog(msg: string) {
  if (PAGER_DEBUG) addLog(msg)
}

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

function axisX(frac0: number): number {
  return frac0 + 1
}

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

function bezSample(s: number, a: number, b: number): number {
  const u = 1 - s
  return 3 * u * u * s * a + 3 * u * s * s * b + s * s * s
}

function bezDeriv(s: number, a: number, b: number): number {
  const u = 1 - s
  return 3 * u * u * a + 6 * u * s * (b - a) + 3 * s * s * (1 - b)
}

/** MAA springEasing cubic-bezier(x1,y1,x2,y2). */
function bezierEase(t: number, ease: readonly [number, number, number, number]): number {
  if (t <= 0) return 0
  if (t >= 1) return 1
  const [x1, y1, x2, y2] = ease
  let s = t
  for (let i = 0; i < 8; i++) {
    const x = bezSample(s, x1, x2) - t
    const dx = bezDeriv(s, x1, x2)
    if (Math.abs(dx) < 1e-6) break
    s = Math.max(0, Math.min(1, s - x / dx))
  }
  return bezSample(s, y1, y2)
}

function resolveReleaseTarget(
  frac: number,
  vel: number,
  pageCount: number,
  startFrac: number,
): number {
  const nearest = clampIndex(Math.round(frac), pageCount)
  const moved = frac - startFrac
  const absVel = Math.abs(vel)
  const flingOk = absVel >= NAV.pagerFlingPagesPerMs
    && Math.abs(moved) >= NAV.pagerFlingMinDelta

  if (flingOk) {
    const dir = vel > 0 ? 1 : -1
    const base = dir > 0 ? Math.floor(frac + 1e-6) : Math.ceil(frac - 1e-6)
    return clampIndex(base + dir, pageCount)
  }

  if (Math.abs(moved) >= NAV.pagerSnapThreshold) {
    return clampIndex(moved > 0 ? Math.ceil(frac - 1e-6) : Math.floor(frac + 1e-6), pageCount)
  }

  return nearest
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

  const reduceMotion = useRef(prefersReducedMotion())
  const skipPropScroll = useRef(false)

  const animTarget = useRef<number | null>(null)
  const animSeq = useRef(0)
  const holdIdx = useRef<number | null>(null)
  const settleRaf = useRef(0)

  const lastProgLogTs = useRef(0)
  const lastProgLogX = useRef(Number.NaN)

  const ptrPhase = useRef<PtrPhase>('none')
  const ptrStartX = useRef(0)
  const ptrStartY = useRef(0)
  const ptrId = useRef<number | null>(null)
  const dragOriginFrac = useRef(0)
  const dragStartClientX = useRef(0)
  const velSamples = useRef<{ t: number; frac: number }[]>([])

  const progressHost = () => progressHostRef?.current ?? null

  const clearSettleRaf = () => {
    if (settleRaf.current) {
      cancelAnimationFrame(settleRaf.current)
      settleRaf.current = 0
    }
  }

  const applyFrac = (frac: number, dragging: boolean) => {
    const track = trackRef.current
    const w = widthRef.current
    if (!track || w <= 0) return
    progressRef.current = frac
    track.style.transform = `translate3d(${-frac * w}px,0,0)`
    writeNavProgress(progressHost(), frac, dragging)
  }

  const logProgress = (why: string, force = false) => {
    const frac = progressRef.current
    const x = axisX(frac)
    const now = performance.now()
    const dt = now - lastProgLogTs.current
    const dx = Number.isFinite(lastProgLogX.current) ? Math.abs(x - lastProgLogX.current) : 99
    if (!force && dt < 32 && dx < 0.03) return
    lastProgLogTs.current = now
    lastProgLogX.current = x
    const target = animTarget.current
    const intentStr = target !== null ? ` →${fmtTarget(target)}` : ''
    const mode = ptrPhase.current === 'dragging'
      ? 'finger'
      : animTarget.current !== null
        ? 'settle'
        : holdIdx.current !== null
          ? 'hold'
          : ptrPhase.current === 'pending'
            ? 'pending'
            : 'idle'
    pagerLog(`[pager] ${fmtX(frac)} ${why} mode=${mode}${intentStr}`)
  }

  const commitIndex = (next: number) => {
    const i = clampIndex(next, pageCount)
    if (i !== indexRef.current) {
      skipPropScroll.current = true
      const from = indexRef.current
      pagerLog(
        `[pager] commit ${fmtTarget(from)}→${fmtTarget(i)} at ${fmtX(progressRef.current)}`,
      )
      onPageChangeRef.current(PRIMARY_PAGES[i])
    }
  }

  const finishSettle = (reason: string, forSeq: number) => {
    if (forSeq !== animSeq.current) {
      pagerLog(
        `[pager] finish-skip stale seq=${forSeq} now=${animSeq.current} `
        + `at ${fmtX(progressRef.current)}`,
      )
      return
    }
    const target = animTarget.current
    clearSettleRaf()
    if (target === null) return
    const before = progressRef.current
    applyFrac(target, false)
    animTarget.current = null
    holdIdx.current = target
    pagerLog(
      `[pager] settle-done →${fmtTarget(target)} (${reason}) from ${fmtX(before)}`,
    )
    logProgress('landed', true)
    commitIndex(target)
  }

  const finishSettleRef = useRef(finishSettle)
  finishSettleRef.current = finishSettle

  /**
   * Sole settler (MAA animateScrollToPage). Last call wins via animSeq.
   */
  const settleTo = (targetIdx: number, why: string) => {
    const w = widthRef.current
    if (w <= 0) return

    const target = clampIndex(targetIdx, pageCount)
    const from = progressRef.current

    // Same target already settling — adopt (MAA: index == targetPage return).
    if (animTarget.current === target && Math.abs(from - target) > 0.002) {
      pagerLog(`[pager] adopt-settle ${fmtX(from)} keep→${fmtTarget(target)} why=${why}`)
      return
    }

    if (Math.abs(from - target) < 0.002 && animTarget.current === null) {
      holdIdx.current = target
      applyFrac(target, false)
      commitIndex(target)
      pagerLog(`[pager] noop-on-slot ${fmtX(from)} already ${fmtTarget(target)} why=${why}`)
      return
    }

    clearSettleRaf()
    holdIdx.current = null
    animSeq.current += 1
    const mySeq = animSeq.current
    animTarget.current = target

    const distance = Math.abs(target - from)
    if (reduceMotion.current || distance < 0.002) {
      applyFrac(target, false)
      finishSettleRef.current('instant', mySeq)
      return
    }

    const dur = maaSettleMs(distance)
    pagerLog(
      `[pager] settleTo ${fmtX(from)}→${fmtTarget(target)} `
      + `Δ=${distance.toFixed(2)} ${dur}ms seq=${mySeq} why=${why}`,
    )
    logProgress('settle-start', true)

    const t0 = performance.now()
    const ease = NAV.pageAnimEase

    const tick = (now: number) => {
      if (animSeq.current !== mySeq || animTarget.current !== target) return
      const u = Math.min(1, (now - t0) / dur)
      const e = bezierEase(u, ease)
      const frac = from + (target - from) * e
      applyFrac(frac, false)
      logProgress('tick')
      if (u < 1) {
        settleRaf.current = requestAnimationFrame(tick)
      } else {
        settleRaf.current = 0
        finishSettleRef.current('raf-end', mySeq)
      }
    }
    settleRaf.current = requestAnimationFrame(tick)
  }

  const settleToRef = useRef(settleTo)
  settleToRef.current = settleTo

  const freezeSettle = () => {
    clearSettleRaf()
    const was = animTarget.current
    animTarget.current = null
    applyFrac(progressRef.current, true)
    return was
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
    const track = trackRef.current
    if (!vp || !track) return
    const measure = () => {
      const w = vp.clientWidth
      if (w <= 0) return
      const prevW = widthRef.current
      widthRef.current = w
      invalidateNavPillLayout()
      track.style.width = `${pageCount * w}px`
      for (const child of Array.from(track.children) as HTMLElement[]) {
        child.style.flex = `0 0 ${w}px`
        child.style.width = `${w}px`
      }
      const i = holdIdx.current ?? animTarget.current ?? indexRef.current
      if (prevW <= 0 || Math.abs(prevW - w) > 0.5) {
        if (animTarget.current === null) {
          applyFrac(i, false)
          holdIdx.current = i
        } else {
          applyFrac(progressRef.current, false)
        }
      } else {
        applyFrac(progressRef.current, ptrPhase.current === 'dragging')
      }
    }
    measure()
    const ro = new ResizeObserver(measure)
    ro.observe(vp)
    return () => ro.disconnect()
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [progressHostRef, pageCount])

  useLayoutEffect(() => {
    if (skipPropScroll.current) {
      skipPropScroll.current = false
      return
    }
    if (widthRef.current <= 0) return
    if (
      Math.abs(progressRef.current - index) < 0.002
      && animTarget.current === null
      && holdIdx.current === index
    ) {
      applyFrac(index, false)
      return
    }
    if (Math.abs(progressRef.current - index) < 0.002 && animTarget.current === null) {
      applyFrac(index, false)
      holdIdx.current = index
      return
    }
    pagerLog(
      `[pager] prop→settle →${fmtTarget(index)} at ${fmtX(progressRef.current)} seq=${navSeq}`,
    )
    settleToRef.current(index, `prop/navSeq=${navSeq}`)
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [index, navSeq])

  useEffect(() => {
    const vp = viewportRef.current
    if (!vp) return

    const sampleVel = (frac: number) => {
      const t = performance.now()
      const s = velSamples.current
      s.push({ t, frac })
      if (s.length > 6) s.shift()
    }

    const readVel = (): number => {
      const s = velSamples.current
      if (s.length < 2) return 0
      const a = s[0]
      const b = s[s.length - 1]
      const dt = b.t - a.t
      if (dt < 8 || dt > NAV.pagerFlingStaleMs * 3) return 0
      return (b.frac - a.frac) / dt
    }

    const beginDrag = (clientX: number) => {
      if (ptrPhase.current === 'dragging') return
      ptrPhase.current = 'dragging'
      holdIdx.current = null
      velSamples.current = []

      const was = freezeSettle()
      const frac = progressRef.current
      dragOriginFrac.current = frac
      dragStartClientX.current = clientX

      if (was !== null) {
        pagerLog(`[pager] drag-grab ${fmtX(frac)} was→${fmtTarget(was)}`)
      } else {
        pagerLog(`[pager] drag-start ${fmtX(frac)}`)
      }
      logProgress('finger', true)
    }

    const onPointerDown = (e: PointerEvent) => {
      if (ptrPhase.current !== 'none') return
      ptrPhase.current = 'pending'
      ptrStartX.current = e.clientX
      ptrStartY.current = e.clientY
      ptrId.current = e.pointerId
      try {
        vp.setPointerCapture(e.pointerId)
      } catch { /* ignore */ }
      pagerLog(
        `[pager] pointer↓ pending ${fmtX(progressRef.current)}`
        + (animTarget.current !== null ? ` settle→${fmtTarget(animTarget.current)}` : ''),
      )
      logProgress('pointer-down', true)
    }

    const onPointerMove = (e: PointerEvent) => {
      if (ptrId.current !== null && e.pointerId !== ptrId.current) return

      if (ptrPhase.current === 'pending') {
        const adx = Math.abs(e.clientX - ptrStartX.current)
        const ady = Math.abs(e.clientY - ptrStartY.current)
        const axis = resolvePagerAxis(adx, ady)
        if (axis === 'none') return
        if (axis === 'v') {
          ptrPhase.current = 'none'
          ptrId.current = null
          try {
            vp.releasePointerCapture(e.pointerId)
          } catch { /* ignore */ }
          pagerLog(`[pager] tap-ignore axis=v ${fmtX(progressRef.current)}`)
          return
        }
        beginDrag(e.clientX)
      }

      if (ptrPhase.current !== 'dragging') return
      e.preventDefault()
      const w = widthRef.current
      if (w <= 0) return
      const dxPx = e.clientX - dragStartClientX.current
      const raw = dragOriginFrac.current - dxPx / w
      const frac = rubberBandPage(raw, pageCount)
      applyFrac(frac, true)
      sampleVel(frac)
      logProgress('tick')
    }

    const endPointer = (e: PointerEvent, reason: 'up' | 'cancel') => {
      if (ptrId.current !== null && e.pointerId !== ptrId.current) return
      const phase = ptrPhase.current
      ptrPhase.current = 'none'
      ptrId.current = null
      try {
        vp.releasePointerCapture(e.pointerId)
      } catch { /* ignore */ }

      if (phase === 'pending') {
        pagerLog(`[pager] tap-ignore ${fmtX(progressRef.current)} reason=${reason}`)
        logProgress('tap-ignore', true)
        return
      }
      if (phase !== 'dragging') return

      const frac = progressRef.current
      const vel = readVel()
      const target = resolveReleaseTarget(frac, vel, pageCount, dragOriginFrac.current)
      pagerLog(
        `[pager] finger↑ ${fmtX(frac)} vel=${vel.toFixed(4)} →${fmtTarget(target)}`,
      )
      logProgress('finger-up', true)
      settleToRef.current(target, 'finger↑')
    }

    const onPointerUp = (e: PointerEvent) => endPointer(e, 'up')
    const onPointerCancel = (e: PointerEvent) => endPointer(e, 'cancel')

    vp.addEventListener('pointerdown', onPointerDown, { passive: true })
    vp.addEventListener('pointermove', onPointerMove, { passive: false })
    vp.addEventListener('pointerup', onPointerUp, { passive: true })
    vp.addEventListener('pointercancel', onPointerCancel, { passive: true })
    pagerLog(`[pager] ready axis ${axisLegend()} mode=maa-single-owner`)
    pagerLog(`[pager] ready at ${fmtX(indexRef.current)} pages=${pageCount}`)
    if (widthRef.current > 0) {
      applyFrac(indexRef.current, false)
      holdIdx.current = indexRef.current
    }

    return () => {
      vp.removeEventListener('pointerdown', onPointerDown)
      vp.removeEventListener('pointermove', onPointerMove)
      vp.removeEventListener('pointerup', onPointerUp)
      vp.removeEventListener('pointercancel', onPointerCancel)
      clearSettleRaf()
      animTarget.current = null
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [pageCount, progressHostRef])

  return (
    <div
      ref={viewportRef}
      className="flex-1 min-h-0 overflow-hidden"
      data-page-pager
      style={{
        touchAction: 'pan-y',
        overscrollBehaviorX: 'none',
        WebkitUserSelect: 'none',
        userSelect: 'none',
      }}
    >
      <div
        ref={trackRef}
        className="flex h-full will-change-transform"
        style={{
          transform: 'translate3d(0,0,0)',
          backfaceVisibility: 'hidden',
        }}
      >
        {panels.map((panel, i) => (
          <div
            key={PRIMARY_PAGES[i] ?? i}
            className="h-full shrink-0 flex flex-col min-h-0 overflow-hidden"
            aria-hidden={i !== index}
          >
            {panel}
          </div>
        ))}
      </div>
    </div>
  )
}
