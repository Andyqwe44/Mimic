// Display helpers for target window titles (C++ keeps English " Entire Desktop").
import type { TFunction } from 'i18next'

/** Canonical desktop title from list_windows (leading space intentional). */
export const DESKTOP_TITLE = ' Entire Desktop'

export function isDesktopTitle(title: string | undefined): boolean {
  return !!title && title.trimStart().startsWith('Entire Desktop')
}

/** Localized label for UI; OS window titles pass through unchanged. */
export function displayTargetTitle(title: string, t: TFunction): string {
  const trimmed = title.trimStart()
  if (!trimmed.startsWith('Entire Desktop')) return title
  const m = trimmed.match(/\(D(\d+)\)/)
  if (m) return t('connection.entire_desktop_n', { n: m[1] })
  return t('connection.entire_desktop')
}
