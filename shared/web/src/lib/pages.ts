/** Primary app pages — same IA on desktop side-nav and mobile bottom-nav. */
export type AppPage = 'Monitor' | 'Peers' | 'Log' | 'Settings' | 'DevTools'

export const PRIMARY_PAGES: AppPage[] = ['Monitor', 'Peers', 'Log', 'Settings']

export function isPrimaryPage(p: AppPage): boolean {
  return p !== 'DevTools'
}

/** Index into PRIMARY_PAGES; DevTools maps to Settings for nav highlight. */
export function pageIndex(page: AppPage): number {
  if (page === 'DevTools') return PRIMARY_PAGES.indexOf('Settings')
  const i = PRIMARY_PAGES.indexOf(page)
  return i < 0 ? 0 : i
}

/** Fractional page index for synced pager + bottom-nav pill. */
export function fractionalPageIndex(index: number, dragPx: number, width: number): number {
  if (width <= 0) return index
  return index - dragPx / width
}
