// Horizontal swipe / drag between primary pages (phone bottom-nav mode).
import { useCallback, useEffect, useRef, useState } from 'react'
import { PRIMARY_PAGES, type AppPage } from '../lib/pages'

const MIN_DX = 56
const MAX_SLOPE = 0.65 // |dy|/|dx| — reject mostly-vertical scrolls

function pageIndex(page: AppPage): number {
  if (page === 'DevTools') return PRIMARY_PAGES.indexOf('Settings')
  const i = PRIMARY_PAGES.indexOf(page)
  return i < 0 ? 0 : i
}

function blockedTarget(t: EventTarget | null): boolean {
  if (!(t instanceof Element)) return false
  return !!t.closest(
    'input, textarea, select, [contenteditable="true"], canvas, [data-no-page-swipe]',
  )
}

/** Callback ref for the page column; only active when `enabled` (bottom nav). */
export function usePageSwipe(
  enabled: boolean,
  page: AppPage,
  setPage: (p: AppPage) => void,
): (node: HTMLElement | null) => void {
  const [el, setEl] = useState<HTMLElement | null>(null)
  const start = useRef<{ x: number; y: number; blocked: boolean; id: number } | null>(null)
  const pageRef = useRef(page)
  pageRef.current = page
  const setPageRef = useRef(setPage)
  setPageRef.current = setPage

  const ref = useCallback((node: HTMLElement | null) => {
    setEl(node)
  }, [])

  useEffect(() => {
    if (!el || !enabled) return

    const onDown = (e: PointerEvent) => {
      if (e.pointerType === 'mouse' && e.button !== 0) return
      start.current = {
        x: e.clientX,
        y: e.clientY,
        blocked: blockedTarget(e.target),
        id: e.pointerId,
      }
    }

    const onUp = (e: PointerEvent) => {
      const s = start.current
      start.current = null
      if (!s || s.blocked || s.id !== e.pointerId) return
      const dx = e.clientX - s.x
      const dy = e.clientY - s.y
      if (Math.abs(dx) < MIN_DX) return
      if (Math.abs(dy) > Math.abs(dx) * MAX_SLOPE) return

      const i = pageIndex(pageRef.current)
      const next = dx < 0 ? i + 1 : i - 1
      if (next < 0 || next >= PRIMARY_PAGES.length) return
      setPageRef.current(PRIMARY_PAGES[next])
    }

    const onCancel = () => {
      start.current = null
    }

    el.addEventListener('pointerdown', onDown)
    el.addEventListener('pointerup', onUp)
    el.addEventListener('pointercancel', onCancel)
    return () => {
      el.removeEventListener('pointerdown', onDown)
      el.removeEventListener('pointerup', onUp)
      el.removeEventListener('pointercancel', onCancel)
    }
  }, [el, enabled])

  return ref
}
