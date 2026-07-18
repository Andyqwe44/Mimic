/** Primary app pages — same IA on desktop side-nav and mobile bottom-nav. */
export type AppPage = 'Monitor' | 'Control' | 'Log' | 'Settings' | 'DevTools'

export const PRIMARY_PAGES: AppPage[] = ['Monitor', 'Control', 'Log', 'Settings']

export function isPrimaryPage(p: AppPage): boolean {
  return p !== 'DevTools'
}
