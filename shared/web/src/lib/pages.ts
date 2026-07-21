/** Primary app pages — same IA on desktop side-nav and mobile bottom-nav. */
export type AppPage = 'Monitor' | 'Peers' | 'Log' | 'Settings' | 'DevTools'

export const PRIMARY_PAGES: AppPage[] = ['Monitor', 'Peers', 'Log', 'Settings']

export function isPrimaryPage(p: AppPage): boolean {
  return p !== 'DevTools'
}

/** Index into PRIMARY_PAGES (0…3); DevTools maps to Settings. */
export function pageIndex(page: AppPage): number {
  if (page === 'DevTools') return PRIMARY_PAGES.indexOf('Settings')
  const i = PRIMARY_PAGES.indexOf(page)
  return i < 0 ? 0 : i
}

/**
 * Pager / pill axis: blank(0) | content(1…N) | blank(N+1).
 * contentIndex 0 → axis 1 (Monitor).
 */
export function pageAxisX(page: AppPage): number {
  return pageIndex(page) + 1
}

/** @deprecated use pageAxisX / scrollLeft/width; kept for any legacy callers */
export function fractionalPageIndex(index: number, dragPx: number, width: number): number {
  if (width <= 0) return index
  return index - dragPx / width
}
