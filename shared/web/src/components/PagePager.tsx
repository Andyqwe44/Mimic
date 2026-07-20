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
  const syncingFromProp = useRef(false)
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
  useLayoutEffect(() => {
    const el = scrollerRef.current
    if (!el) return
    const w = el.clientWidth
    if (w <= 0) return
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
      const w = el.clientWidth
      if (w <= 0) return
      const next = Math.max(0, Math.min(PRIMARY_PAGES.length - 1, Math.round(el.scrollLeft / w)))
      writeNavProgress(progressHost(), next, false)
      if (syncingFromProp.current) return
      if (next !== indexRef.current) {
        onPageChangeRef.current(PRIMARY_PAGES[next])
      }
    }

    const onScroll = () => {
      const w = el.clientWidth
      if (w <= 0) return
      writeNavProgress(progressHost(), el.scrollLeft / w, true)
      // Android WebView may lack scrollend — debounce settle.
      window.clearTimeout(settleTimer.current)
      settleTimer.current = window.setTimeout(() => {
        syncingFromProp.current = false
        commitPage()
      }, 80) as unknown as number
    }

    const onScrollEnd = () => {
      window.clearTimeout(settleTimer.current)
      syncingFromProp.current = false
      commitPage()
    }

    el.addEventListener('scroll', onScroll, { passive: true })
    el.addEventListener('scrollend', onScrollEnd)
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
